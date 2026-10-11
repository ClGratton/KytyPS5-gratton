# Lookahead notes (shaders.md D2-C2: PM4 command-buffer lookahead, stall-never-skip)

Branch `lookahead-581` (from `phase2-581` tip `f218edb`), worktree `kyty/wt-phase2-581`, build dir `kyty/build/phase2-581`.
Flag: `--pipeline-lookahead true` (default off; needs `--async-pipelines stall|skip`, otherwise ignored).
Debug flag for the feasibility trace: `--bench-gpu-trace true` (needs `--shader-timing-log`).

## Commits

| SHA | What |
|---|---|
| `a17f838` | `--bench-gpu-trace`: `gpu_submit / gpu_start / gpu_end / gpu_draw / gpu_miss` rows in the shader timing log |
| `1d15b6f`.. (see `git log`) | shadow CommandProcessor, lookahead thread, compute prefetch, graphics prefetch, accounting, tests |

## Step 1: feasibility (one trace run, `routes/intro90.json`, stall mode, fresh cache)

Run `bench/runs/20261011T170850Z-lookahead-trace`; tools `bench/lookahead_trace_analysis.py`, `bench/lookahead_sim.py`.

Submission stream (95 s): 29,855 submissions (18,340 graphics, 7,845 compute queue, 3,670 suspend points), 956k draws+dispatches.
Graphics buffers: p50 1,581 dwords, p90 24,807, max 42,541.

* **Submit -> first execution on the GPU thread: p50 22.7 ms, p90 26.7 ms, p99 33.3 ms** (the guest runs about one frame, 23 ms, ahead of the
  GPU thread; the maximum 11 s is a buffer queued behind a stall). At a miss there were a median of 7 and at most 13 submitted-but-not-started buffers.
* 232 misses: 127 program, 66 graphics pipeline, 39 compute pipeline. Lead time between guest submit and the draw that missed, measured on the stall
  timeline: p50 513 ms, p95 4.9 s (since the start of execution of that buffer: p50 303 ms, p95 4.65 s). These numbers are inflated by queueing behind
  earlier stalls; without the stalls the lead is the 23 ms above. Compute alone: p50 491 ms / p95 3.9 s, graphics p50 795 ms / p95 6.3 s.
* Misses cluster in a few big buffers: one 24k-dword buffer held 16 graphics/compute pipeline misses that the GPU thread built one after the other
  (6.3 s serial; about 1.5 s on 8 workers).
* Replay of the 232 measured misses on 8 workers (request time = submit of the buffer that contains the draw, creation times as measured, 1.0-1.5x
  slowdown for contention; baseline pipeline stall 16.7 s):

  | request lead before the draw | total pipeline stall | frames-sized stalls (> 50 ms) |
  |---|---|---|
  | none (request at the draw, = stall mode) | 16.7 s | 38 |
  | scan the buffer when it starts executing | 8.0-12.2 s | 11-13 |
  | submit time, 23 ms lead | 7.6-11.7 s | 10-12 |
  | 100 ms | 6.7-10.6 s | 6-8 |
  | 500 ms | 4.3-7.4 s | 3-4 |
  | 2 s | 1.7-4.2 s | 1-2 |

**Decision.** The lead time of a submit-time hook is about one frame, which is ~0 compared with a 1-4 s pipeline. But the gain does not need lead:
the 8 workers run the independent pipelines of a buffer (and of the 7-13 queued buffers) in parallel instead of one after the other, which the model
puts at -30 to -55% of the stall and about a quarter of the frame-sized stalls. Built. What no submit-time hook can hide: the single longest compiles
(3.6-4.8 s compute, 1.2-2.8 s graphics) and the boot burst. Earlier hooks, with numbers from the replay:

* *Guest-side command-buffer building* (agc/gnm calls): at best one more frame (~25 ms) of lead -> 23 ms vs 50 ms in the table, a few percent. Not worth it.
* *"Next frames" prediction from the previous frame's misses*: first-play misses are new by definition. No gain.
* *Shader-load hook + persisted variant list* (record the lookahead's requests per shader in one session, replay them when the game registers the shader in
  `ShaderMapUserData` in the next session): this is the only route to seconds of lead (the 500 ms-2 s rows), but it is repeat-play (already solved by the warm
  driver cache) unless a manifest is shipped. shaders.md items 8-9 (persisted job records, gated warm-up).
* *Cheaper compiles*: the 3.6-4.8 s compute pipelines are one driver call each; neither lookahead nor workers shorten them.

## Step 2: what was built

```
guest thread  Submit()/SubmitCompute()/SuspendPoint -> GuestGpu::Enqueue: copy the PM4 stream (dwords) -> lookahead queue
lookahead thread (kyty-lookahead)  one shadow CommandProcessor per guest queue, same decoder and register tracking
        draw     -> RenderExecutor::PrefetchDraw   -> PipelineCache::PrefetchGraphicsPrograms / PrefetchGraphicsPipeline
        dispatch -> PipelineCache::PrefetchCompute
        -> program jobs and pipeline jobs on the existing worker pool (dedupe with the GPU thread's own requests)
GPU thread   unchanged draw path; it finds the pipeline published or the job in flight
```

| Piece | Where |
|---|---|
| Shadow CommandProcessor: `SetShadowSink()`; in shadow mode every side effect is a no-op (memory writes, DMA, EOP/events, flips, barriers, waits, predication, const-RAM dump, GET_LOD_STATS) and draws/dispatches go to the sink | `guest_gpu/graphicsRun.cpp`, `command_processor/commandProcessor.h`, `command_processor/shadowTypes.h`, `pm4Handlers.cpp` (one guard) |
| Lookahead thread: queue, one shadow CP per queue id, resets on suspend points, skips submissions the GPU thread already started (`items_late`) | `guest_gpu/pipelineLookahead.{h,cpp}` |
| Compute prefetch: same program key as `GetComputeProgram` (cs registers, user SGPRs, `mode` bit 5), program job, pipeline job keyed by program id | `PipelineCache::PrefetchCompute` |
| Graphics prefetch: mirrors `DrawIndex/DrawAuto -> PrepareDrawRenderState -> ExecutePreparedDraw` up to the pipeline lookup | `RenderExecutor::PrefetchDraw` (renderDraw.cpp) |
| Key exactness by construction: the pipeline key is built by the same functions as the draw path | `PipelineCache::BuildGraphicsKey` (split out of `GetGraphicsPipeline`), `DescribeRenderColorTarget`, `DescribeRenderDepthTarget`, `DepthStencilCopyPlan` (split out of the resolve functions; the image lookup stays in the draw path), `BuildTargetExportMapping` |
| Program lookups on the lookahead thread: `ProgramCache::Get` with `AsyncContext::prefetch` | `pipelineCache.cpp` |
| Pool: `Submit(job, urgent)`; `WaitFor()` moves a still-queued job to the front ("nearest draw wins"); the draw path submits its own misses urgent | `asyncPipelineCompiler.*` |
| Speculation guard (see risks) | `common/speculation.{h,cpp}`, `common/assert.cpp` |

How the prefetch differs from `ProgramCache::Get` on the GPU thread (the places where sharing state with the GPU thread would be unsafe):

* `MaterializeResources` writes `entry->resources` / `entry->specialization`, which the GPU thread reads without a lock after `Get()` returns. The prefetch
  materializes into a private snapshot (`SourceEntry::PeekPermutation`, `MaterializePrivate`) and never touches them. It still takes the entry's exclusive lock,
  because `MaterializeResources` keeps mutable scratch inside the `ResourcePlan` (evaluation contexts, read capture) that the GPU thread uses under the same lock.
* Guest memory is read with `TryReadBacking` (`ReadShaderGuestMemorySpeculative`), not `TryReadGpuCleanBacking`, which refuses every guest address off the
  GPU thread. A descriptor the GPU has written but not flushed can be stale: that predicts a wrong key, it cannot corrupt anything.
* Vertex input tables (attribute and V# tables behind user SGPRs) are read raw by the draw path; on the lookahead thread they go through the bounds-checked
  backing reader and an unreadable table drops the request (`Speculation::NoteFailure`).
* New sources are translated on the lookahead thread (frontend + resource plan), exactly like the GPU thread does for a miss; `InsertEntry` is first-wins.
* Program jobs are waited for on the lookahead thread (the push-data start of the VS depends on the compiled layout of the PS), pipeline jobs are not.
* Stage infos copied into prefetched jobs carry a shared empty `ResourceSnapshot` (stage must look valid; workers never read it).
* Dispatch with a direct group count of 0 and graphics draws that become something else in the draw path (color-buffer decompress/resolve modes, depth/stencil copy,
  no vertex shader, unsupported primitive, no target, rect-list without exports, tessellation) are not prefetched; indirect draws/dispatches are (arguments unknown).

Counters (`Pipeline lookahead: ...` line at exit and a `summary_lookahead` row in the timing log): `*_seen`, `*_skipped` (+ `skip_<reason>`), `*_requested`
(jobs the lookahead created), `*_already` (published or in flight), `*_hit_ready` / `*_hit_inflight` (first use by a draw/dispatch of a pipeline that a
prefetch built; "in flight" = the draw had to wait for it), `*_miss` (the draw had to create its own job). Rows of the pool in the timing log get `prefetch=` / `claimed=`.

## Step 3: results (route intro90, 1 vs 1, fresh cache each, `--async-pipelines stall`, timing log on in both)

Runs `20261011T173136Z-final-stall` (binary `bf37209`, no lookahead flag) and `20261011T173323Z-final-lookahead` (`bf37209`, `--pipeline-lookahead true`);
a second lookahead run `20261011T173709Z-final-lookahead2` on the final binary `f62c659` for repeatability. Configs `bench/config-phase2-stall.json`, `bench/config-lookahead.json`.

| | stall | stall + lookahead | lookahead run 2 |
|---|---|---|---|
| frames > 50 ms (all) | 18 / 18.84 s | 13 / 10.19 s | 13 / 9.54 s |
| frames > 50 ms after 30 s | 10 / 13.14 s | **6 / 1.00 s** | **6 / 0.98 s** |
| worst frames (ms) | 7467, 4532, 4018, 568 | 5467, 3037, 528, 260 | 5461, 2537, 521, 221 |
| GPU-thread wait on jobs | 16.85 s (232 waits) | **8.67 s (29 waits)** | 8.15 s (28 waits) |
| of which compute / graphics / programs | 8.20 / 8.44 / 0.22 s | 5.40 / 3.24 / 0.77 s | 4.97 / 3.17 / 0.70 s |
| frames presented in 96 s | 3709 | 4087 | 4131 |
| p036 checkpoint | PASS | PASS (1 of 64 bits) | PASS (1 of 64 bits) |

The prediction (lookahead run 1; run 2 is the same within one): **105 pipelines were needed (39 compute, 66 graphics); 101 were requested by the lookahead and
all 101 were used by a draw (82 already built, 19 still building when the draw arrived); 4 were not predicted (3 compute, 1 graphics) and were created by the draw
path. No wasted request.** 750k draws and 330k dispatches were looked at; 683k+317k of them found the pipeline published or in flight (`*_already`); 66k draws
were skipped, 34k because the draw is a color-buffer operation and 32k because they have no color/depth target.
The lookahead thread processed 32.9k submissions (13 of them after the GPU thread had started them), max queue 14, busy 12.4 s out of 96 s (13% of a core).

What is left: the stall at about 28-32 s (a burst of 4-5 heavy pipelines needed within a few frames: compute 4.8 s and 3.6 s, graphics 2.8 s under 8-way contention),
and the boot burst (the first compute pipelines of the game, 1.7-2 s, requested only ~0.2 s before use). Both are limited by the 5.3x throughput of the driver
and by the one-frame lead.

Difference is far outside run-to-run noise (frames after 30 s: 13.1 s vs 1.0 s of stutter; both lookahead runs agree), so no 3 vs 3.

## Tests

`ctest -j4` in `kyty/build/phase2-581`: 50 / 50 pass, including `async_pipeline` with the new `TestUrgentSubmitAndPromote` (urgent submit order, promote-on-wait,
prefetch claim). The shadow processor and the prefetch have no unit tests; they are covered by the game runs above (`aborted=0`, no speculative check failed,
zero wasted requests). `shader_recompiler_compute_async_stall` and the graphics tests exercise the refactored key builders (`GetGraphicsPipeline` is now
`BuildGraphicsKey` + the same lookup/job code; the register-only halves of `ResolveRenderColorTarget` / `ResolveRenderDepthTarget` are separate functions).

## Risks and open points

1. **Speculation.** The lookahead runs the real decoder and key code on buffers the GPU thread has not executed. The project is built with `-fno-exceptions`, so a
   fatal check (`EXIT*`) on that thread cannot be unwound: `Common::Speculation` makes the fatal handlers, on the lookahead thread only, print one line, release the locks the
   thread registered (`HeldLock`: the entry lock during materialization, the prefetch gate), disable the lookahead for the rest of the run and park the thread forever. The
   emulator keeps running. It never triggered in the 5 game runs (`aborted=0`), but a lock that is *not* registered and is held at the time of an `EXIT` would deadlock the GPU
   thread. Audited: `g_shader_map_mutex` (the miss is now a non-fatal failure, `f62c659`), `programs_mutex`, the job registries and the pool mutex are never held across code
   that can reach an `EXIT`. `ResourcePlan` scratch is left in an undefined state after an abort inside `MaterializeResources` (evaluation depth never decremented); only that
   plan is affected and only the lookahead is stopped.
2. **Shadow drift.** The shadow tracks registers from a copy of each submitted stream, in order, but cannot know GPU-written memory: indirect buffers, `COND_EXEC` and
   `BRANCH` conditions, indirect register loads read live memory, DMA/CE-dump/WRITE_DATA results are not applied. A wrong branch or stale table gives a wrong key, which costs
   worker time, not correctness: the draw path always computes its own key and uses what it finds. Over-prediction would waste worker time and add contention; here it was 0.
3. **Contention.** Prefetched heavy jobs share the 5.3x driver throughput; the first-needed job slows (~1.5x when 8 run together: compute 3.6 s -> 4.8 s). Urgent submits and
   promote-on-wait put what a draw is waiting on first, but cannot preempt running jobs. A dependency-aware order (nearest draw first among the queued ones) is the next step.
4. **Cost.** One extra thread at 13% of a core on this route; every submitted command buffer is copied once (<= 170 KB, ~33k copies, freed after decode). The GPU thread's `Enqueue`
   (under the queue lock) does the copy.
5. **Not covered.** `--async-pipelines skip` + lookahead (should drop far fewer draws than skip alone since 96% of pipelines are ready; not measured), tessellation
   (not prefetched), a second (warm-cache) run, Vulkan validation layers, more than one title, `--pipeline-workers` other than 8, long routes (the VRAM-exhaustion exit at ~165 s of the pinned base is unrelated).
6. **Not hidden.** Anything needed less than one frame after the guest produced it and costing seconds (boot burst, the 28-32 s burst). See Step 1 options.
