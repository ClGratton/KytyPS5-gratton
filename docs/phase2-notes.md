# Phase 2 notes (shaders.md C1a items 4-6: captured jobs, worker pool, in-flight dedupe, stall/skip)

Branch `phase2-581` (from `phase1-581` tip `52f86e3`), worktree `kyty/wt-phase2-581`, build dir `kyty/build/phase2-581`.
Local commits only. Default `--async-pipelines off` keeps the stock code path: the hit path and the miss path are the
phase 1 code (no worker, no extra lock; only a null check on `AsyncPool()`).

## Commits

| SHA | What |
|---|---|
| `06e3b37` | `--async-pipelines off\|stall\|skip`, `--pipeline-workers N` (config, parsing, help) |
| `a656c93` | `AsyncPipelineCompiler` pool, `AsyncJob`, `InFlightRegistry`, `Bench::ThreadId/LogTimingAs`, `async_pipeline_tests` |
| `965d33b` | stall mode: `ProgramJob`, `GraphicsPipelineJob`, `ComputePipelineJob`, thread-safe publish, atomics, shutdown order |
| `058ed47` | skip mode: draw drop, `pending`, skip counters, timing fields, `graphics_pipeline_rasterization_async_skip` test |

Only the branch tip was built (Release, clang/lld, ccache); `965d33b` was derived mechanically from the tip by removing the
skip bits and was not compiled on its own (same practice as phase 1). The tip is what the tests and the sanity runs used.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--async-pipelines off\|stall\|skip` | `off` | `off`: stock synchronous. `stall`: a miss submits a job and the draw/dispatch waits for it. `skip`: graphics draws whose program or pipeline is not ready are dropped; compute always waits. |
| `--pipeline-workers N` (1-64) | `min(8, hardware threads / 2)`, at least 1 | pool size (8 on this machine) |

`--shader-timing-log`: the rows `program_translate`, `pipeline_create_gfx`, `pipeline_create_cs` keep their phase 1 detail and,
in async modes, get `;mode=<stall|skip>;queue_ns=<submit to worker start>;wait_ns=<GPU thread blocked on this job>;skipped=<draws dropped waiting for it>`
appended. In async modes these rows are written by the worker: `thread` is the worker's tid and `gpu_thread` is 0
(for a new source, `frontend_ns`/`plan_ns`/`materialize_ns` inside `program_translate` were spent on the GPU thread before the hand-off).
A `summary_async` row (and the console line `Async pipelines: stopped (...)`) is written when the pool stops:
`jobs_submitted/completed/cancelled, peak_queue, gpu_waits, gpu_wait_ns, skipped_draws, skipped_program_draws, skipped_pipeline_draws`.
Off mode writes exactly the phase 1 rows.

## Design

### Pieces

| Piece | Where | What |
|---|---|---|
| `AsyncPipelineCompiler` | `pipeline/asyncPipelineCompiler.{h,cpp}` | Fixed pool of `std::thread` workers (`--pipeline-workers`), FIFO queue, `Submit()`, `Shutdown()` (cancels queued jobs, lets running ones finish, joins), stats. No Vulkan dependency, so it is unit-tested alone. |
| `AsyncJob` | same | Base class of a unit of work. `Execute()` runs on a worker, `Cancel()` replaces it at shutdown. `Wait()` blocks the GPU thread and accounts the blocked time. Carries `skipped` / `wait_ns` / `queue_ns` and the timing rows recorded while it ran; the rows are written when the job is destroyed (after the last waiter and skip), so they can carry the final `wait_ns` and `skipped`. |
| `InFlightRegistry<Key>` | same | De-duplication table: key -> the in-flight job. One per key type: `ProgramJobKey` (inside `ProgramCache`), `GraphicsPipelineKey`, compute program id. |
| `ProgramJob` | `pipelineCache.cpp`, in `ProgramCache` | Compiles one shader permutation: (for a known source) `TranslateProgram`, then `CompilePermutation` = `CompileProgram` + SPIR-V validation + `vkCreateShaderModule`, then publishes into the `SourceEntry`. |
| `GraphicsPipelineJob`, `ComputePipelineJob` | `pipelineCache.cpp`, nested in `PipelineCache` | `CreatePipelineInternal` (descriptor and pipeline layouts, `vkCreate{Graphics,Compute}Pipelines` on the shared driver `VkPipelineCache`), then publishes into `m_graphics_pipelines` / `m_compute_pipelines`. |

### Who does what

GPU thread (unchanged, always): `PrepareProgram` (reads guest registers), `DumpShaderOriginal`, `MaterializeResources`
(guest memory reads, `ResourcePlan` scratch), `FindPermutation`, building the pipeline key, descriptor binding and recording.
For a source seen for the first time it also runs `TranslateProgram` and `ExtractResourcePlan` (the resource plan comes
out of the translation, and `MaterializeResources` needs it), then hands the translated program to the job.
The measured share of this step is 2.6% of the spike time, so it was not worth a two-step job.

Worker: `TranslateProgram` (known source, new permutation), `CompileProgram`, SPIR-V validation/dump, `vkCreateShaderModule`,
`CreatePipelineInternal`, and the publish.

### What a job captures (all by value, on the GPU thread)

Program job: the guest code bytes (`std::vector<uint32_t>`, empty when the GPU thread already translated), `back_code`,
`user_data` (only the `user_data_count` words), `wave_size`, `user_data_base`, the stage input info (a by-value copy of the
`ShaderVertexInputInfo` / `ShaderPixelInputInfo` / `ShaderComputeInputInfo`; the job re-points `options.input_info` at its own
copy), stage label/name, `dump_ir`, the resolved `ResourceSpecialization` (inside the job key, with its hash computed at
capture), the push-data cursor, and for a new source the `TranslateResult` itself (moved in).
Pipeline job: the full `GraphicsPipelineKey` (rendering state, vertex input, static parameters, program ids), copies of the
vertex input infos (1 or 3) and of the pixel input info, the `GraphicsPrograms` handles (ids + `VkShaderModule`), the driver
`VkPipelineCache` handle, and for compute the `ShaderComputeInputInfo` copy and the module.
The `stage.program` pointers inside the copied infos point at published `Permutation`s. Those live in a `std::deque`
inside a `SourceEntry`, are never erased while the cache lives, and are immutable after publish.
`stage.resources` (the `&entry.resources` aliasing noted in the phase 1 audit) is never read by a worker.

### Flow per mode

`ProgramCache::Get` (hit path unchanged, `FindPermutation` first):

```
miss -> key = (ProgramKey, specialization + hash, push cursor)
  known source: Find(key) in registry
       in flight   -> stall / compute: Wait();  skip (graphics): count skip, return "pending"
       not in flight -> re-check published; else capture ProgramJob, Insert, Submit, then as above
  new source:  GPU thread translates, inserts the entry, materializes; capture job with the translated program;
       Insert, Submit, then stall: Wait / skip: return "pending"
  Wait() returned false (pool stopped): run the stock synchronous code
```
`GetGraphicsPipeline`: key lookup (shared lock), else `Find(key)` in the registry, else capture `GraphicsPipelineJob`;
skip -> return `nullptr` (the draw is dropped in `ExecutePreparedDraw` before anything depending on the pipeline is
recorded), stall -> wait and look the pipeline up. `GetComputePipeline` always waits.

Skip mode details:
* A graphics draw is dropped at two places: `PrepareDrawRenderState` (a program is pending: nothing has been resolved yet, so
  the drop has no side effect) and `ExecutePreparedDraw` (pipeline pending: after binding preparation, before
  `AcquireRenderTargets`/commit, as in KytyPlus).
* A pending stage stops the program lookup for the remaining stages of that draw. The stages after it cannot be captured
  because their push-data start (the `push_data_cursor`) depends on the compiled layout of the stages before. A brand-new
  draw therefore needs up to 3 frames in skip mode (PS program, VS program, pipeline); stall mode does all of it in the first
  draw.
* Compute dispatches are never skipped: `GetComputeProgram` and `GetComputePipeline` always wait, in both modes.

### Thread-safety argument

1. **Program cache.** Unchanged from phase 1: `programs_mutex` guards the map, each `SourceEntry` has a `shared_mutex` for
   `resource_plan`/`resources`/`specialization`/`permutations`. Workers only call `SourceEntry::Publish` (exclusive lock,
   duplicate check). They never touch `resource_plan` or `resources`. `FindPublished` takes the shared lock.
   `stage_permutation_counts`/`next_shader_id` are atomics.
2. **Pipeline maps.** `m_pipelines_mutex` (`shared_mutex`): readers (`FindGraphicsPipeline`/`FindComputePipeline`) take it shared,
   publishers (workers and the synchronous fallback) exclusive. Taken only when `--async-pipelines` is on, so the off path does
   not pay for it. `Pipeline` objects are heap nodes (`unique_ptr`) never erased while the cache lives, so a `Pipeline&`
   returned to the draw path stays valid after the lock is dropped.
3. **Publish-then-unregister.** A job publishes its result first and unregisters from its in-flight registry last. So a key that is
   neither registered nor published has no job. Only the serialised GPU thread creates jobs (it holds the render context
   mutex), and it checks in this order: registry, published results, create. That makes duplicate jobs impossible; and
   if one ever happened `Publish`/`PublishGraphicsPipeline` still discard the second result (the duplicate module/pipeline is
   destroyed). Covered by `TestPublishThenUnregisterContract`.
4. **Vulkan.** `VkPipelineCache` is created with flags 0 (no `EXTERNALLY_SYNCHRONIZED_BIT`), so concurrent
   `vkCreate*Pipelines` on it is legal. `vkCreateShaderModule`, `vkCreateDescriptorSetLayout`, `vkCreatePipelineLayout` and
   the pipeline creation calls are externally-synchronised only on their output objects, which are worker-private until
   published. `GraphicContext` is read-only. Descriptor-heap/set state is only touched by the draw path on the GPU thread.
5. **Translator.** Phase 1 audit: no mutable global state in `TranslateProgram`/`CompileProgram`. The three plain `static bool`
   log-once flags that `CreatePipelineInternal` reaches (`shaders.cpp`: `logged_113`, `logged_121`, `OutOfBounds logged`) are now
   `std::atomic_bool`. `Bench::SetDriverCreateNs` is thread-local.
6. **Lifetime.** `PipelineCache::Save()` (called from `WindowRun` under the render-context lock and from the destructor) first runs
   `ShutdownAsync()`: stops the pool, cancels queued jobs, joins the workers (running jobs finish and publish), clears the
   registries. So the workers are gone before the driver `VkPipelineCache` is read/destroyed, before the device is destroyed
   and before `Config::Shutdown()`. Because `Save()` holds the render-context lock, no draw can be inside `Wait()` at that
   point. A draw after shutdown finds no running pool and takes the stock synchronous path.

## Tests

`ctest -j4` in `kyty/build/phase2-581`: **50 / 50 passed** (47 from phase 1 + 3 new; log `kyty/ctest_phase2_581.log`).
* `async_pipeline` (`tests/AsyncPipelineTests.cpp`, no Vulkan): jobs run concurrently on N worker threads (a barrier only
  passes if N run at once); dedupe: a second `Insert` joins the first job, 8 waiters wake, body runs once, `Find(count_skip)`
  accumulates `skipped`; the publish-then-unregister contract with 3 concurrent readers over 400 keys x 2 rounds (never "neither registered nor
  published", each key built once, no torn read); 2000 jobs on 8 workers; shutdown cancels queued jobs (`Wait()==false`,
  `Cancel()` unregisters), waits for the running job, refuses new submits, is idempotent. Ran 300x in a loop and under
  ThreadSanitizer (clang `-fsanitize=thread`, standalone build of the three sources): clean.
* `shader_recompiler_compute_async_stall`: the whole GPU suite (the 20 s `shader_recompiler_compute_tests`) with
  `KYTY_TEST_ASYNC_PIPELINES=stall`, i.e. every compute program/pipeline and the hand-built graphics pipelines go through the real
  `ProgramJob`/`*PipelineJob` path with 4 workers.
* `graphics_pipeline_rasterization_async_skip`: `--polygon-mode-only` with `=skip`; `GetGraphicsPrograms`/`GetGraphicsPipeline` are retried while
  `pending`/`nullptr` (like the next frame's draw), then the pipeline checks must pass. The tail of `CheckRasterization` that drives
  `RenderExecutor` draws is skipped in this mode (those draws are dropped by design). This test found a real gap in the test
  scaffolding only (callers had to handle `pending`); no product bug.
* Existing tests were adapted for the `GetGraphicsPipeline` pointer return only.

## Sanity runs (Astro Bot, route `intro90`, one fresh run per mode)

Route `bench/routes/intro90.json` = `intro150v2` with `run_seconds` 95 and only checkpoint `p036` (P+36 s); `p080` would never fire in
90 s. Configs `bench/config-phase2-{off,stall,skip}.json` (binary `kyty/build/phase2-581/kyty_emulator` = `058ed47`, flags as
`config-phase1-timing.json` + `--async-pipelines <mode>`, default 8 workers). Runs: `20261011T165904Z-phase2-off`, `...170059Z-phase2-stall`,
`...170246Z-phase2-skip`, each with `--shader-timing-log shader_timing.csv` and `--bench-frames-csv frames.csv`.

All three runs: valid, every hard control PASS, `analyze.py` checkpoint verdict PASS (dHash within 10 bits), exit code 0, no crash, no
"EXIT"/segfault text, no warnings from the pool. Soft `environment` WARN only (low MemAvailable/high load from concurrent builds of other
sessions; the skip run dipped to 0.9 GiB for 25 s, peak RSS 7.9 GiB).

Frame stats from `frames.csv` (dt between consecutive presents; "over 50 ms" counts frames, "total" is the sum of those frame times, as in RESULTS.md; the first column is the whole run, brackets are frames after the first 30 s):

| mode | frames | frames > 50 ms | total of those | worst frames (ms) | median / p99 ms |
|---|---|---|---|---|---|
| off | 3744 | 16 (9) | 18.5 s (13.0 s) | 11149, 4468, 565, 406, 381, 323 | 16.68 / 35.7 |
| stall | 3717 | 18 (11) | 18.8 s (13.0 s) | 11041, 4589, 557, 437, 382, 321 | 16.68 / 35.5 |
| skip | 4023 | 9 (1) | 10.8 s (0.07 s) | 5463, 4053, 463, 287, 227, 122 | 16.68 / 35.5 |

* **off**: equals the phase 1 timing run (19 frames, 18.4 s, worst 11.0 s in a 150 s run). No regression in the default path.
* **stall**: equals off within noise (so the foundation does not regress): 232 jobs, 232 GPU waits, 16.8 s of GPU-thread wait, peak queue 1
  (nothing runs in parallel in stall mode; that is expected).
* **skip**: 232 jobs, **1917 skipped graphics draws** (259 because a program was pending, 1658 because the pipeline was pending),
  78 GPU waits totalling 9.1 s. After the first 30 s only one frame over 50 ms (66 ms) remains; the 11 s frame at ~36 s became a 5.5 s
  frame at ~30 s. What is left is compute: `pipeline_create_cs` cs_id=95 took 4.43 s and cs_id=8 1.74 s with the GPU thread blocked
  for the whole time (`wait_ns` = duration, `skipped=0`), and the 4.1 s boot frame. The longest graphics creations (1.7-2.3 s) were
  hidden: `wait_ns=0`, `skipped` 2-456 draws each. Compute dispatches are never skipped, so they are the next lever (lookahead, D2-C2).
* One run per mode: the stall/skip differences in frame count (3717-4023 over 96 s) are not an FPS comparison; timing-log runs are not
  baselines. The full 3-vs-3 comparison is for the scheduled bench.
* Skipped draws were not image-diffed; the p036 checkpoint (planet scene) matched in all three. No visual review of the tunnel scene was done.

## Known risks and open points

1. **Skip can corrupt render-target and depth passes** (the Decision 1 dissent). A draw into an offscreen target that is only rendered once and
   is dropped stays empty. 1917 draws were dropped in this 95 s run. It stays behind the flag; stall is the safe mode.
2. **Compute still stalls the GPU thread** in both modes (4.4 s + 1.7 s here). Parallelism needs lookahead; with stall and one outstanding job
   the pool has nothing to parallelise (`peak_queue=1`).
3. **Skip needs up to 3 frames for a new draw** (PS program, VS program, pipeline), because the VS push-data start depends on the PS layout. A
   pending stage also stops the lookup of the later stages. Speculating the cursor could shorten this.
4. **New-source translation (frontend + plan extraction) stays on the GPU thread** (measured 2.6% of spike time); a very large shader can still stall.
5. **Shader ids are assigned at completion**, so with workers they are no longer in draw order (ids are only keys/log text). Two-boot replay diffs of SPIR-V
   are unaffected but any id-based golden output would change.
6. **EXIT* inside a worker** kills the process like today but from the worker thread; the log line is the same.
7. **Shutdown waits for running jobs** (up to a few seconds for a long compute pipeline) before `Save()` writes the driver cache; the harness' 15 s
   SIGTERM grace was enough here. Queued jobs are dropped (their pipelines are simply not in the saved cache).
8. **Pending skipped draws re-capture a job key per draw** (vector copies of the static key and specialization, about 1 KB); fine at this rate
   (1917 total), but a lookahead design should dedupe per frame.
9. **Memory**: each job copies up to 3 stage infos (a few KB) plus the code bytes; the in-flight set is bounded by the number of distinct misses.
10. Not covered: tessellation/mesh pipelines in a game run (the sanity route did not hit them; the unit/GPU tests exercise the same path), Vulkan validation layers with workers,
    and a second (warm-cache) run. `--pipeline-workers` other than 8 was not tried.
11. The `kyty_tests` build uses a non-ccache configuration; the build dir was reconfigured with ccache afterwards (only the emulator target was rebuilt).
