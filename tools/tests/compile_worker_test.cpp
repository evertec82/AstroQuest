// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/compile_worker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    std::atomic<int> completed{};
    {
        Common::CompileWorker worker;
        std::promise<void> release;
        auto gate = release.get_future().share();
        int live_value = 7;
        auto first = worker.Submit([snapshot = live_value, gate] {
            gate.wait();
            return std::make_unique<int>(snapshot);
        });
        live_value = 99;
        assert(first.wait_for(10ms) == std::future_status::timeout);
        release.set_value();
        assert(*first.get() == 7); // Owned state, move-only Vulkan-like result.

        auto failure = worker.Submit([]() -> int { throw std::runtime_error("compile failed"); });
        bool caught = false;
        try { (void)failure.get(); } catch (const std::runtime_error&) { caught = true; }
        assert(caught);
        assert(worker.Submit([] { return 42; }).get() == 42); // Worker survives failure.

        // More than the queue capacity: no dropped jobs and FIFO execution.
        std::vector<std::future<int>> results;
        for (int i = 0; i < 100; ++i) {
            results.push_back(worker.Submit([&completed] { return completed++; }));
        }
        worker.WaitIdle();
        for (int i = 0; i < 100; ++i) {
            assert(results[i].get() == i);
        }
        for (int i = 0; i < 100; ++i) {
            (void)worker.Submit([&completed] { ++completed; });
        }
    } // Must drain jobs whose futures were never consumed.
    assert(completed == 200);
    std::cout << "PASS: owned inputs, mandatory readiness, failures, FIFO, queue drain\n";
}
