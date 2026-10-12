# stutter-next notes (K11, K29, K33)

Branch `stutter-next`, from `gratton/gratton-shaders` (20cfb4dd: phase 2 async pipelines + PM4 lookahead).
All new behaviour is behind default-off flags except the K11 fix, which only makes output deterministic.

## K11: deterministic dispatcher spill ids (always on)
The second wave half's spill ids were allocated by iterating a pointer-keyed `unordered_map`, so a dispatcher-mode
module differed from run to run. Ids are now allocated in program order (`spirvEmitterProgram.cpp`). Test:
`TestNewShaderRecompilerDispatcherSpillIdsAreDeterministic` (shader_cfg) translates one irreducible shader 12 times under
different heap layouts with 24 spills in 2-lane mode and requires identical SPIR-V; it fails without the fix.
Idea from chenxiao07/KytyPS5@e6dfd8e6b (GPL-2.0).

## K29: `--skip-ir-validation <true|false>` (default false)
`ValidateProgram` (after translation and before SPIR-V emission) and the duplicate-use scan in `Inst::AddUse` only stop the
emulator on a malformed IR. With the flag they are skipped in release builds; debug builds always validate, and
`KYTY_IR_VALIDATE=1` forces them on. Test: the SPIR-V is byte-identical with and without validation, a broken program is
still caught when validation is on, the gate honours build type and env override (`TestShaderIrValidationGate`).
Idea from chenxiao07/KytyPS5@a28de66fe.

## K33: `--pipelines-unoptimized-first <true|false>` (default false)
Needs `--async-pipelines stall|skip`; ignored otherwise.

Flow, per pipeline job (the GPU thread's own misses and lookahead prefetches use the same job classes):
1. `CreatePipelineInternal(..., PipelineBuild::Fast)`: ask the driver cache with `FAIL_ON_PIPELINE_COMPILE_REQUIRED`
   (device feature `pipelineCreationCacheControl`, enabled only when the flag is on). A hit gives the optimized pipeline,
   nothing is compiled. A miss compiles with `DISABLE_OPTIMIZATION` and no cache, marks `Pipeline::unoptimized`.
2. The job publishes the pipeline (the waiting draw proceeds) and queues an Optimize job on the pool's **low-priority
   queue**: served only when the normal queue is empty, at most half of the workers at a time. The Optimize job reuses
   the unoptimized pipeline's layouts and fills an `OptimizedBuild`.
3. The GPU thread, at its next lookup of that pipeline (`PromoteOptimized`, the only writer after publication), swaps the
   new `VkPipeline` into the `Pipeline` object. The replaced handle goes to a retire handler; `RenderContext` defers its
   destruction to the end of the current scheduler tick (command buffers in flight may still bind it).

Gates: active only on NVIDIA (an AMD RDNA2 driver crashed in the fork that introduced this; `KYTY_PIPELINE_FAST_BUILD=1`
forces it on other vendors, `=0` forces it off) and only when the driver cache can be probed (or there is no driver cache),
so a warm cache is never ignored in favour of unoptimized code.

Observability: console line `Unoptimized-first pipelines: unoptimized_created=..;served_from_cache=..;optimized_done=..;swapped=..;retired=..`
at shutdown, `summary_unoptimized_first` timing row, `unopt=` on `pipeline_create_*` rows, `pipeline_optimize_*` rows, `low_*`
counters in the pool summary.

Known limits:
- Pipelines whose optimized build had not finished at exit stay out of the saved driver cache, so the next session builds
  them unoptimized-first again (queued Optimize jobs are cancelled at shutdown; a running one is waited for).
- Until the swap the pipeline runs unoptimized code: GPU time per draw can be higher for a while. Measured in
  `bench/RESULTS.md`.
- Retired handles are held until the GPU finishes the current tick, so VRAM holds both versions briefly.
- Tests: `shader_recompiler_compute_unoptimized_first` (whole GPU suite on unoptimized pipelines swapped while it runs),
  `graphics_pipeline_rasterization_unoptimized_first`, `pipeline_unoptimized_first_promotion`, `async_pipeline` (low-priority
  queue). The probe path with a real driver cache is not covered by a unit test (needs a title id); see the bench runs.

## Measured (Astro Bot, skip3 to s14, fresh cache, RTX 3080 Ti, driver 610.57; bench/RESULTS.md has the table)
- K33: no gain. DISABLE_OPTIMIZATION does not shorten the compile on this driver: unoptimized compute pipelines 54.9 s total
  (max 3.96 s) vs 53.7 s (max 3.99 s) stock; graphics 34.8 s vs 35.0 s. The optimized rebuild afterwards costs 0.5 s in total
  (the driver reuses the first compile), so the swap is correct but pointless here. Default stays off.
- K29: translation 2.66 s to 2.05 s (-23 %) over 261 shaders, output identical.
- No res pin on this base (no `--gpu-timestamps`); Astro Bot renders at 1080p at every scale.
