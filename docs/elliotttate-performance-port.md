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
- Their guest HR-timer scheduler addresses the game's 3-ms tick, with broader cancellation/lifetime implications than the presentation timer already in this fork.
- Parallel texture copies, stream/shader memoization and lock-skipping are not imported wholesale. Their notes document unresolved memory corruption near the parallel-copy/shader-memo work; no causal fix is established.
- Their 32 presentation polls and high-resolution sleeps overlap with this fork. Their new delivery phase was measured at simulated 120 Hz; this fork retains its phase until a real-runtime comparison.

Startup checks do not establish improved headset gameplay. The existing regular release remains available for comparison.
