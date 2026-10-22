// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>

namespace Common {

// One worker and a bounded queue: compiling a burst must not consume every CPU
// core or retain an unbounded number of pipeline snapshots. Tasks never skip.
class CompileWorker {
public:
    CompileWorker() : worker{[this] { Run(); }} {}
    ~CompileWorker() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        changed.notify_all();
        worker.join(); // Drain before the device/cache and snapshots are destroyed.
    }

    template <typename F>
    auto Submit(F&& function) {
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
        auto result = task->get_future();
        {
            std::unique_lock lock{mutex};
            changed.wait(lock, [this] { return tasks.size() < 16; });
            tasks.emplace_back([task] { (*task)(); });
        }
        changed.notify_all();
        return result;
    }

    void WaitIdle() {
        std::unique_lock lock{mutex};
        changed.wait(lock, [this] { return tasks.empty() && !active; });
    }

private:
    void Run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (tasks.empty()) {
                    return;
                }
                active = true;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            changed.notify_all();
            task(); // packaged_task transports failures to the mandatory bind wait.
            task = {};
            {
                std::lock_guard lock{mutex};
                active = false;
            }
            changed.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> tasks;
    bool active{};
    bool stopping{};
    std::thread worker;
};

} // namespace Common
