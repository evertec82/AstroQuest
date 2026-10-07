#include "common/windows_sleep_timer.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
int main() {
  int failed = 0;
  Common::WindowsSleepTimer timer;
  if (timer.Wait(0ns, false) != WAIT_OBJECT_0 ||
      timer.Wait(-1ns, false) != WAIT_OBJECT_0)
    ++failed;
  if (timer.Wait(1ns, false) != WAIT_OBJECT_0)
    ++failed;
  std::atomic_int callbacks{};
  QueueUserAPC([](ULONG_PTR p) { ++*reinterpret_cast<std::atomic_int *>(p); },
               GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&callbacks));
  if (timer.Wait(10ms, true) != WAIT_IO_COMPLETION || callbacks != 1)
    ++failed;
  if (timer.Wait(1ms, false) != WAIT_OBJECT_0)
    ++failed;
  std::atomic_int thread_failures{};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t)
    workers.emplace_back([&] {
      Common::WindowsSleepTimer own;
      for (int i = 0; i < 20; ++i)
        if (own.Wait(400us, false) != WAIT_OBJECT_0)
          ++thread_failures;
    });
  for (auto &worker : workers)
    worker.join();
  failed += thread_failures.load();
  for (bool high : {false, true}) {
    Common::WindowsSleepTimer own(high);
    std::vector<double> samples;
    for (int i = 0; i < 200; ++i) {
      auto start = std::chrono::steady_clock::now();
      if (own.Wait(434us, false) != WAIT_OBJECT_0)
        ++failed;
      samples.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count());
    }
    std::sort(samples.begin(), samples.end());
    printf("%s 0.434-ms wait: median %.3f p95 %.3f p99 %.3f ms\n",
           high ? "high resolution" : "standard", samples[100], samples[190],
           samples[198]);
  }
  printf("zero/negative/tiny waits, APC interruption and rearming, independent "
         "thread timers: %d failures\n",
         failed);
  return failed ? 1 : 0;
}
