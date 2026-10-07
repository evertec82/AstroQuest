// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <windows.h>

namespace Common {
// Reused only by its owning thread. No global timer-resolution change or busy wait.
class WindowsSleepTimer {
public:
    explicit WindowsSleepTimer(bool high_resolution = true) {
        timer = CreateWaitableTimerExW(nullptr, nullptr,
                                       high_resolution ? CREATE_WAITABLE_TIMER_HIGH_RESOLUTION : 0,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer && high_resolution) {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
    }
    ~WindowsSleepTimer() {
        if (timer)
            CloseHandle(timer);
    }
    WindowsSleepTimer(const WindowsSleepTimer&) = delete;
    WindowsSleepTimer& operator=(const WindowsSleepTimer&) = delete;
    DWORD Wait(std::chrono::nanoseconds duration, bool interruptible) {
        if (duration.count() <= 0)
            return WAIT_OBJECT_0;
        if (!timer)
            return WAIT_FAILED;
        LARGE_INTEGER interval;
        interval.QuadPart = -std::max<long long>(1, duration.count() / 100);
        if (!SetWaitableTimer(timer, &interval, 0, nullptr, nullptr, FALSE))
            return WAIT_FAILED;
        return WaitForSingleObjectEx(timer, INFINITE, interruptible);
    }

private:
    HANDLE timer{};
};
} // namespace Common
