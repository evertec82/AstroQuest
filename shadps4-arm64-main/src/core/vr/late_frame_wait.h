// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#ifdef _WIN32
#include "common/windows_sleep_timer.h"
#endif

namespace Core::Vr {
// Producer notification wakes immediately; the Windows timeout uses its own high-
// resolution timer instead of the coarse timeout of SleepConditionVariableSRW.
class LateFrameSignal {
public:
#ifdef _WIN32
    LateFrameSignal() : event{CreateEventW(nullptr, FALSE, FALSE, nullptr)} {}
    ~LateFrameSignal() {
        if (event)
            CloseHandle(event);
    }
    void Notify() {
        if (event)
            SetEvent(event);
    }
#else
    void Notify() {
        delivered.notify_one();
    }
#endif
    LateFrameSignal(const LateFrameSignal&) = delete;
    LateFrameSignal& operator=(const LateFrameSignal&) = delete;
#ifndef _WIN32
    LateFrameSignal() = default;
#endif
    template <typename Ready>
    bool WaitUntil(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point until,
                   Ready ready) {
#ifdef _WIN32
        while (!ready()) {
            const auto remaining = until - std::chrono::steady_clock::now();
            // Unsupported timers or allocation failure disable waiting, rather than use
            // a coarse timeout that can consume a whole headset refresh.
            if (remaining.count() <= 0 || !event || !timer.IsHighResolution())
                return false;
            // Producer updates readiness and notifies under the same mutex. Resetting
            // before unlocking cannot lose a delivery between the predicate and wait.
            ResetEvent(event);
            if (!timer.Arm(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)))
                return false;
            const HANDLE handles[]{event, timer.Handle()};
            lock.unlock();
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            lock.lock();
            if (ready())
                return true;
            if (result != WAIT_OBJECT_0)
                return false;
        }
        return true;
#else
        return delivered.wait_until(lock, until, ready);
#endif
    }

private:
#ifdef _WIN32
    HANDLE event{};
    Common::WindowsSleepTimer timer;
#else
    std::condition_variable delivered;
#endif
};

enum class LateFrameResult { Skipped, Ready, TimedOut };

// Budget begins at the runtime wakeup. The predicate is always checked first so a
// delivery between the initial TakeFrame and taking the mutex is not lost.
template <typename Ready>
LateFrameResult WaitForLateFrame(LateFrameSignal& delivered, std::unique_lock<std::mutex>& lock,
                                 std::chrono::steady_clock::time_point woke,
                                 std::chrono::steady_clock::time_point last_delivery, float rate,
                                 float wait_ms, Ready ready) {
    using Clock = std::chrono::steady_clock;
    if (ready())
        return LateFrameResult::Ready;
    if (!(wait_ms > 0.0f && rate > 30.0f) || last_delivery == Clock::time_point{} ||
        woke - last_delivery >= std::chrono::duration<double>(2.0 / rate)) {
        return LateFrameResult::Skipped;
    }
    const auto until = woke + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double, std::milli>(wait_ms));
    if (Clock::now() >= until)
        return LateFrameResult::TimedOut;
    return delivered.WaitUntil(lock, until, ready) ? LateFrameResult::Ready
                                                   : LateFrameResult::TimedOut;
}
} // namespace Core::Vr
