// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>
#ifdef _WIN32
#include <immintrin.h>
#include "common/windows_sleep_timer.h"
#endif
namespace Common {
// Adapted from elliotttate/AstroQuest's precise guest HR timer scheduler.
// Callback owners supply lifetime/cancellation guards. Callbacks run without the queue lock.
class PreciseTimerScheduler {
public:
    using Clock = std::chrono::steady_clock;
    explicit PreciseTimerScheduler(
        std::chrono::microseconds budget = std::chrono::microseconds{300})
        : spin(budget) {
#ifdef _WIN32
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        available = wake && timer.IsHighResolution();
        if (available)
            worker = std::thread{[this] { Run(); }};
#else
        available = true;
        worker = std::thread{[this] { Run(); }};
#endif
    }
    ~PreciseTimerScheduler() {
#ifdef _WIN32
        stop.store(true);
        if (wake)
            SetEvent(wake);
        if (worker.joinable())
            worker.join();
        if (wake)
            CloseHandle(wake);
#else
        {
            std::scoped_lock lock{mutex};
            stop.store(true);
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
#endif
    }
    bool Available() const {
        return available;
    }
    bool Add(Clock::time_point deadline, std::function<void()> callback) {
        if (!available)
            return false;
        {
            std::scoped_lock lock{mutex};
            queue.push({deadline, std::move(callback)});
        }
#ifdef _WIN32
        SetEvent(wake);
#else
        changed.notify_one();
#endif
        return true;
    }

private:
    struct Entry {
        Clock::time_point deadline;
        std::function<void()> callback;
        bool operator>(const Entry& other) const {
            return deadline > other.deadline;
        }
    };
#ifdef _WIN32
    void Run() {
        SetThreadDescription(GetCurrentThread(), L"shadPS4:PreciseTimers");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        while (!stop.load()) {
            std::unique_lock lock{mutex};
            if (queue.empty()) {
                lock.unlock();
                WaitForSingleObject(wake, INFINITE);
                continue;
            }
            const auto deadline = queue.top().deadline;
            const auto now = Clock::now();
            if (now >= deadline) {
                auto callback = queue.top().callback;
                queue.pop();
                lock.unlock();
                callback();
                continue;
            }
            lock.unlock();
            if (deadline - now > spin) {
                if (timer.Arm(deadline - now - spin)) {
                    const HANDLE handles[]{wake, timer.Handle()};
                    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                } else {
                    WaitForSingleObject(wake, 1);
                }
            } else {
                // Recheck the queue while spinning so an earlier newly armed timer wins.
                _mm_pause();
            }
        }
    }
    WindowsSleepTimer timer;
    HANDLE wake{};
#else
    void Run() {
        std::unique_lock lock{mutex};
        while (!stop.load()) {
            if (queue.empty()) {
                changed.wait(lock, [this] { return stop.load() || !queue.empty(); });
                continue;
            }
            const auto deadline = queue.top().deadline;
            if (Clock::now() < deadline) {
                changed.wait_until(lock, deadline);
                continue;
            }
            auto callback = queue.top().callback;
            queue.pop();
            lock.unlock();
            callback();
            lock.lock();
        }
    }
    std::condition_variable changed;
#endif
    std::thread worker;
    std::atomic<bool> stop{false};
    bool available{};
    std::chrono::microseconds spin;
    std::mutex mutex;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
};
} // namespace Common
