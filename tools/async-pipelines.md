# Asynchronous graphics pipeline candidate

Graphics pipeline driver compilation runs on one worker while the render thread
prepares textures, vertex buffers and attachments. `Pipeline::Handle()` waits for
the actual pipeline before the draw is bound. There is no missing-pipeline skip,
placeholder shader or substitute rendering state.

GCN translation and compute compilation remain synchronous. First-use stalls
can remain when compilation takes longer than resource preparation. This is an
overlap optimization, not a promise of stutter-free first encounters.

Inputs that depend on guest memory (descriptor layout, vertex descriptions,
multisample settings and auxiliary shader code) are captured on the render
thread. The worker owns its descriptions; Vulkan create-info pointers refer to
worker-local storage. Pipeline destruction and shader replacement wait for
outstanding jobs. The queue is bounded to 16 entries and one worker to avoid a
compilation burst consuming all CPU cores. Vulkan's default pipeline-cache flags
provide internal creation synchronization:
https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateGraphicsPipelines.html

Enabled by default. Set `SHADPS4_ASYNC_PIPELINES=0` before launch for the synchronous
comparison. In the PC launcher's settings.txt this is `env=SHADPS4_ASYNC_PIPELINES=0`.
Remove that override to enable it again.

Logs separate `prepared in` (render-thread setup), `driver compile` (worker
duration), and `Async pipeline bind wait` (unhidden delay at first use). Compare
the same scene and resolution, allowing for driver-cache warming between runs.
No performance gain or visual correctness has yet been verified in a headset.

Compile `tools/tests/compile_worker_test.cpp` as C++20 or later with the core's
`src` include directory. It checks input ownership, readiness, propagated errors,
FIFO execution, more jobs than queue capacity, and draining during destruction.
