// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/late_frame_wait.h"
#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Core::Vr::LateFrameResult;
int main() {
  int failures = 0;
  std::mutex mutex;
  Core::Vr::LateFrameSignal delivered;
  bool ready = false;
  auto run = [&](auto woke, auto last, float ms) {
    std::unique_lock lock{mutex};
    return Core::Vr::WaitForLateFrame(delivered, lock, woke, last, 90, ms,
                                      [&] { return ready; });
  };
  auto now = Clock::now();
  if (run(now, now, 0) != LateFrameResult::Skipped)
    ++failures;
  if (run(now, now - 1s, 3) != LateFrameResult::Skipped)
    ++failures;
  if (run(now, Clock::time_point{}, 3) != LateFrameResult::Skipped)
    ++failures;
  ready = true;
  if (run(now - 1s, now, 3) != LateFrameResult::Ready)
    ++failures;
  ready = false;
  now = Clock::now();
  if (run(now - 10ms, now - 11ms, 3) != LateFrameResult::TimedOut)
    ++failures;
  std::jthread producer{[&] {
    std::this_thread::sleep_for(1ms);
    std::scoped_lock lock{mutex};
    ready = true;
    delivered.Notify();
  }};
  now = Clock::now();
  if (run(now, now, 30) != LateFrameResult::Ready)
    ++failures;
  producer.join();
  ready = false;
  std::jthread spurious{[&] {
    std::this_thread::sleep_for(1ms);
    delivered.Notify();
  }};
  now = Clock::now();
  if (run(now, now, 3) != LateFrameResult::TimedOut)
    ++failures;
  spurious.join();
  std::vector<double> waits;
  for (int i = 0; i < 50; ++i) {
    now = Clock::now();
    if (run(now, now, 3) != LateFrameResult::TimedOut)
      ++failures;
    waits.push_back(
        std::chrono::duration<double, std::milli>(Clock::now() - now).count());
  }
  std::sort(waits.begin(), waits.end());
  printf("3-ms timeout: median %.3f p95 %.3f max %.3f ms\n", waits[25],
         waits[47], waits.back());
  printf("late-frame disabled/stale/race/deadline/arrival/spurious checks: %d "
         "failures\n",
         failures);
  return failures ? 1 : 0;
}
