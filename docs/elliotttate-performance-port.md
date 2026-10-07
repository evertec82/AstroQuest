# Selected elliotttate performance port

Reviewed on 2026-10-07: [elliotttate/AstroQuest at b72db1b](https://github.com/elliotttate/AstroQuest/tree/b72db1b).
[Their measurements and limitations](https://github.com/elliotttate/AstroQuest/blob/b72db1b/docs/pc-performance.md).

Adapted pipeline-cache repairs and late-frame recovery from `6af3210`, preserving the original license headers. This candidate retains upstream 0.20, the soccer-enemy fix, Full framerate selector, expanded resolutions, separate headset queue and precise presentation timer.

## Included

Cached image/sampler lists now contain their storage, so a new process does not load pointers from the process that wrote the cache. Ordinary descriptor layouts avoid reading guest descriptors during preloading; dynamic-mip layouts requiring guest memory are deferred. Vertex-fetch data survives later shader stages, and shader permutations point at the persistent cached program's information.

Metadata and pipeline-key versions are incremented to reject the former layout. Upstream 0.20's shader-binary version 3 (geometry-input ABI fix) is preserved. Separate cold/warm processes test cache reuse before enabling it in the installed configuration.

When a recent stream's next frame is missing, the OpenXR thread waits briefly before repeating the last picture. The budget starts at xrWaitFrame wakeup, defaults to 3 ms, and is configurable with `SHADPS4_XR_LATE_WAIT_MS` (0 disables, maximum 6). Idle streams and pictures the runtime does not want are skipped. No Vulkan queue lock is held. The log counts recovered pictures and timeouts.

The initial condition-variable port's nominal 3-ms timeout took 15-16 ms on this PC. The Windows adaptation uses a producer event and a high-resolution timeout timer; measured timeout was about 3.5 ms. Arriving frames can wake it immediately. Unsupported precise timers disable waiting instead of using a coarse timeout. This is a scheduling budget, not a hard real-time guarantee.

## Kept separate

- Their native 90/120 engine mode is promising, but their 120 measurements use a simulator and game-1.00 offsets. The 90 mode is unmeasured and 1.04 offsets are unverified. It is not enabled here.
- Parallel texture copies, stream/shader memoization and lock-skipping are not imported wholesale. Their notes document unresolved memory corruption near the parallel-copy/shader-memo work; no causal fix is established.
- Their 32 presentation polls and high-resolution sleeps overlap with this fork. Their new delivery phase was measured at simulated 120 Hz; this fork retains its phase until a real-runtime comparison.

Startup checks do not establish improved headset gameplay. The existing regular release remains available for comparison.

## Precise guest timers and reduced desktop mirror updates (2026-10-07)

This candidate also adapts their precise guest HR-timer scheduling and mirror-rate limit. Windows HR events of 1.2 ms or longer use a dedicated high-resolution waitable timer thread, with a final 300 microseconds of deadline checking. Short HR timers and ordinary periodic timers retain their existing paths. Timer arm serials reject deleted/replaced events. Weak owner guards and a destruction lock prevent callbacks from accessing deleted event queues; the worker is joined at scheduler shutdown. Unsupported high-resolution timers use the existing asio implementation.

The local headset still receives every available frame. Its desktop spectator/stereo mirror defaults to 60 updates per second. A deadline accumulator preserves an average 60 updates at 72/90/120 Hz instead of rounding down to a divisor of the headset rate. Desktop-only and remote-export presentation remain unrestricted. The simple FPS counter counts game submissions rather than mirror updates.

For comparisons, add lines to `pc-vr/settings.txt` (restart the game):

- `env=SHADPS4_PERF_PRECISE_TIMERS=0`: original guest timer path.
- `env=SHADPS4_VR_WINDOW_FPS=0`: disable the local headset mirror.
- `env=SHADPS4_VR_WINDOW_FPS=1000`: effectively remove the mirror cap at normal headset rates.
- `env=SHADPS4_TIMER_SPIN_US=0`: precise timer sleeping without the final spin (default 300; maximum 5000).

Validation: 100 standalone 3-ms deadlines had 0.9 microseconds median lateness, 16.4 microseconds p95 and 68.5 microseconds maximum on this PC. Ordering, earlier-deadline wakeup, scheduling from callbacks and worker shutdown passed. Mirror tests produced 600 updates in ten seconds for 60/72/90/120/144 Hz sources, and checked disabled-mirror/desktop-only fallback. These are component tests, not headset gameplay measurements.

Three isolated SteamVR startup checks ran for 45 seconds each without a critical log error: precise timers with the 60-FPS mirror, precise timers with the mirror disabled, and original timers with the mirror cap effectively removed. The warm launch preloaded all 27 cached pipelines and compiled none. These checks used an idle headset and do not establish native gameplay FPS or judder improvements.
