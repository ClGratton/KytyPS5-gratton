# Phase 1 notes (shaders.md Wk1: instrument, driver cache key, thread-safe ProgramCache)

**This is the `phase1-581` port.** Branch `phase1-581` of the KytyPS5 clone, based on upstream
`581dad7` (2026-10-01, newest commit that still boots Astro Bot; its child `337f432` introduces the
RT/BVH crash, upstream #1268), worktree `kyty/wt-phase1-581`, build dir `kyty/build/phase1-581`.
The original branch `phase1` (based on `16a7fe6`, worktree `kyty/wt-phase1`, build dir
`kyty/build/phase1`) is untouched. Local commits only, nothing pushed. Every flag is
off (or `revision`) by default; with no flags the behaviour equals stock. The async worker port is
**not** implemented: the plan gates it on timings from a real game (G1), and none exists yet.

Why the commits matter for the cache: `git describe --dirty` is baked into the binary and a
`-dirty` hash silently disables the stock-mode driver pipeline cache. Always build from a clean tree
(commit first, then build) and check `kyty_emulator --help | head -1` shows no `-dirty`.

## Commits

| SHA (phase1-581) | Original (`phase1`) | What |
|---|---|---|
| `af1caed` | `ef83735` | `--bench-frames-csv`, `--bench-exit-after-frames` (observation only) |
| `85d3667` | `57338e8` | `--shader-timing-log` (translation, pipeline creation, MaterializeResources) |
| `3f189f8` | `26f47a9` | `--pipeline-cache-key schema` (revision-proof driver VkPipelineCache key) |
| `e65e935` | `7a38936` | thread-safe `ProgramCache` |
| `0b56c24` | `5c7840d` | `docs/phase1-translator-audit.md` (written against `16a7fe6`, see port notes) |
| (dropped) | `740feeb` | S_SWAP_PC refresh scratch fix: nothing to fix at this base (no S_SWAP_PC machinery) |
| `b436ce7` | `8575b49` | follow-up: no specialization copy on the per-draw hit path |
| (this commit) | `7a4259b` | `docs/phase1-notes.md` |

## Port from 16a7fe6 to 581dad7 (what changed)

581dad7 is ten days older than 16a7fe6; the code the original branch touched had moved. Resolutions,
all keeping the original intent and the flag-gated, default-off behaviour:

* **`emulatorConfig.h`, `main.cpp`** (`af1caed`): HEAD-side options `sync_raw_image_buffers`,
  `trophy_enabled`, `skip_notice_screen` and their CLI flags do not exist at 581dad7; only the bench
  fields/flags were kept.
* **`swapchain.cpp`** (`af1caed`): 581dad7 has no `WindowLoopState::presented_frames` counter (it was
  added later); `Present()` just calls `window.UpdateTitle()`. The frame CSV now uses its own
  `static std::atomic<uint64_t>` counter, incremented only when `Bench::FramesEnabled()`, right after
  the successful Present (same position: after `UpdateTitle()`), so `frame_idx` is still the 1-based
  count of successfully presented frames. The exit path (push `SDL_EVENT_QUIT`) is unchanged and works
  the same way (`GameEventQuit` sets `need_exit`).
* **`pipelineCache.cpp` timing** (`85d3667`): 581dad7 has no S_SWAP_PC call-source linking
  (`ShaderSource`, `PrepareShaderSource`, `RefreshShaderSource`, `FunctionCode`). The timing was ported
  onto the simpler 581dad7 `Get()` (inline `find_if` over permutations, `skip_dispatch` early outs
  kept). Consequences: no `swappc_refresh` rows and no `summary_swappc_refresh_hit` row (the
  `HitSlot::SwapPcRefresh` slot was removed), `prepare_ns` is always 0 and `reused_permutation` is
  always 0 in the new-cache-entry/known-program paths here (it only ever fired in the S_SWAP_PC
  source-changed path, except that a lost publish race now also sets it, see below). Other row kinds and the row format are identical, so analysis scripts keep
  working. `shaders.cpp` (`driver_ns`) applied unchanged.
* **`pipelineCache.cpp` schema key** (`3f189f8`): applied without conflicts; the stock gates
  (Release, known revision, not dirty) are the same at 581dad7.
* **Thread-safe `ProgramCache`** (`e65e935`): re-done by hand on the 581dad7 struct. Same design
  (`programs_mutex`, per-entry `shared_mutex`, thread_local lookup key, atomic ids and per-stage
  counters, deque of permutations, duplicate check on publish) minus everything about call sources
  (`LinkedPtr`, `CallSnapshot`, `RefreshCallSource`, `HasCalls` variants, `function_code`).
  `skip_dispatch` became a `const` member fixed at entry creation (read without the entry lock; the
  skip entry is created via `InsertEntry(..., skip=true)`). `ShaderRecompiler.{h,cpp}` and
  `tests/shaderCfgTests.cpp` are therefore **unchanged** from 581dad7.
* **Follow-ups**: `740feeb` is dropped (it only fixed `ShaderSource::observed_function`, which does not
  exist here); `8575b49` was re-applied by hand to the new `FindPermutation`/`Materialize`
  (copy the specialization out only on a miss).
* Original build-time behaviour that does **not** carry over: the original `shader_cfg` regression
  from the S_SWAP_PC snapshot change cannot occur here for the same reason.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--bench-frames-csv <path>` | off | append `frame_idx,t_ns` per successful Present |
| `--bench-exit-after-frames <N>` | off (0) | clean exit once N frames were presented |
| `--shader-timing-log <path>` | off | CSV of translation / pipeline creation / materialize timings (no S_SWAP_PC rows at this base) |
| `--pipeline-cache-key revision\|schema` | `revision` | what the on-disk driver VkPipelineCache signature is keyed on |

### 1. Frame CSV and exit (`af1caed`)
* Hook: `swapchain.cpp`, right after `presented_frames.fetch_add` in `Presenter::Present`.
  `frame_idx` is the 1-based number of frames presented so far (the counter value after the
  increment). `t_ns` is `std::chrono::steady_clock` nanoseconds since the clock epoch, taken in the
  same call. Same clock as the shader-timing rows, so the two CSVs join on `t_ns`.
* File: opened in append mode; the header line `frame_idx,t_ns` is written only when the file is
  new/empty, so repeated runs into one file stay a single valid CSV. Use a fresh path per run.
* Buffered in memory; flushed at exit (`atexit` and `at_quick_exit`, because the emulator leaves via
  `std::quick_exit(0)`), when the exit-after count is reached, and every 32768 rows (crash insurance,
  about 9 minutes at 60 fps).
* `--bench-exit-after-frames N`: when frame N is presented the CSV is flushed and `SDL_EVENT_QUIT` is
  pushed. That is the same path as closing the window: main loop ends (`GameEventQuit`),
  `WindowRun()` saves the driver pipeline cache, `Execute()` calls `std::quick_exit(0)`. Exit status 0.
  It does not wait for the slow 1 s title-refresh timeout (the push wakes `SDL_WaitEventTimeout`).
* Cost with the flags absent: one relaxed `atomic<bool>` load per presented frame.
* Stage 1 acceptance from the harness spec (median fps within stock run-to-run noise) still has to
  be measured on a real run.

### 2. Shader/pipeline timing (`85d3667`)
Row format (header written once): `t_ns,kind,key_hash,dur_ns,thread,gpu_thread,detail`.
`t_ns` = step start (steady_clock, joins with the frames CSV), `thread` = OS tid, `gpu_thread` = 1 when
`GuestGpu::IsGpuThread()` (the guest GPU command thread), `detail` = `k=v;k=v`.

| kind | when | `dur_ns` | key_hash | detail |
|---|---|---|---|---|
| `program_translate` | every ProgramCache miss (one per translation) | translate_ns = prepare + frontend + plan + backend + module | `ProgramKeyHash` of the lookup key | `stage, shader_hash, shader_id, new_source` (1 = first sight of this program, 0 = known program, new permutation), `reused_permutation`, `prepare_ns` (decode/link), `frontend_ns` (`TranslateProgram`), `plan_ns` (`ExtractResourcePlan`), `materialize_ns`, `backend_ns` (`CompileProgram` + validation + dumps), `module_ns` (`vkCreateShaderModule`), `code_dwords` |
| `pipeline_create_gfx` | each graphics pipeline miss | `CreatePipelineInternal` (layouts + driver call) | `GraphicsPipelineKeyHash` | `vs_id, ps_id, tess, driver_cache` (0/1), `driver_ns` (the `vkCreateGraphicsPipelines` call alone) |
| `pipeline_create_cs` | each compute pipeline miss | same | compute shader id | `cs_id, driver_cache, driver_ns` |
| `materialize` | `MaterializeResources`: always (miss=1) when the `Get()` ends in a translation; on the hit path only if >= 200 us (miss=0) | the call | program key hash | `miss, stage, shader_hash` |
| `swappc_refresh` | (not emitted on phase1-581: no S_SWAP_PC machinery at 581dad7) | | | |
| `summary_materialize_hit` (`summary_swappc_refresh_hit` only on the 16a7fe6-based `phase1` branch) | at flush | total ns of all hit-path calls | 0 | `count, max_ns` (deltas since the previous flush; sum them) |
| `driver_cache_init` | once at start | init time | 0 | `result` (`loaded`/`empty`/`disabled`), `loaded_bytes`, `active` (cold vs warm marker) |

Reading it for gate G1: sum `dur_ns` of `program_translate` + `pipeline_create_*` (+ `materialize` and
`swappc_refresh` with `miss=1`) per spike, using `t_ns` overlap with frames whose frame time exceeds a
threshold; `gpu_thread=1` rows are the ones that stall draws. New-shader vs known-shader/new-state
misses (R4 safeguard 4): a `pipeline_create_gfx` whose `vs_id`/`ps_id` were first created by a
`program_translate` row just before it is a new-shader miss; otherwise it is known-shader/new-state.
Cold vs warm: `driver_cache_init` plus `driver_ns`.

The probes cost one relaxed atomic load each when the flag is absent. With the flag on, each `Get()`
adds a few `steady_clock` reads and atomics; per-draw rows are not written for hits, only counters.

### 3. Revision-proof driver cache key (`3f189f8`)
* `--pipeline-cache-key schema`: signature `KytyPCS:<kPipelineCacheSchemaVersion>:<vendorID>:<deviceID>:<driverVersion>:<pipelineCacheUUID>`
  (stock: `KytyPC1:<git revision>:...`). The Release / known-revision / not-dirty gates are skipped in
  this mode, so dev and dirty builds can use the cache. File location unchanged:
  `_PipelineCache/<TITLE_ID>.bin` relative to the CWD. Switching modes just invalidates and
  rewrites the file (different prefix). The driver still validates the blob itself.
* A title id is still required (a real game must be loaded), as before.
* Log lines to look for at game start: `Vulkan pipeline cache: key=schema (version 1)`, then
  `loading|initializing ...`, `loaded N bytes` or `initialized empty`; at exit `saved N bytes`.
* Bump `kPipelineCacheSchemaVersion` only when the file layout or signature meaning changes. Changes
  to the emulator's pipeline creation do not need a bump (blobs are content-addressed).

### 4. Thread-safe ProgramCache (`e65e935`) and audit (`0b56c24`)
See the commit message and the comment above `struct PipelineCache::ProgramCache`.
Summary: map mutex, per-entry `shared_mutex`, thread_local per-call lookup key, atomic ids,
deque of permutations, duplicate check on publish (a lost publish race logs `reused_permutation=1`).
The immutable `shared_ptr<const Linked>` S_SWAP_PC snapshots and the `shaderCfgTests.cpp` change of the
original branch do not exist here (see port notes). Translator audit: `docs/phase1-translator-audit.md`
(line numbers refer to `16a7fe6`; the S_SWAP_PC rows do not apply to this base).

## How to verify

```
export PATH=$HOME/.local/share/mise/installs/ninja/1.13.2:$PATH
cd /home/claudio/Documents/ps5-emulator/kyty
cmake -S wt-phase1-581 -B build/phase1-581 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
cmake --build build/phase1-581 --target kyty_emulator -j8
build/phase1-581/kyty_emulator --help | head -1     # git = ...-<n>-g<sha>, no -dirty
cmake --build build/phase1-581 --target kyty_tests -j8 && (cd build/phase1-581 && ctest -j4)
```
With a game (not available yet), per run from a fresh CWD:
```
kyty_emulator --game <dir> --bench-frames-csv frames.csv --bench-exit-after-frames 3600 \
  --shader-timing-log timing.csv --pipeline-cache-key schema
```
Checks: process exits 0 after about 3600 frames and `frames.csv` has 3600 rows (header + rows,
`frame_idx` 1..3600); `timing.csv` has `driver_cache_init`, `program_translate` and
`pipeline_create_*` rows and ends with `summary_*` rows; a second run from the same CWD logs
`result=loaded` and a much smaller `driver_ns`; a rebuilt/new-commit binary run from the same CWD still
loads the cache (revision mode would log `invalidating`).

## Test results
`ctest -j4` over the 47 tests registered at this base (43 unit/GPU tests built by `kyty_tests` + the 4
`emulator_*` CLI tests that need `kyty_emulator`; RTX 3080 Ti, so the GPU tests ran). The test set is
identical for both builds (there are 54 tests at `16a7fe6`; the 7 extra ones were added upstream
after 581dad7).

| Build | Result |
|---|---|
| Pinned stock `581dad7` (`kyty/build/pin-581dad7`, tests built in the same dir) | 47 / 47 passed |
| Port `phase1-581` (`kyty/build/phase1-581`) | 47 / 47 passed |

(For reference, original branch on `16a7fe6`: stock 54 / 54, `phase1` 54 / 54.)

Not tested (no game available, by instruction): the live Present hook and clean-exit path, real
timing rows from draws, schema-mode cache load/save with a title id, and any behavioural/perf
comparison against stock on a real run. The standalone CSV checks of the original branch were not
repeated for the port. What does exercise the changed code: `shader_cfg`, `shader_recompiler_compute`
(real `ProgramCache::Get` miss + hit + storage-reuse on the GPU), the four CLI tests (`--help` with
other flags). A standalone throwaway program (not committed) checked the bench CSV: header once,
append across runs, `at_quick_exit` flush, exit-after-N trigger, and the timing log rows/summary.
Builds: the branch tip was built (Release, clang/lld, one full emulator build plus `kyty_tests`); the
intermediate commits of the port were not built individually. No warnings in the changed files (grepped
the emulator log: only the known `-Wdeprecated-declarations` noise).

## Risks and open points
* **Not exercised on a game.** The first real run is the actual test of the Present hook, SDL quit
  path, `Shaders:`-counter equivalence and schema-cache load. Do one throwaway run with
  `--bench-exit-after-frames 600` before relying on the harness.
* **Dirty tree = no stock-mode driver cache.** Any tracked-file edit gives a `-dirty` hash. Commit
  before building the binary used for baselines. (`schema` mode ignores this, which is its point.)
* **Mixing cache modes in one CWD** just rewrites the file; but for a clean A/B the harness should use a
  fresh CWD per run as already specified.
* **ProgramCache concurrency is only half-solved by design.** Safe: different programs from different
  threads, and workers translating from snapshots. Not safe: two threads consuming `Get()` results of
  the *same* program concurrently (`input_info.stage.resources` aliases `entry.resources`). Also
  `PipelineCache::m_graphics_pipelines` / `m_compute_pipelines` are unsynchronised. Both are phase 2
  work and are called out in the audit.
* **Hit-path overhead (default build).** `Get()` now takes an uncontended mutex (map lookup) and one
  per-entry exclusive lock per stage per draw; the hit path makes no copies or allocations (the
  specialization is copied out only on a miss). I did not measure it. Expect noise-level cost, but compare
  median frame time stock vs phase1 in the harness before trusting it.
* **Timing flag overhead.** With `--shader-timing-log` on, the hit path adds ~4 `steady_clock` reads and
  2 atomics per stage per draw. Do not use timing-enabled runs as the FPS baseline.
* **Thread column.** `gpu_thread` is `GuestGpu::IsGpuThread()` (the guest GPU command thread), not the
  Vulkan presenter thread. Both execute `Present`; pipeline/program work only happens on the former
  today, so the column is a worker-vs-GPU discriminator, not a render-vs-present one.
* **Pre-existing hazard noticed, not fixed:** `permutations` was a `std::vector` with `reserve(8)` and
  callers kept `Permutation*`/`&permutation.program` across later pushes (stock). p1 switched to a
  deque, which removes it.
* **Audit caveats:** pointer-keyed `unordered_map`s in the translator (see audit section 6) were only
  checked by grep for iteration; replay determinism must be validated by diffing SPIR-V.

## What remains gated
* **G1 (Wk1 d3)**: needs real timings on 2+ titles from `--shader-timing-log` + frames CSV: translation
  + pipeline creation >= 50% of draw-path spike time, MaterializeResources not
  dominant. The audit half of G1 is done (statics are fixable in days; nothing to fix in the translator).
* Async worker port: split `GetGraphicsPrograms`/`GetGraphicsPipeline`, captured job, pool port from
  KytyPlus (GPL notices), in-flight dedupe, skip-on-miss flag (default = synchronous wait).
* G2 persisted job records + two-boot replay diff; G3 boot warm-up; stored SPIR-V keyed by guest-byte
  hash + specialization + translator build hash.
* Decision 2 Step 0 (layout invasiveness, stall p95/p99 cold vs warm) can start from the same timing
  CSV; no GPL code was written.
* `PipelineCache::m_graphics_pipelines` / `m_compute_pipelines` are still unsynchronised (GPU thread only).
