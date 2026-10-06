// SPDX-License-Identifier: GPL-2.0-or-later

#include "core_process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <aaudio/AAudio.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

namespace Protocol = Core::Vr::Protocol;

namespace {

void MakeDirs(const std::string& path) {
    for (size_t pos = 1; pos <= path.size(); ++pos) {
        if (pos == path.size() || path[pos] == '/') {
            ::mkdir(path.substr(0, pos).c_str(), 0700);
        }
    }
}

int Listen(const std::string& path, int type) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        LOGE("socket path too long: %s", path.c_str());
        return -1;
    }
    std::strcpy(address.sun_path, path.c_str());
    ::unlink(path.c_str());

    const int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGE("socket(%s): %s", path.c_str(), std::strerror(errno));
        return -1;
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 4) != 0) {
        LOGE("bind/listen(%s): %s", path.c_str(), std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Waits for a connection without blocking forever, so the thread can notice a shutdown.
int Accept(int listen_fd, const std::atomic<bool>& quit) {
    while (!quit) {
        pollfd poll_fd{.fd = listen_fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&poll_fd, 1, 200);
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
        if (ready > 0) {
            const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) {
                return fd;
            }
        }
    }
    return -1;
}

bool ReadFully(int fd, void* data, size_t size) {
    auto* bytes = static_cast<uint8_t*>(data);
    while (size > 0) {
        const ssize_t count = ::recv(fd, bytes, size, 0);
        if (count == 0 || (count < 0 && errno != EINTR)) {
            return false;
        }
        if (count > 0) {
            bytes += count;
            size -= static_cast<size_t>(count);
        }
    }
    return true;
}

//// The output stream of one audio port of the guest.
///
/// Nothing here waits inside the audio system: a write that blocks does so for as long as the
/// output takes, and an output that is suspended (the headset asleep, the route changing) takes
/// for ever. Writes only ever put in what there is room for; the waiting is done here, and it
/// ends.
class AudioOutput {
public:
    ~AudioOutput() {
        Close();
    }

    void Open(uint32_t sample_rate) {
        Close();
        rate = sample_rate;
        opened = Clock::now();
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
            return;
        }
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sample_rate));
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        if (AAudioStreamBuilder_openStream(builder, &stream) != AAUDIO_OK) {
            LOGE("unable to open an audio stream at %u Hz", sample_rate);
            stream = nullptr;
        } else {
            static std::atomic<int> streams{0};
            id = ++streams;
            // The stream takes sound out of its buffer a "burst" at a time. What waits in the
            // buffer is the cushion for an emulator that delivers a few milliseconds at a time
            // and is sometimes late; it is also delay. Two bursts are the least that plays
            // without gaps at all; a third is what a delay of the emulator may use up before
            // it is heard (the emulator makes up for it afterwards and fills the buffer again).
            burst = std::max(AAudioStream_getFramesPerBurst(stream), 1);
            const int32_t wanted =
                std::max(burst * 2, static_cast<int32_t>(sample_rate * 3 / 50));
            const int32_t set = AAudioStream_setBufferSizeInFrames(stream, wanted);
            const int32_t size = set > 0 ? set : AAudioStream_getBufferSizeInFrames(stream);
            // Room is made a burst at a time: several bursts is as long as a write can take
            // while the stream is alive, also when it has to wake the speakers up first.
            patience = std::chrono::nanoseconds{std::clamp<int64_t>(
                int64_t{burst} * 8'000'000'000LL / sample_rate, 100'000'000, 300'000'000)};
            underruns = 0;
            reported_underruns = 0;
            stalled = false;
            const aaudio_result_t started = AAudioStream_requestStart(stream);
            // The system only starts to play a stream once its buffer has been filled to that
            // size, the first time and after every longer gap: nothing may be held back from it
            // before that, or it stays silent for good.
            LOGI("sound %d: stream opened at %u Hz, bursts of %d frames, %d of %d frames of "
                 "buffer in use (start: %d)",
                 id, sample_rate, burst, size, AAudioStream_getBufferCapacityInFrames(stream),
                 static_cast<int>(started));
        }
        AAudioStreamBuilder_delete(builder);
        reported = Clock::now();
    }

    /// Plays `frames` frames of 16-bit stereo. Taking as long as the sound lasts is what paces
    /// the guest's audio thread; a stream that does not play must not hold the game back,
    /// though: what it has no room for is then dropped and the emulator keeps time by its own
    /// clock.
    void Play(const uint8_t* data, int32_t frames) {
        if (stream == nullptr) {
            // The stream was lost: try for a new one now and then.
            if (rate != 0 && Clock::now() - opened > std::chrono::seconds{1}) {
                Open(rate);
            }
            if (stream == nullptr) {
                return;
            }
        }

        // (After running dry the buffer fills up again by itself: the emulator hands over what
        // it was late with as fast as there is room for it.)
        underruns = AAudioStream_getXRunCount(stream);

        // What fits goes in at once; the rest waits here for the stream to make room, which
        // is the wait that keeps the game's sound in step with the speakers.
        const auto begun = Clock::now();
        int32_t left = frames;
        while (left > 0) {
            const aaudio_result_t result = AAudioStream_write(stream, data, left, 0);
            if (result < 0) {
                // Typically the output device went away (headphones, a route change).
                LOGW("sound %d: stream error %d, opening a new one", id, static_cast<int>(result));
                Close();
                return;
            }
            data += static_cast<size_t>(result) * 4;
            left -= result;
            if (left == 0 || stalled) {
                break;
            }
            if (Clock::now() - begun >= patience) {
                stalled = true;
                read_at_stall = AAudioStream_getFramesRead(stream);
                if (++stalls <= 5 || stalls % 100 == 0) {
                    LOGW("sound %d: the stream takes nothing (%s, %lld frames played so far): "
                         "dropping sound until it does (%d times so far)",
                         id, AAudio_convertStreamStateToText(AAudioStream_getState(stream)),
                         static_cast<long long>(read_at_stall), stalls);
                }
                break;
            }
            ::usleep(1'000);
        }
        taken += frames - left;
        dropped += left;
        // A stream that takes sound out of its buffer again is back.
        if (stalled && AAudioStream_getFramesRead(stream) != read_at_stall) {
            stalled = false;
            if (stalls <= 5 || stalls % 100 == 0) {
                LOGI("sound %d: the stream plays again", id);
            }
        }
        Report();
    }

private:
    using Clock = std::chrono::steady_clock;

    /// Every ten seconds: whether what the emulator delivers ends up at the speakers.
    void Report() {
        const auto now = Clock::now();
        if (now - reported < std::chrono::seconds{10}) {
            return;
        }
        const int64_t written = AAudioStream_getFramesWritten(stream);
        const int64_t played = AAudioStream_getFramesRead(stream);
        // (The stream counts its dry runs from when it was opened.)
        LOGI("sound %d: %.1f s delivered, %.1f s dropped, %.1f s played by the stream in %.1f s; "
             "ran dry %d times in that time (%d since it was opened); %lld frames waiting, "
             "stream %s",
             id, static_cast<double>(taken) / rate, static_cast<double>(dropped) / rate,
             static_cast<double>(played - reported_played) / rate,
             std::chrono::duration<double>(now - reported).count(), underruns - reported_underruns,
             underruns, static_cast<long long>(written - played),
             AAudio_convertStreamStateToText(AAudioStream_getState(stream)));
        reported_underruns = underruns;
        taken = 0;
        dropped = 0;
        reported_played = played;
        reported = now;
    }

    void Close() {
        if (stream != nullptr) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = nullptr;
        }
        reported_played = 0;
    }

    AAudioStream* stream{};
    int id{};
    uint32_t rate{};
    int32_t burst{1};
    int32_t underruns{};
    int32_t reported_underruns{};
    /// How long a buffer may wait for room; whether the stream has been given up on for now,
    /// and how much it had played when that was decided.
    std::chrono::nanoseconds patience{200'000'000};
    bool stalled{};
    int64_t read_at_stall{};
    int32_t stalls{};
    /// For the log: frames the stream took and frames it had no room for since the last line.
    int64_t taken{};
    int64_t dropped{};
    int64_t reported_played{};
    Clock::time_point reported{};
    Clock::time_point opened{};
};

/// The headset's microphone, for a port the guest listens on.
///
/// As with the output, nothing here waits inside the audio system: reads only take what has
/// been heard so far.
class AudioInput {
public:
    ~AudioInput() {
        Close();
    }

    bool Open(uint32_t sample_rate) {
        Close();
        // The headset's microphone is a single channel; two are only asked for should that
        // ever be refused.
        for (const int32_t count : {1, 2}) {
            AAudioStreamBuilder* builder = nullptr;
            if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
                return false;
            }
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(builder, count);
            AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sample_rate));
            // What the game listens for is somebody blowing, which is noise: the input that
            // does without noise suppression and automatic gain.
            AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
            error = AAudioStreamBuilder_openStream(builder, &stream);
            AAudioStreamBuilder_delete(builder);
            if (error == AAUDIO_OK) {
                channels = count;
                break;
            }
            stream = nullptr;
        }
        if (stream == nullptr) {
            return false;
        }
        error = AAudioStream_requestStart(stream);
        if (error != AAUDIO_OK) {
            Close();
            return false;
        }
        return true;
    }

    bool IsOpen() const {
        return stream != nullptr;
    }

    /// Why the last Open failed.
    int LastError() const {
        return static_cast<int>(error);
    }

    /// What the stream is doing, in the audio system's words.
    const char* State() const {
        return stream != nullptr ? AAudio_convertStreamStateToText(AAudioStream_getState(stream))
                                 : "closed";
    }

    /// Up to `capacity` frames of 16-bit stereo heard since the last call; negative once the
    /// stream is lost.
    int32_t Read(int16_t* stereo, int32_t capacity) {
        if (channels == 2) {
            return AAudioStream_read(stream, stereo, capacity, 0);
        }
        mono.resize(static_cast<size_t>(capacity));
        const int32_t frames = AAudioStream_read(stream, mono.data(), capacity, 0);
        for (int32_t frame = 0; frame < frames; ++frame) {
            stereo[frame * 2] = mono[static_cast<size_t>(frame)];
            stereo[frame * 2 + 1] = mono[static_cast<size_t>(frame)];
        }
        return frames;
    }

    void Close() {
        if (stream != nullptr) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = nullptr;
        }
    }

private:
    AAudioStream* stream{};
    int32_t channels{1};
    aaudio_result_t error{AAUDIO_OK};
    std::vector<int16_t> mono;
};

double Decibels(double level) {
    return level > 0.0 ? 20.0 * std::log10(level) : -120.0;
}

/// A microphone port of the guest: what the headset hears goes down the socket as 16-bit
/// stereo, as it comes. The guest keeps its own time and takes silence for what does not
/// arrive, so nothing here has to be on time, or to happen at all.
void ServeMicrophone(int fd, uint32_t sample_rate, const MicrophoneSettings& settings) {
    using Clock = std::chrono::steady_clock;
    AudioInput input;
    Clock::time_point tried{};
    bool reported_failure = false;
    std::vector<int16_t> samples(static_cast<size_t>(sample_rate / 50) * 2);

    // What the guest was sent, for the log: the game makes "how hard is somebody blowing" of
    // the loudness of each 256 frames, so that is the measure.
    constexpr size_t BlockSamples = 512;
    double block_squares = 0.0;
    size_t block_filled = 0;
    double loudest = 0.0;
    double squares = 0.0;
    uint64_t counted = 0;
    uint64_t dropped = 0;
    uint64_t heard = 0;
    auto reported = Clock::now();

    LOGI("the game listens to the microphone (%u Hz)%s", sample_rate,
         !settings.enabled ? ", which is switched off (mic=0)"
                           : settings.test_signal ? ": a test signal stands in for it" : "");
    const auto started = Clock::now();
    uint64_t generated = 0;
    bool was_made = false;
    bool blow_reported = false;
    uint32_t noise = 0x2545f491;
    while (true) {
        // The guest says nothing more on this connection; it ends by closing it.
        char discard[64];
        const ssize_t received = ::recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                              errno != EINTR)) {
            break;
        }
        // Blowing with buttons is heard whatever the microphone is, or is allowed to be.
        const bool blown =
            settings.blowing != nullptr && settings.blowing->load(std::memory_order_relaxed);
        if (blown && !blow_reported) {
            blow_reported = true;
            LOGI("microphone: the player blows with buttons, the game hears that instead");
        }
        if (!settings.enabled && !blown) {
            was_made = false;
            ::usleep(50'000);
            continue;
        }

        const auto now = Clock::now();
        if (now - reported >= std::chrono::seconds{10}) {
            if (counted != 0) {
                LOGI("microphone: loudest moment %.1f dB, average %.1f dB (0 dB is full scale; "
                     "the game takes -9 dB for blowing as hard as it gets, -21 dB for half of "
                     "that)%s",
                     Decibels(loudest), Decibels(std::sqrt(squares / counted)),
                     dropped != 0 ? ", some of it not taken by the emulator" : "");
            } else if (input.IsOpen()) {
                // An app nobody looks at is not given the microphone, for one.
                LOGI("microphone: nothing came from it in the last ten seconds (stream %s, %llu "
                     "frames before that)",
                     input.State(), static_cast<unsigned long long>(heard));
                // A stream opened while the headset slept has been seen to stay like that;
                // a new one costs nothing.
                input.Close();
            }
            heard += counted / 2;
            loudest = 0.0;
            squares = 0.0;
            counted = 0;
            dropped = 0;
            reported = now;
        }
        int32_t frames = 0;
        const bool made = settings.test_signal || blown;
        if (!made) {
            was_made = false;
        }
        if (made) {
            // By the clock, a fiftieth of a second at a time.
            const auto due = static_cast<uint64_t>(
                std::chrono::duration<double>(now - started).count() * sample_rate);
            if (!was_made) {
                // (Nothing is owed for the time the microphone spoke for itself.)
                was_made = true;
                generated = due;
            }
            if (due - generated < samples.size() / 2) {
                ::usleep(2'000);
                continue;
            }
            frames = static_cast<int32_t>(samples.size() / 2);
            // Evenly distributed noise of this amplitude has a root mean square of 0.35.
            const bool blowing = blown || generated % (5ull * sample_rate) < sample_rate;
            for (size_t i = 0; i < samples.size(); ++i) {
                noise ^= noise << 13;
                noise ^= noise >> 17;
                noise ^= noise << 5;
                const float unit = static_cast<float>(noise >> 8) / 8388608.0f - 1.0f;
                samples[i] = blowing ? static_cast<int16_t>(unit * 0.606f * 32767.0f) : 0;
            }
            generated += static_cast<uint64_t>(frames);
        } else if (!input.IsOpen()) {
            // Without the permission there is no stream. The user may be looking at the
            // question right now, so it is tried again.
            if (now - tried < std::chrono::seconds{2}) {
                ::usleep(100'000);
                continue;
            }
            tried = now;
            if (!input.Open(sample_rate)) {
                if (!reported_failure) {
                    LOGW("the microphone cannot be opened (error %d); trying again every two "
                         "seconds",
                         input.LastError());
                    reported_failure = true;
                }
                continue;
            }
            LOGI("microphone opened");
            reported_failure = false;
        }

        if (!made) {
            frames = input.Read(samples.data(), static_cast<int32_t>(samples.size() / 2));
        }
        if (frames < 0) {
            LOGW("microphone stream error %d, opening a new one", static_cast<int>(frames));
            input.Close();
            continue;
        }
        if (frames == 0) {
            ::usleep(2'000);
            continue;
        }

        const size_t count = static_cast<size_t>(frames) * 2;
        // (What buttons blow is as loud as blowing gets, whatever the microphone needs.)
        const float gain = blown ? 1.0f : settings.gain;
        for (size_t i = 0; i < count; ++i) {
            const float amplified = static_cast<float>(samples[i]) * gain;
            samples[i] = static_cast<int16_t>(std::clamp(amplified, -32768.0f, 32767.0f));
            const double sample = samples[i] / 32768.0;
            block_squares += sample * sample;
            if (++block_filled == BlockSamples) {
                loudest = std::max(loudest, std::sqrt(block_squares / BlockSamples));
                squares += block_squares;
                counted += BlockSamples;
                block_squares = 0.0;
                block_filled = 0;
            }
        }
        // A guest that is not reading (paused, busy) does not get the sound later instead.
        const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
        const size_t size = count * sizeof(int16_t);
        const ssize_t sent = ::send(fd, bytes, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        if (sent < static_cast<ssize_t>(size)) {
            ++dropped;
            // What does get through has to end with a whole frame, or everything after it
            // would be read out of step: the few bytes a cut frame lacks are waited for.
            constexpr size_t FrameBytes = 2 * sizeof(int16_t);
            const size_t cut = sent > 0 ? static_cast<size_t>(sent) % FrameBytes : 0;
            if (cut != 0 && ::send(fd, bytes + sent, FrameBytes - cut, MSG_NOSIGNAL) !=
                                static_cast<ssize_t>(FrameBytes - cut)) {
                break;
            }
        }
    }
    ::close(fd);
}

/// One audio port of the guest: a framed stream of "prepare" and "PCM" requests.
void ServeAudio(int fd, MicrophoneSettings microphone) {
    static constexpr uint8_t RequestPrepare = 4;
    static constexpr uint8_t RequestWrite = 5;
    static constexpr uint8_t RequestCapture = 6;

    // The emulator's busiest threads run at a raised priority and there are more of them than
    // processor cores: whoever carries the sound to the speakers must not queue behind them.
    ::setpriority(PRIO_PROCESS, 0, -16);

    AudioOutput output;
    std::vector<uint8_t> payload;
    while (true) {
        uint8_t header[5];
        if (!ReadFully(fd, header, sizeof(header))) {
            break;
        }
        uint32_t size = 0;
        std::memcpy(&size, header + 1, sizeof(size));
        if (size > (1u << 24)) {
            break;
        }
        payload.resize(size);
        if (size > 0 && !ReadFully(fd, payload.data(), size)) {
            break;
        }

        if (header[0] == RequestPrepare && size >= 10) {
            // channels, sample type, rate, buffer size. The core always sends 16-bit stereo.
            uint32_t sample_rate = 0;
            std::memcpy(&sample_rate, payload.data() + 2, sizeof(sample_rate));
            output.Open(sample_rate);
        } else if (header[0] == RequestWrite) {
            output.Play(payload.data(), static_cast<int32_t>(size / 4));
        } else if (header[0] == RequestCapture && size >= 10) {
            // The connection is for the other direction from here on.
            uint32_t sample_rate = 0;
            std::memcpy(&sample_rate, payload.data() + 2, sizeof(sample_rate));
            if (sample_rate >= 8000 && sample_rate <= 96000) {
                ServeMicrophone(fd, sample_rate, microphone);
                return;
            }
            break;
        }
    }
    ::close(fd);
}

} // namespace

CoreProcess::~CoreProcess() {
    Stop();
}

std::string CoreProcess::GetMessage() {
    std::scoped_lock lock{mutex};
    return message;
}

void CoreProcess::Fail(const std::string& text) {
    LOGE("%s", text.c_str());
    {
        std::scoped_lock lock{mutex};
        message = text;
    }
    state = State::Failed;
}

bool CoreProcess::Start(const CoreLaunch& launch) {
    const std::string run_dir = launch.storage_root + "/run";
    const std::string home = launch.storage_root + "/home";
    const std::string tmp = launch.storage_root + "/tmp";
    const std::string cache = launch.storage_root + "/cache";
    for (const auto& dir : {run_dir, home, tmp, cache, cache + "/mesa"}) {
        MakeDirs(dir);
    }

    const std::string control_path = run_dir + "/control.sock";
    const std::string audio_path = run_dir + "/audio.sock";
    const std::string vr_path = run_dir + "/vr.sock";
    control_listen = Listen(control_path, SOCK_STREAM);
    audio_listen = Listen(audio_path, SOCK_STREAM);
    vr_listen = Listen(vr_path, SOCK_SEQPACKET);
    if (control_listen < 0 || audio_listen < 0 || vr_listen < 0) {
        Fail("Unable to create the sockets the emulator connects to");
        return false;
    }

    const std::string host_dir = launch.runtime_root + "/host";
    const std::string driver_dir = launch.runtime_root + "/drivers/turnip";
    const std::string core = host_dir + "/shadps4-arm64-fex";
    // The Vulkan loader opens the driver by name, so its folder has to be searched too.
    const std::string library_path = host_dir + ":" + driver_dir;
    std::vector<std::string> args = {launch.loader, "--library-path", library_path};
    // Horizon OS only lets apps use the newer half of the GPU driver's memory interface, while
    // Turnip still uses the older one for some calls; this library translates between the two.
    // It has to go through the loader's own preload option: LD_PRELOAD would also be picked up
    // by Android programs the core starts.
    const std::string gpu_shim = host_dir + "/libkgsl_compat.so";
    if (::access(gpu_shim.c_str(), R_OK) == 0) {
        args.insert(args.end(), {"--preload", gpu_shim});
    } else {
        LOGW("%s is missing, the GPU driver will probably fail to allocate memory",
             gpu_shim.c_str());
    }
    args.insert(args.end(), {core, "--bachata-storage-root", launch.storage_root,
                             "--bachata-socket", control_path, "-g", launch.game});
    args.insert(args.end(), launch.extra_args.begin(), launch.extra_args.end());

    std::vector<std::pair<std::string, std::string>> env = {
        {"HOME", home},
        {"TMPDIR", tmp},
        {"XDG_CACHE_HOME", cache},
        {"XDG_CONFIG_HOME", home + "/.config"},
        {"XDG_DATA_HOME", home + "/.local/share"},
        {"MESA_SHADER_CACHE_DIR", cache + "/mesa"},
        {"LD_LIBRARY_PATH", library_path},
        // Android's seccomp filter kills processes that register rseq.
        {"GLIBC_TUNABLES", "glibc.pthread.rseq=0"},
        {"SDL_VIDEODRIVER", "dummy"},
        {"SDL_AUDIODRIVER", "dummy"},
        {"SDL_VULKAN_LIBRARY", host_dir + "/libvulkan.so.1"},
        {"VK_ICD_FILENAMES", driver_dir + "/freedreno_icd.aarch64.json"},
        {"SHADPS4_HEADLESS", "1"},
        {"SHADPS4_VR_SOCKET", vr_path},
        {"BACHATA_ALSA_SOCKET", audio_path},
    };
    for (const auto& entry : launch.extra_env) {
        bool replaced = false;
        for (auto& existing : env) {
            if (existing.first == entry.first) {
                existing.second = entry.second;
                replaced = true;
            }
        }
        if (!replaced) {
            env.push_back(entry);
        }
    }

    // Everything the child touches between fork and exec has to exist beforehand.
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env_strings;
    for (const auto& [name, value] : env) {
        env_strings.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (auto& entry : env_strings) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    std::string command;
    for (const auto& arg : args) {
        command += arg + " ";
    }
    LOGI("starting core: %s", command.c_str());

    state = State::Starting;
    quit = false;
    paused = false;
    const char* log_path = launch.log_file.c_str();
    const pid_t child = ::fork();
    if (child < 0) {
        Fail(std::string{"fork failed: "} + std::strerror(errno));
        return false;
    }
    if (child == 0) {
        const int log = ::open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log >= 0) {
            ::dup2(log, STDOUT_FILENO);
            ::dup2(log, STDERR_FILENO);
        }
        if (::chdir(launch.storage_root.c_str()) != 0) {
            ::_exit(126);
        }
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    pid = child;

    control_thread = std::thread{[this] { ControlLoop(); }};
    audio_thread = std::thread{[this] { AudioLoop(); }};
    vr_thread = std::thread{[this] { VrLoop(); }};
    wait_thread = std::thread{[this] { WaitLoop(); }};
    return true;
}

void CoreProcess::Stop() {
    if (state == State::Idle && pid < 0) {
        return;
    }
    quit = true;
    if (pid > 0) {
        // A stopped process does not see a request to terminate.
        ::kill(pid, SIGCONT);
        ::kill(pid, SIGTERM);
    }
    for (std::thread* thread : {&wait_thread, &control_thread, &audio_thread, &vr_thread}) {
        if (thread->joinable()) {
            thread->join();
        }
    }
    for (int* fd : {&control_listen, &audio_listen, &vr_listen}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    pid = -1;
    state = State::Idle;
}

void CoreProcess::WaitLoop() {
    int status = 0;
    auto asked_to_quit = std::chrono::steady_clock::time_point{};
    // Every half minute, what the emulator uses: its share of the processors tells whether it
    // runs or hangs, and the processors it is allowed on differ with how the app was started.
    auto reported = std::chrono::steady_clock::now();
    double reported_cpu = 0.0;
    uint64_t reported_frames = 0;
    while (true) {
        if (const auto now = std::chrono::steady_clock::now();
            now - reported >= std::chrono::seconds{30} && !paused && !quit) {
            const Usage usage = GetUsage();
            const double seconds = std::chrono::duration<double>(now - reported).count();
            const uint64_t frames = presented_frames;
            LOGI("emulator: %.0f%% of a processor on %s, %d MB, %.1f frames a second",
                 (usage.cpu_seconds - reported_cpu) * 100.0 / seconds,
                 usage.cpus.empty() ? "?" : usage.cpus.c_str(), usage.memory_mb,
                 static_cast<double>(frames - reported_frames) / seconds);
            reported = now;
            reported_cpu = usage.cpu_seconds;
            reported_frames = frames;
        }
        const pid_t child = pid;
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) {
            break;
        }
        if (quit) {
            // A core that ignores SIGTERM for two seconds is killed.
            const auto now = std::chrono::steady_clock::now();
            if (asked_to_quit == std::chrono::steady_clock::time_point{}) {
                asked_to_quit = now;
            } else if (now - asked_to_quit > std::chrono::seconds{2}) {
                ::kill(pid, SIGKILL);
            }
        }
        ::usleep(50'000);
    }

    char text[128];
    if (WIFEXITED(status)) {
        std::snprintf(text, sizeof(text), "The emulator exited with code %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        std::snprintf(text, sizeof(text), "The emulator was stopped by signal %d",
                      WTERMSIG(status));
    } else {
        std::snprintf(text, sizeof(text), "The emulator stopped");
    }
    LOGI("%s", text);
    {
        std::scoped_lock lock{mutex};
        if (message.empty()) {
            message = text;
        } else {
            message = std::string{text} + " (" + message + ")";
        }
    }
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    state = clean || quit ? State::Stopped : State::Failed;
}

void CoreProcess::SetPaused(bool pause) {
    const pid_t child = pid;
    if (child <= 0 || paused.exchange(pause) == pause) {
        return;
    }
    LOGI("%s the emulator", pause ? "pausing" : "resuming");
    ::kill(child, pause ? SIGSTOP : SIGCONT);
}

pid_t CoreProcess::FindThread(const std::string& prefix) const {
    const auto threads = FindThreads(prefix);
    return threads.empty() ? 0 : threads.front();
}

std::vector<pid_t> CoreProcess::FindThreads(const std::string& prefix) const {
    std::vector<pid_t> found;
    const pid_t child = pid;
    if (child <= 0) {
        return found;
    }
    const std::string tasks = "/proc/" + std::to_string(child) + "/task";
    DIR* dir = ::opendir(tasks.c_str());
    if (dir == nullptr) {
        return found;
    }
    while (const dirent* entry = ::readdir(dir)) {
        const pid_t tid = static_cast<pid_t>(std::atoi(entry->d_name));
        if (tid <= 0) {
            continue;
        }
        char name[32]{};
        const std::string path = tasks + "/" + entry->d_name + "/comm";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        const ssize_t length = ::read(fd, name, sizeof(name) - 1);
        ::close(fd);
        // The kernel keeps the first 15 characters of a thread's name.
        if (length > 0 && prefix.compare(0, 15, name, std::min<size_t>(prefix.size(), 15)) == 0) {
            found.push_back(tid);
        }
    }
    ::closedir(dir);
    return found;
}

CoreProcess::Usage CoreProcess::GetUsage() const {
    Usage usage;
    const pid_t child = pid;
    if (child <= 0) {
        return usage;
    }
    const auto read_file = [](const std::string& path) {
        std::string text;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return text;
        }
        char chunk[4096];
        ssize_t count;
        while ((count = ::read(fd, chunk, sizeof(chunk))) > 0) {
            text.append(chunk, static_cast<size_t>(count));
        }
        ::close(fd);
        return text;
    };
    const std::string base = "/proc/" + std::to_string(child);

    // The name in the second field may hold anything, spaces included: count from its end.
    const std::string stat = read_file(base + "/stat");
    const size_t name_end = stat.rfind(')');
    if (name_end != std::string::npos) {
        unsigned long long user = 0;
        unsigned long long system = 0;
        // state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
        if (std::sscanf(stat.c_str() + name_end + 1,
                        " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user,
                        &system) == 2) {
            usage.cpu_seconds =
                static_cast<double>(user + system) / static_cast<double>(::sysconf(_SC_CLK_TCK));
        }
    }

    const std::string status = read_file(base + "/status");
    const auto field = [&](const char* name) {
        const size_t at = status.find(name);
        if (at == std::string::npos) {
            return std::string{};
        }
        size_t begin = at + std::strlen(name);
        while (begin < status.size() && (status[begin] == ' ' || status[begin] == '\t')) {
            ++begin;
        }
        const size_t end = status.find('\n', begin);
        return status.substr(begin, end == std::string::npos ? end : end - begin);
    };
    usage.cpus = field("Cpus_allowed_list:");
    usage.memory_mb = std::atoi(field("VmRSS:").c_str()) / 1024;
    return usage;
}

void CoreProcess::SetMicrophone(bool enabled, float gain, bool test_signal) {
    microphone.enabled = enabled;
    microphone.gain = gain;
    microphone.test_signal = test_signal;
}

void CoreProcess::SetPad(const PadState& new_pad) {
    std::scoped_lock lock{mutex};
    pad = new_pad;
}

void CoreProcess::ControlLoop() {
    const int fd = Accept(control_listen, quit);
    if (fd < 0) {
        return;
    }
    control_fd = fd;
    LOGI("core connected to the control socket");

    std::string line;
    PadState last_sent;
    bool sent_once = false;
    uint64_t sequence = 0;
    auto last_send = std::chrono::steady_clock::now();

    while (!quit) {
        pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&poll_fd, 1, 4);
        if (ready > 0) {
            char chunk[512];
            const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
            if (count == 0 || (count < 0 && errno != EINTR)) {
                break;
            }
            for (ssize_t i = 0; i < count; ++i) {
                if (chunk[i] != '\n') {
                    if (line.size() < 1024) {
                        line.push_back(chunk[i]);
                    }
                    continue;
                }
                if (line == "BACHATA/1 EVENT Frame") {
                    ++presented_frames;
                } else if (line == "BACHATA/1 EVENT Running") {
                    state = State::Running;
                    LOGI("core: running");
                } else if (line.rfind("BACHATA/1 ERROR code=", 0) == 0) {
                    std::scoped_lock lock{mutex};
                    message = "emulator error " + line.substr(21);
                    LOGE("core: %s", line.c_str());
                } else {
                    LOGI("core: %s", line.c_str());
                }
                line.clear();
            }
        }

        PadState current;
        {
            std::scoped_lock lock{mutex};
            current = pad;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool changed = !sent_once || !(current == last_sent);
        // Motion samples are a stream; everything else only needs sending when it changes, plus
        // a slow heartbeat.
        const auto interval = current.has_motion ? std::chrono::milliseconds{4}
                                                 : std::chrono::milliseconds{100};
        if (!changed && now - last_send < interval) {
            continue;
        }

        char text[512];
        int length = std::snprintf(
            text, sizeof(text),
            "BACHATA/1 INPUT slot=0 seq=%" PRIu64
            " buttons=0x%x lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u touch=%d tx=%u ty=%u",
            ++sequence, current.buttons, current.left_x, current.left_y, current.right_x,
            current.right_y, current.left_trigger, current.right_trigger,
            current.touch_down ? 1 : 0, current.touch_x, current.touch_y);
        if (current.has_motion) {
            const auto milli = [](float value) {
                const float scaled = value * 1000.0f;
                return static_cast<int>(scaled < -999999.0f ? -999999.0f
                                                             : scaled > 999999.0f ? 999999.0f
                                                                                  : scaled);
            };
            length += std::snprintf(text + length, sizeof(text) - length,
                                    " gx=%d gy=%d gz=%d ax=%d ay=%d az=%d",
                                    milli(current.gyro[0]), milli(current.gyro[1]),
                                    milli(current.gyro[2]), milli(current.accel[0]),
                                    milli(current.accel[1]), milli(current.accel[2]));
        }
        text[length++] = '\n';
        if (::send(fd, text, static_cast<size_t>(length), MSG_NOSIGNAL) < 0) {
            break;
        }
        last_sent = current;
        sent_once = true;
        last_send = now;
    }

    control_fd = -1;
    ::close(fd);
}

void CoreProcess::AudioLoop() {
    while (!quit) {
        const int fd = Accept(audio_listen, quit);
        if (fd < 0) {
            break;
        }
        // The guest opens and closes ports as it likes; each lives until its socket closes,
        // which happens at the latest when the core exits.
        MicrophoneSettings settings = microphone;
        settings.blowing = blowing;
        std::thread{ServeAudio, fd, settings}.detach();
    }
}

void CoreProcess::SetBuffers(const std::vector<int>& fds, uint32_t width, uint32_t height,
                             uint32_t stride) {
    std::scoped_lock lock{mutex};
    buffer_fds = fds;
    buffers = {};
    buffers.count = static_cast<uint32_t>(fds.size());
    buffers.width = width;
    buffers.height = height;
    buffers.stride = stride;
    buffers_sent = false;
    SendBuffersLocked();
}

void CoreProcess::SendBuffersLocked() {
    const int fd = vr_fd;
    if (fd < 0 || buffers_sent || buffer_fds.empty()) {
        return;
    }
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * Protocol::MaxBuffers)]{};
    iovec io{.iov_base = &buffers, .iov_len = sizeof(buffers)};
    msghdr header{};
    header.msg_iov = &io;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = CMSG_SPACE(sizeof(int) * buffer_fds.size());
    cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * buffer_fds.size());
    std::memcpy(CMSG_DATA(cmsg), buffer_fds.data(), sizeof(int) * buffer_fds.size());
    if (::sendmsg(fd, &header, MSG_NOSIGNAL) < 0) {
        LOGE("unable to offer the frame buffers: %s", std::strerror(errno));
        return;
    }
    buffers_sent = true;
    LOGI("offered %u frame buffers of %ux%u to the core", buffers.count, buffers.width,
         buffers.height);
}

void CoreProcess::SendPose(const Protocol::Pose& pose) {
    const int fd = vr_fd;
    if (fd >= 0) {
        ::send(fd, &pose, sizeof(pose), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void CoreProcess::SendPadPose(const Protocol::PadPose& pose) {
    const int fd = vr_fd;
    if (fd >= 0) {
        ::send(fd, &pose, sizeof(pose), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void CoreProcess::SendOptics(float ipd) {
    const int fd = vr_fd;
    if (fd >= 0) {
        Protocol::Optics optics;
        optics.ipd = ipd;
        ::send(fd, &optics, sizeof(optics), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void CoreProcess::SendRefresh(float rate) {
    const int fd = vr_fd;
    if (fd >= 0) {
        Protocol::Refresh refresh;
        refresh.rate = rate;
        // std::chrono::steady_clock is the clock the emulator reads too.
        refresh.time = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        ::send(fd, &refresh, sizeof(refresh), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

std::optional<Protocol::Frame> CoreProcess::TakeFrame() {
    std::scoped_lock lock{mutex};
    std::optional<Protocol::Frame> result;
    result.swap(frame);
    return result;
}

void CoreProcess::VrLoop() {
    ::setpriority(PRIO_PROCESS, 0, -12);
    const int fd = Accept(vr_listen, quit);
    if (fd < 0) {
        return;
    }
    LOGI("core connected to the VR socket");
    {
        std::scoped_lock lock{mutex};
        vr_fd = fd;
        SendBuffersLocked();
    }

    while (!quit) {
        pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&poll_fd, 1, 200);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        union {
            Protocol::Header header;
            Protocol::Frame frame;
            Protocol::PadFeedback feedback;
        } received{};
        const ssize_t size = ::recv(fd, &received, sizeof(received), 0);
        if (size == 0 || (size < 0 && errno != EINTR)) {
            break;
        }
        if (size < static_cast<ssize_t>(sizeof(Protocol::Header)) ||
            received.header.magic != Protocol::Magic) {
            continue;
        }
        if (received.header.type == Protocol::MessageType::Frame &&
            size == static_cast<ssize_t>(sizeof(Protocol::Frame))) {
            std::scoped_lock lock{mutex};
            frame = received.frame;
        } else if (received.header.type == Protocol::MessageType::PadFeedback &&
                   size == static_cast<ssize_t>(sizeof(Protocol::PadFeedback))) {
            const auto& feedback = received.feedback;
            pad_feedback = uint64_t{feedback.small_motor} | uint64_t{feedback.large_motor} << 8 |
                           uint64_t{feedback.red} << 16 | uint64_t{feedback.green} << 24 |
                           uint64_t{feedback.blue} << 32;
        }
    }

    vr_fd = -1;
    ::close(fd);
}
