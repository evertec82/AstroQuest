// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/precise_timer_scheduler.h"
#include "core/vr/mirror_limiter.h"
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <iostream>
using namespace std::chrono_literals;
int main() {
  using Clock = Common::PreciseTimerScheduler::Clock;
  Common::PreciseTimerScheduler scheduler;
  assert(scheduler.Available());
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> order;
  auto record = [&](int n) {
    std::scoped_lock lock{mutex};
    order.push_back(n);
    cv.notify_one();
  };
  auto start = Clock::now();
  // An earlier timer inserted while the worker sleeps must interrupt the old
  // deadline.
  scheduler.Add(start + 60ms, [&] { record(2); });
  std::this_thread::sleep_for(2ms);
  scheduler.Add(start + 10ms, [&] {
    record(1);
    // Scheduling inside a callback must not deadlock the queue.
    scheduler.Add(start + 25ms, [&] { record(3); });
  });
  {
    std::unique_lock lock{mutex};
    assert(cv.wait_for(lock, 2s, [&] { return order.size() == 3; }));
  }
  assert((order == std::vector<int>{1, 3, 2}));
  std::vector<double> lateness;
  for (int i = 0; i < 100; ++i) {
    bool fired = false;
    const auto deadline = Clock::now() + 3ms;
    scheduler.Add(deadline, [&] {
      std::scoped_lock lock{mutex};
      lateness.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - deadline)
              .count());
      fired = true;
      cv.notify_one();
    });
    std::unique_lock lock{mutex};
    assert(cv.wait_for(lock, 1s, [&] { return fired; }));
    assert(lateness.back() >= 0);
  }
  std::sort(lateness.begin(), lateness.end());
  std::cout << "3-ms timer lateness (us): median=" << lateness[50]
            << " p95=" << lateness[95] << " max=" << lateness.back() << '\n';
  // Destruction cancels pending deadlines and joins the worker promptly.
  bool unexpected = false;
  start = Clock::now();
  {
    Common::PreciseTimerScheduler temporary;
    temporary.Add(Clock::now() + 1h, [&] { unexpected = true; });
  }
  assert(!unexpected && Clock::now() - start < 1s);
  for (int source_rate : {60, 72, 90, 120, 144}) {
    Core::Vr::MirrorLimiter limiter;
    int count = 0;
    for (int i = 0; i < source_rate * 10; ++i) {
      const auto now = Clock::time_point{} + 1s +
                       std::chrono::nanoseconds{1000000000LL * i / source_rate};
      count += limiter.Due(now, true, 60);
    }
    assert(count >= 598 && count <= 601);
    std::cout << source_rate << " Hz source: " << count
              << " mirror updates / 10 seconds\n";
  }
  Core::Vr::MirrorLimiter limiter;
  assert(!limiter.Due(Clock::now(), true, 0));
  assert(limiter.Due(Clock::now(), false, 0));
  assert(limiter.Due(Clock::now(), true, 60));
  assert(limiter.Due(Clock::now() + 10s, true, 60));
  assert(!limiter.Due(Clock::now() + 10s, true, 60));
  std::cout << "Timer ordering/reentrant scheduling/shutdown and mirror "
               "cap/fallback: passed\n";
}
