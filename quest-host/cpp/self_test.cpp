// SPDX-License-Identifier: GPL-2.0-or-later

// Everything the host does with the emulator core except showing the result in the headset:
// starting it, the control socket (controller input), the frame buffers it shares with the core
// and the GL path that reads them. It runs from the SandboxShell instrumentation, so it needs
// nobody wearing the headset, and leaves pictures of what a headset would have been shown.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <sys/stat.h>
#include <zlib.h>

#include "core_process.h"
#include "gl_frames.h"
#include "held_pad.h"
#include "log.h"

namespace Protocol = Core::Vr::Protocol;

namespace {

void AppendChunk(std::vector<uint8_t>& png, const char* type, const uint8_t* data, size_t size) {
    const auto put32 = [&](uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<uint8_t>(value >> shift));
        }
    };
    put32(static_cast<uint32_t>(size));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data, data + size);
    put32(static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(4 + size))));
}

/// Writes RGBA pixels whose first row is the bottom one (as glReadPixels returns them) as an
/// opaque PNG, at half size: these are for looking at, not for measuring.
bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width,
              uint32_t height) {
    const uint32_t out_width = width / 2;
    const uint32_t out_height = height / 2;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(out_height) * (1 + out_width * 3));
    for (uint32_t y = 0; y < out_height; ++y) {
        raw.push_back(0); // filter: none
        const uint8_t* upper = rgba.data() + static_cast<size_t>(height - 1 - 2 * y) * width * 4;
        const uint8_t* lower = upper - static_cast<size_t>(width) * 4;
        for (uint32_t x = 0; x < out_width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const uint32_t sum = upper[x * 8 + channel] + upper[x * 8 + 4 + channel] +
                                     lower[x * 8 + channel] + lower[x * 8 + 4 + channel];
                raw.push_back(static_cast<uint8_t>(sum / 4));
            }
        }
    }
    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()),
                  3) != Z_OK) {
        return false;
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    uint8_t header[13] = {};
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<uint8_t>(out_width >> (24 - 8 * i));
        header[4 + i] = static_cast<uint8_t>(out_height >> (24 - 8 * i));
    }
    header[8] = 8; // bits per channel
    header[9] = 2; // RGB
    AppendChunk(png, "IHDR", header, sizeof(header));
    AppendChunk(png, "IDAT", compressed.data(), compressed_size);
    AppendChunk(png, "IEND", nullptr, 0);

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    std::fclose(file);
    return ok;
}

/// Stands in for the headset's compositor, which takes the GPU away from the emulator several
/// times every display refresh: a context of its own that, so many times a second, draws and
/// waits for the GPU to be done with it. Among the settings:
///   HOST_INTERRUPTS=<per second>          next to nothing is drawn each time
///   HOST_COMPOSITOR=<Hz>[,<n>[,<Mpx>]]    per refresh, a pass the size of the compositor's own
///                                         (9 megapixels unless told otherwise) and n - 1 times
///                                         next to nothing (n is 5 unless told otherwise: what
///                                         a session shows, with one layer and nothing else of
///                                         the system on screen)
class Interrupter {
public:
    ~Interrupter() {
        Stop();
    }

    void Start(const GlContext& main, int refreshes, int per_refresh, float megapixels) {
        if (refreshes > 0 && per_refresh > 0) {
            thread = std::thread{[this, display = main.display, config = main.config, refreshes,
                                  per_refresh,
                                  megapixels] { Run(display, config, refreshes, per_refresh, megapixels); }};
        }
    }

    void Stop() {
        quit = true;
        if (thread.joinable()) {
            thread.join();
        }
    }

    uint64_t Count() const {
        return count;
    }

    /// Milliseconds the large passes took on average since the last call.
    float TakePassTime() {
        const uint64_t passes = pass_count.exchange(0);
        const uint64_t time = pass_microseconds.exchange(0);
        return passes != 0 ? static_cast<float>(time) / static_cast<float>(passes) / 1000.0f : 0.0f;
    }

private:
    void Run(EGLDisplay display, EGLConfig config, int refreshes, int per_refresh,
             float megapixels) {
        using Clock = std::chrono::steady_clock;
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLContext context =
            eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        const EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(display, surface, surface, context)) {
            LOGE("self test: no context for the interruptions");
            return;
        }
        const auto make_target = [](GLsizei width, GLsizei height, GLuint& texture,
                                    GLuint& framebuffer) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture,
                                   0);
        };
        GLuint small = 0;
        GLuint small_framebuffer = 0;
        make_target(64, 64, small, small_framebuffer);
        // The compositor's pass: the game's picture, both eyes, drawn to the display's size.
        GLuint source = 0;
        GLuint source_framebuffer = 0;
        GLuint large = 0;
        GLuint large_framebuffer = 0;
        FrameBlitter blitter;
        const auto large_height = static_cast<GLsizei>(std::sqrt(megapixels * 1e6f / 1.87f));
        const auto large_width = static_cast<GLsizei>(static_cast<float>(large_height) * 1.87f);
        if (megapixels > 0.0f) {
            make_target(2880, 1536, source, source_framebuffer);
            make_target(large_width, large_height, large, large_framebuffer);
            blitter.Create();
        }

        const auto period = std::chrono::nanoseconds{1'000'000'000LL / refreshes};
        auto refresh = Clock::now();
        while (!quit) {
            for (int i = 0; i < per_refresh && !quit; ++i) {
                if (i == 0 && megapixels > 0.0f) {
                    const auto begun = Clock::now();
                    glBindFramebuffer(GL_FRAMEBUFFER, large_framebuffer);
                    blitter.Draw(source, static_cast<uint32_t>(large_width),
                                 static_cast<uint32_t>(large_height), false, false);
                    glFinish();
                    pass_microseconds += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begun)
                            .count());
                    ++pass_count;
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, small_framebuffer);
                    glClearColor(0.0f, static_cast<float>(count % 2), 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glFinish();
                }
                ++count;
                std::this_thread::sleep_until(refresh + period * (i + 1) / per_refresh);
            }
            refresh += period;
        }
        if (megapixels > 0.0f) {
            blitter.Destroy();
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
    }

    std::thread thread;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> pass_count{0};
    std::atomic<uint64_t> pass_microseconds{0};
};

std::string ToString(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result{chars != nullptr ? chars : ""};
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

std::string RunSelfTest(const CoreLaunch& launch, const std::string& out_dir, int seconds,
                        uint32_t eye_width, uint32_t eye_height, const std::string& microphone) {
    using Clock = std::chrono::steady_clock;
    const uint32_t width = eye_width * 2;
    const uint32_t height = eye_height;
    std::string report;
    const auto note = [&](const char* format, auto... values) {
        char line[512];
        std::snprintf(line, sizeof(line), format, values...);
        LOGI("self test: %s", line);
        report += line;
        report += '\n';
    };

    ::mkdir(out_dir.c_str(), 0700);
    SetLogFile((out_dir + "/host.log").c_str());
    GlContext gl;
    if (!gl.Create()) {
        return "no GL context\n";
    }
    note("GL: %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    CoreProcess core;
    FrameBuffers frames;
    FrameBlitter blitter;
    // "real" listens to the headset (which hears nothing while nobody wears it, but the path
    // is the one a session takes), "off" to nothing, anything else to the test signal.
    core.SetMicrophone(microphone != "off", 1.0f, microphone != "real");
    if (!core.Start(launch)) {
        gl.Destroy();
        return report + "the core did not start: " + core.GetMessage() + "\n";
    }
    if (!frames.Create(gl.display, width, height, core) || !blitter.Create()) {
        core.Stop();
        gl.Destroy();
        return report + "could not create the frame buffers\n";
    }

    // Stands in for the compositor's image the frame is copied to.
    GLuint target = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        note("%s", "the test framebuffer is incomplete");
    }
    // And one of the kind the compositor's images are, for timing the copy a session makes.
    GLuint display_target = 0;
    GLuint display_framebuffer = 0;
    glGenTextures(1, &display_target);
    glBindTexture(GL_TEXTURE_2D, display_target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
    glGenFramebuffers(1, &display_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, display_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, display_target,
                           0);
    note("frames are copied %s", blitter.WritesUnencoded() ? "as they are" : "through a decoder");

    Interrupter interrupter;
    bool fov_mask = false;
    for (const auto& [name, value] : launch.extra_env) {
        if (name == "HOST_FOV_MASK") {
            // Exercise the session's optical-axis mask in the isolated test and its captures.
            fov_mask = value == "1";
            note("optical-axis FOV mask: %s", fov_mask ? "on" : "off");
        } else if (name == "HOST_INTERRUPTS") {
            interrupter.Start(gl, std::atoi(value.c_str()), 1, 0.0f);
            note("taking the GPU away from the emulator %s times a second", value.c_str());
        } else if (name == "HOST_COMPOSITOR") {
            int refreshes = 0;
            int per_refresh = 5;
            float megapixels = 9.0f;
            std::sscanf(value.c_str(), "%d,%d,%f", &refreshes, &per_refresh, &megapixels);
            interrupter.Start(gl, refreshes, per_refresh, megapixels);
            note("a stand-in for the compositor: %d refreshes a second, the GPU taken %d times "
                 "in each, once for a pass of %.1f megapixels",
                 refreshes, per_refresh, megapixels);
        }
    }
    uint64_t reported_interruptions = 0;
    bool view_reset = false;

    // HOST_DISPLAY=<Hz> among the settings: the loop below goes round once for every refresh
    // of a display of that rate, the way a session's does, tells the emulator of each, and
    // counts for how many refreshes each of the game's frames stayed the newest (a game that
    // draws one frame for two refreshes should have every frame shown for two).
    float display_rate = 0.0f;
    for (const auto& [name, value] : launch.extra_env) {
        if (name == "HOST_DISPLAY") {
            display_rate = std::strtof(value.c_str(), nullptr);
            note("a display of %.2f refreshes a second", display_rate);
        }
    }
    uint32_t shown_for[5]{};
    uint32_t shown_since = 0;
    bool something_shown = false;

    // HOST_LOOK="<from second>:<degrees left>,<degrees up>;..." among the settings: where the
    // head looks from when on, in place of its swaying. "8:85,22;100:0,-12" finds the first
    // level of Astro Bot from the world map (the planets are picked by looking at them) and
    // then looks along it.
    struct Look {
        float from;
        float yaw;
        float pitch;
    };
    std::vector<Look> looks;
    for (const auto& [name, value] : launch.extra_env) {
        if (name != "HOST_LOOK") {
            continue;
        }
        size_t at = 0;
        while (at < value.size()) {
            const size_t end = std::min(value.find(';', at), value.size());
            Look look{};
            if (std::sscanf(value.substr(at, end - at).c_str(), "%f:%f,%f", &look.from, &look.yaw,
                            &look.pitch) == 3) {
                looks.push_back(look);
            }
            at = end + 1;
        }
        note("the head follows %zu looks", looks.size());
    }

    // HOST_TURN="<second>:<steps>;...": the player asks to be turned round where they sit, so
    // many steps to the right (to the left if negative), as a button held and the right stick
    // flicked ask in a session.
    struct Turn {
        float at;
        int steps;
        bool done;
    };
    std::vector<Turn> turns;
    for (const auto& [name, value] : launch.extra_env) {
        if (name != "HOST_TURN") {
            continue;
        }
        size_t at = 0;
        while (at < value.size()) {
            const size_t end = std::min(value.find(';', at), value.size());
            Turn turn{};
            if (std::sscanf(value.substr(at, end - at).c_str(), "%f:%d", &turn.at, &turn.steps) ==
                2) {
                turns.push_back(turn);
            }
            at = end + 1;
        }
        note("the view is turned %zu times", turns.size());
    }

    // HOST_PAD="<from second>:<kind>,<right>,<up>,<ahead>,<left>,<tilt>,<roll>;..." among the
    // settings: what the host says of the controller from when on, the way a session does.
    // Where it is in metres from the head (which this test keeps where it starts), how it is
    // turned in degrees (to the left, its front up, its right side up). The kinds:
    //   c  one of the headset's own controllers: it says everything of itself
    //   h  hands seen around a gamepad without motion sensors: they say where it is and, as
    //      far as hands can, how it is turned (HeldPad)
    //   p  hands seen around a gamepad that has motion sensors: only where it is, and its
    //      heading for the sensors to hold on to
    //   n  nothing is seen of it
    struct PadStep {
        float from;
        char kind;
        float right, up, ahead;
        float yaw, pitch, roll;
    };
    std::vector<PadStep> pad_steps;
    float shot_interval = 10.0f;
    for (const auto& [name, value] : launch.extra_env) {
        if (name == "HOST_SHOTS") {
            // A picture every so many seconds instead of every ten.
            shot_interval = std::max(1.0f, std::strtof(value.c_str(), nullptr));
        }
        if (name != "HOST_PAD") {
            continue;
        }
        size_t at = 0;
        while (at < value.size()) {
            const size_t end = std::min(value.find(';', at), value.size());
            PadStep step{};
            if (std::sscanf(value.substr(at, end - at).c_str(), "%f:%c,%f,%f,%f,%f,%f,%f",
                            &step.from, &step.kind, &step.right, &step.up, &step.ahead, &step.yaw,
                            &step.pitch, &step.roll) == 8) {
                pad_steps.push_back(step);
            }
            at = end + 1;
        }
        note("the controller follows %zu steps", pad_steps.size());
    }
    const PadStep* pad_step = nullptr;
    bool pad_seen = false;

    const auto start = Clock::now();
    const auto seconds_since_start = [&] {
        return std::chrono::duration<float>(Clock::now() - start).count();
    };
    uint32_t received = 0;
    uint32_t saved = 0;
    uint32_t last_frame_id = 0;
    uint32_t interval_frames = 0;
    float first_frame_at = -1.0f;
    float next_save_at = 0.0f;
    float interval_start = 0.0f;
    double reported_cpu_seconds = 0.0;
    float copy_ms = 0.0f;
    float worst_copy_ms = 0.0f;
    uint32_t copies = 0;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    std::thread writer;

    auto next_refresh = Clock::now();
    for (uint32_t tick = 0; seconds_since_start() < static_cast<float>(seconds); ++tick) {
        if (display_rate > 0.0f) {
            core.SendRefresh(display_rate);
        }
        const float now = seconds_since_start();
        const auto state = core.GetState();
        if (state == CoreProcess::State::Failed || state == CoreProcess::State::Stopped) {
            note("the core ended after %.1f s: %s", now, core.GetMessage().c_str());
            break;
        }

        // A head that looks slowly left and right, the way a compositor reports it, or
        // where it was told to look.
        float yaw = 0.35f * std::sin(now * 0.8f);
        float pitch = 0.0f;
        Protocol::Pose pose;
        if (looks.empty()) {
            pose.angular_velocity[1] = 0.35f * 0.8f * std::cos(now * 0.8f);
        } else {
            yaw = 0.0f;
            for (const Look& look : looks) {
                if (now >= look.from) {
                    yaw = look.yaw / 57.29578f;
                    pitch = look.pitch / 57.29578f;
                }
            }
        }
        pose.orientation[0] = std::cos(yaw * 0.5f) * std::sin(pitch * 0.5f);
        pose.orientation[1] = std::sin(yaw * 0.5f) * std::cos(pitch * 0.5f);
        pose.orientation[2] = -std::sin(yaw * 0.5f) * std::sin(pitch * 0.5f);
        pose.orientation[3] = std::cos(yaw * 0.5f) * std::cos(pitch * 0.5f);
        core.SendPose(pose);
        if (tick % 300 == 0) {
            core.SendOptics(0.064f);
        }
        // Once, mid-sway: the player asks for the view to be reset (the emulator's log says
        // where it then takes the seat to be).
        if (!view_reset && looks.empty() && now > 33.0f) {
            view_reset = true;
            Protocol::PadPose reset;
            reset.flags = Protocol::PadPose::RecenterSeat;
            core.SendPadPose(reset);
            note("view reset asked for with the head turned %.0f degrees", yaw * 57.29578f);
        }

        for (Turn& turn : turns) {
            if (turn.done || now < turn.at) {
                continue;
            }
            turn.done = true;
            for (int step = 0; step < std::abs(turn.steps); ++step) {
                Protocol::PadPose message;
                message.flags = turn.steps > 0 ? Protocol::PadPose::TurnRight
                                               : Protocol::PadPose::TurnLeft;
                core.SendPadPose(message);
            }
            note("%.0f s: the view is asked to turn %d steps to the right", now, turn.steps);
        }

        // The controller, as a session would report it.
        for (const PadStep& step : pad_steps) {
            if (now >= step.from && (pad_step == nullptr || step.from > pad_step->from)) {
                pad_step = &step;
                note("%.0f s: the controller is '%c', %.2f right, %.2f up, %.2f ahead, turned "
                     "%.0f left, %.0f up, rolled %.0f",
                     now, step.kind, step.right, step.up, step.ahead, step.yaw, step.pitch,
                     step.roll);
            }
        }
        if (pad_step != nullptr) {
            const PadStep& step = *pad_step;
            const float turn = step.yaw / 57.29578f;
            const float tilt = step.pitch / 57.29578f;
            const float roll = step.roll / 57.29578f;
            const HeldPad::Vec right{std::cos(turn) * std::cos(roll), std::sin(roll),
                                     -std::sin(turn) * std::cos(roll)};
            const HeldPad::Vec level_front{-std::sin(turn), 0.0f, -std::cos(turn)};
            const HeldPad::Vec level_up = HeldPad::Cross(right, level_front);
            const HeldPad::Vec front{
                level_front.x * std::cos(tilt) + level_up.x * std::sin(tilt),
                level_front.y * std::cos(tilt) + level_up.y * std::sin(tilt),
                level_front.z * std::cos(tilt) + level_up.z * std::sin(tilt)};
            const HeldPad::Vec middle{step.right, step.up, -step.ahead};
            Protocol::PadPose message;
            const auto place = [&](const HeldPad::Vec& at) {
                message.flags |= Protocol::PadPose::PositionValid;
                message.position[0] = at.x;
                message.position[1] = at.y;
                message.position[2] = at.z;
            };
            const auto turned = [&](const HeldPad::Quat& q) {
                message.flags |= Protocol::PadPose::OrientationValid;
                message.orientation[0] = q.x;
                message.orientation[1] = q.y;
                message.orientation[2] = q.z;
                message.orientation[3] = q.w;
            };
            if (step.kind == 'c') {
                place(middle);
                turned(HeldPad::FromAxes(right, HeldPad::Cross(right, front),
                                         HeldPad::Scale(front, -1.0f)));
            } else if (step.kind == 'h' || step.kind == 'p') {
                // Two palms a gamepad's width apart, both hands pointing the way it does.
                HeldPad::Hands hands;
                hands.left_palm = HeldPad::Sub(middle, HeldPad::Scale(right, 0.1f));
                hands.right_palm = HeldPad::Sub(middle, HeldPad::Scale(right, -0.1f));
                hands.pointing_known = true;
                hands.left_pointing = front;
                hands.right_pointing = front;
                if (const auto held = HeldPad::Locate(hands, 0.0f, 0.35f)) {
                    place(held->position);
                    if (held->turned_known) {
                        message.yaw = held->yaw;
                        message.flags |= Protocol::PadPose::YawValid;
                        if (step.kind == 'h') {
                            turned(held->orientation);
                            message.flags |= Protocol::PadPose::HeldInHands;
                        }
                    }
                }
            }
            // While it is seen, every time; once more when it no longer is.
            const bool seen = message.flags != 0;
            if (seen || pad_seen) {
                core.SendPadPose(message);
            }
            pad_seen = seen;
        }

        // Cross for a quarter of every five seconds gets a title past its menus.
        PadState pad;
        pad.buttons = std::fmod(now, 5.0f) < 0.25f && now > 20.0f ? 0x4000u : 0u;
        core.SetPad(pad);

        ++shown_since;
        if (const auto frame = core.TakeFrame()) {
            if (something_shown) {
                ++shown_for[std::min<uint32_t>(shown_since, 4)];
            }
            something_shown = true;
            shown_since = 0;
            ++received;
            ++interval_frames;
            last_frame_id = frame->frame_id;
            if (first_frame_at < 0.0f) {
                first_frame_at = now;
                note("first frame after %.1f s: buffer %u, eye %ux%u, fov tan %.3f/%.3f/%.3f/%.3f",
                     now, frame->buffer, frame->eye_width, frame->eye_height, frame->fov[0],
                     frame->fov[1], frame->fov[2], frame->fov[3]);
            }
            if (frames.IsValid(frame->buffer)) {
                // What a headset session does with every frame, timed until the GPU is done
                // with it: the copy has to get its turn on a GPU the emulator keeps busy, and
                // the session's frame loop goes no faster than that.
                const auto begun = Clock::now();
                glBindFramebuffer(GL_FRAMEBUFFER, display_framebuffer);
                blitter.Draw(frames.Texture(frame->buffer), width, height,
                             frame->swap_red_blue != 0, true, fov_mask, frame->fov);
                const GLsync done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glClientWaitSync(done, GL_SYNC_FLUSH_COMMANDS_BIT, 2'000'000'000);
                glDeleteSync(done);
                const float took =
                    std::chrono::duration<float, std::milli>(Clock::now() - begun).count();
                copy_ms += took;
                worst_copy_ms = std::max(worst_copy_ms, took);
                ++copies;
            }
            if (frames.IsValid(frame->buffer) && now >= next_save_at && saved < 24) {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                blitter.Draw(frames.Texture(frame->buffer), width, height,
                             frame->swap_red_blue != 0, false, fov_mask, frame->fov);
                glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                             GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                char name[64];
                std::snprintf(name, sizeof(name), "/frame_%03u_%05u.png", saved, frame->frame_id);
                // Packing the picture takes many refreshes of the display this loop stands in
                // for: that is done beside it.
                if (writer.joinable()) {
                    writer.join();
                }
                writer = std::thread{[path = out_dir + name, copy = pixels, width, height] {
                    WritePng(path, copy, width, height);
                }};
                ++saved;
                next_save_at = now + shot_interval;
            }
        }
        if (now - interval_start >= 10.0f) {
            const CoreProcess::Usage usage = core.GetUsage();
            note("%.0f s: %u frames, %.1f per second; emulator %.0f%% of a processor on %s, %d MB; "
                 "copying a frame took %.1f ms, %.1f at worst",
                 now, received, static_cast<float>(interval_frames) / (now - interval_start),
                 (usage.cpu_seconds - reported_cpu_seconds) * 100.0 / (now - interval_start),
                 usage.cpus.c_str(), usage.memory_mb,
                 copies != 0 ? copy_ms / static_cast<float>(copies) : 0.0f, worst_copy_ms);
            if (const uint64_t interruptions = interrupter.Count(); interruptions != 0) {
                note("      %.0f interruptions a second, the large pass took %.2f ms",
                     static_cast<float>(interruptions - reported_interruptions) /
                         (now - interval_start),
                     interrupter.TakePassTime());
                reported_interruptions = interruptions;
            }
            if (display_rate > 0.0f) {
                note("      frames shown for one refresh: %u, for two: %u, for three: %u, for "
                     "more: %u",
                     shown_for[1], shown_for[2], shown_for[3], shown_for[4]);
                std::fill(std::begin(shown_for), std::end(shown_for), 0u);
            }
            reported_cpu_seconds = usage.cpu_seconds;
            interval_start = now;
            interval_frames = 0;
            copy_ms = 0.0f;
            worst_copy_ms = 0.0f;
            copies = 0;
        }
        if (display_rate > 0.0f) {
            next_refresh += std::chrono::nanoseconds{static_cast<int64_t>(1e9f / display_rate)};
            std::this_thread::sleep_until(next_refresh);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    if (writer.joinable()) {
        writer.join();
    }
    note("%u frames received (last id %u), %u pictures in %s, GL error 0x%x", received,
         last_frame_id, saved, out_dir.c_str(), glGetError());
    interrupter.Stop();
    core.Stop();
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &target);
    glDeleteFramebuffers(1, &display_framebuffer);
    glDeleteTextures(1, &display_target);
    blitter.Destroy();
    frames.Destroy(gl.display);
    gl.Destroy();
    return report;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_com_astrobotquest_vrhost_SandboxShell_nativeSelfTest(
    JNIEnv* env, jclass, jstring loader, jstring runtime_root, jstring storage_root, jstring game,
    jstring log_file, jstring out_dir, jint seconds, jobjectArray extra_env, jstring microphone) {
    CoreLaunch launch;
    launch.loader = ToString(env, loader);
    launch.runtime_root = ToString(env, runtime_root);
    launch.storage_root = ToString(env, storage_root);
    launch.game = ToString(env, game);
    launch.log_file = ToString(env, log_file);
    const jsize count = extra_env != nullptr ? env->GetArrayLength(extra_env) : 0;
    for (jsize i = 0; i < count; ++i) {
        const auto element = static_cast<jstring>(env->GetObjectArrayElement(extra_env, i));
        const std::string entry = ToString(env, element);
        env->DeleteLocalRef(element);
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals > 0) {
            launch.extra_env.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        }
    }
    const std::string report =
        RunSelfTest(launch, ToString(env, out_dir), seconds, 1440, 1536,
                    ToString(env, microphone));
    return env->NewStringUTF(report.c_str());
}
