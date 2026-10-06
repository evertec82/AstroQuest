// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "pad_state.h"
#include "vr_protocol.h"

/// Whether the game may listen to the headset's microphone, and how much louder than it is
/// what it hears should be made.
struct MicrophoneSettings {
    bool enabled{true};
    float gain{1.0f};
    /// For tests nobody is there to blow for: instead of the microphone, a second of noise
    /// every five, as loud as the game takes blowing hard to be.
    bool test_signal{};
    /// Set while the player blows with buttons instead of with their breath: the game then
    /// hears that same noise, whatever the microphone has to say (or is allowed to).
    std::shared_ptr<const std::atomic<bool>> blowing;
};

/// Everything needed to start the emulator core.
struct CoreLaunch {
    std::string loader;       ///< glibc's dynamic loader, inside the app's native library folder
    std::string runtime_root; ///< extracted runtime: host/ (libraries, core), drivers/
    std::string storage_root; ///< writable folder the core keeps its data in
    std::string game;         ///< path of the game's eboot.bin
    std::string log_file;
    std::vector<std::string> extra_args;
    std::vector<std::pair<std::string, std::string>> extra_env;
};

/// Runs the emulator core as a child process and serves the sockets it talks to: the control
/// line protocol (state out, controller in), audio, and the VR frame/pose channel.
class CoreProcess {
public:
    enum class State { Idle, Starting, Running, Stopped, Failed };

    CoreProcess() = default;
    ~CoreProcess();

    bool Start(const CoreLaunch& launch);
    void Stop();

    State GetState() const {
        return state.load();
    }
    /// Human-readable reason once the state is Stopped or Failed.
    std::string GetMessage();
    /// Frames the core reported as presented so far.
    uint64_t GetPresentedFrames() const {
        return presented_frames.load();
    }

    void SetPad(const PadState& pad);

    /// Freezes the emulator while nobody is looking (headset taken off), or lets it go on.
    void SetPaused(bool paused);

    /// Offers the frame buffers to the core. `fds` are dma-bufs, they stay owned by the caller.
    void SetBuffers(const std::vector<int>& fds, uint32_t width, uint32_t height, uint32_t stride);
    void SendPose(const Core::Vr::Protocol::Pose& pose);
    void SendPadPose(const Core::Vr::Protocol::PadPose& pose);
    void SendOptics(float ipd);
    /// The display refreshes: the emulated headset keeps step with it (`rate` in Hz).
    void SendRefresh(float rate);
    /// Returns the newest frame the core finished since the last call, if any.
    std::optional<Core::Vr::Protocol::Frame> TakeFrame();

    /// What the game last asked of the controller, packed: small motor, large motor, red,
    /// green, blue, one byte each from the lowest up.
    uint64_t GetPadFeedback() const {
        return pad_feedback.load();
    }

    /// What the emulator process has used: processor time since it started, the processors it
    /// may run on (as the system lists them, "0-5") and its memory.
    struct Usage {
        double cpu_seconds{};
        std::string cpus;
        int memory_mb{};
    };
    Usage GetUsage() const;

    /// Applies to microphone ports the game opens from now on.
    void SetMicrophone(bool enabled, float gain, bool test_signal = false);
    /// The player blows with buttons, or has stopped: what the game hears follows at once.
    void SetBlowing(bool blowing_) {
        blowing->store(blowing_, std::memory_order_relaxed);
    }

    /// Thread id of the core's thread whose name starts with `prefix`, or 0.
    pid_t FindThread(const std::string& prefix) const;
    /// All threads of the emulator whose names begin with `prefix`.
    std::vector<pid_t> FindThreads(const std::string& prefix) const;

private:
    void ControlLoop();
    void AudioLoop();
    void VrLoop();
    void WaitLoop();
    void SendBuffersLocked();
    void Fail(const std::string& message);

    std::atomic<State> state{State::Idle};
    std::atomic<bool> quit{};
    std::atomic<bool> paused{};
    std::atomic<uint64_t> presented_frames{};
    std::atomic<uint64_t> pad_feedback{};
    std::atomic<pid_t> pid{-1};

    int control_listen{-1};
    int audio_listen{-1};
    int vr_listen{-1};
    std::atomic<int> control_fd{-1};
    std::atomic<int> vr_fd{-1};

    std::thread control_thread;
    std::thread audio_thread;
    std::thread vr_thread;
    std::thread wait_thread;

    MicrophoneSettings microphone;
    /// Shared with the threads that serve the game's microphone ports, which may outlast this.
    std::shared_ptr<std::atomic<bool>> blowing{std::make_shared<std::atomic<bool>>(false)};

    std::mutex mutex;
    std::string message;
    PadState pad;
    std::optional<Core::Vr::Protocol::Frame> frame;
    std::vector<int> buffer_fds;
    Core::Vr::Protocol::Buffers buffers{};
    bool buffers_sent{};
};
