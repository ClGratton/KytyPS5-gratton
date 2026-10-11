# Phase 1 translator static-state audit (shaders.md, Wk1 days 1-3)

Question: can `TranslateProgram` / `CompileProgram` / `CreatePipelineInternal` run on worker threads
(C1a, items 4-5) without hidden shared mutable state, and what has to change first?

Scope: everything under `src/graphics/shader/` (frontend, IR, emitter, `shader.cpp`), the
`PipelineCache` wrappers that call it, and what those call into (Config, logging, Vulkan, Xbyak).
Method: grep for `static`, `thread_local`, `extern`, `inline` variables, namespace-scope variables,
`mutable`, `getenv`/`Singleton`/`rand`/locale calls, `Config::`, logging, then read each hit.
Line numbers: stock `16a7fe6` unless marked `p1:` (phase1 branch at the ProgramCache commit).

> Port note (`phase1-581`, base `581dad7`): this audit was written against `16a7fe6`. At 581dad7 the S_SWAP_PC
> call-source machinery (`ShaderSource`, `PrepareCallTarget`, `RefreshShaderSource`) does not exist yet, so
> the rows about it and about the immutable `Linked` snapshot do not apply; line numbers will differ.

**Result in one line:** the translator itself has no problematic mutable global state. The real
obstacles to worker translation are borrowed memory (guest spans, raw pointers in `input_info`),
GPU-thread-only scratch inside the retained `ResourcePlan`, and the cache entry aliasing in
`ProgramCache::Get`, not statics. Two plain-`bool` log flags exist on the capture side.

Risk scale: **none** (safe as is), **low** (benign race or trivially fixed), **medium** (needs a code
change before workers), **high** (would corrupt or crash if ignored).

## 1. Mutable statics and globals

| # | Where (file:line) | What | Risk | Fix for worker translation |
|---|---|---|---|---|
| 1 | `graphics/shader/recompiler/backend/spirv/spirvEmitterFlow.cpp:601` | function-local `static std::atomic_bool logged` (log-once flag) | none | atomic; none |
| 2 | `graphics/shader/recompiler/frontend/translate/Memory.cpp:961` | function-local `static std::atomic_flag warned` | none | atomic; none |
| 3 | `graphics/shader/shader.cpp:49-50` | `g_shader_map` (`unique_ptr<unordered_map>`) + `g_shader_map_mutex`; all accesses lock the mutex (`:66`, `:73`) | none | already synchronised; this is the guest-shader-address map used while capturing `ShaderParams`, i.e. GPU-thread side |
| 4 | `graphics/shader/shader.cpp:142` (`vs_check`), `:239`, `:247` (`ps_check`), `:277` (`cs_check`) | function-local `static bool logged` (plain, non-atomic) | low | data race only if two threads run the register checks at once. They belong to `PrepareProgram`, which stays on the GPU thread in the plan. If that ever changes, make them `std::atomic_bool` (4 one-line edits) |
| 5 | `graphics/shader/shader.cpp:193,204,229,256` | function-local `static std::atomic_uint log_count` | none | atomic |
| 6 | `pipelineCache.cpp:118,137` | `DumpShaderSpirv` / `DumpShaderOriginal`: `static std::atomic_int id` | none | atomic. Dump file naming order becomes nondeterministic with workers (cosmetic) |
| 7 | `pipelineCache.cpp:817,882` | `GetGraphicsPipeline`: `static std::atomic_bool warned`, `static std::atomic<uint32_t> log_count` | none | atomic |
| 8 | `pipelineCache.cpp:448` | `std::printf("Shaders: VS ...")` after each miss | low | stdio is thread-safe per call. Counts came from walking `programs`; p1 now uses per-stage atomics (`stage_permutation_counts`) |
| 9 | `ProgramCache::lookup_key` (`:472`), `next_shader_id` (`:474`), `programs` (`:471`) | shared mutable members | **high** (was) | fixed in p1: thread_local per-call key, atomic id, `programs_mutex` |

No `thread_local`, `extern` variables, `inline` non-constexpr variables, `getenv`, `Singleton`,
`rand`, `strtok`, `localtime` or locale use was found anywhere under `graphics/shader/`. All other
`static`/`inline` hits are `constexpr` tables or functions. Xbyak's error state used by the SRT JIT
(`SrtWalker.cpp:435`, `:489`) is `thread_local` in the bundled header
(`xbyak.h` `local::GetErrorRef`, `XBYAK_TLS`), so JIT building is thread-safe.

## 2. `mutable` members (const methods that write)

| Where | What | Risk | Fix |
|---|---|---|---|
| `ir/ShaderIR.h:606-612` (`ResourcePlan`) | `evaluation_contexts`, `evaluation_value_count`, `evaluation_depth`, `active_sources`, `visited_blocks`, `pending_blocks`, `material_keys`: comment says "GPU-thread scratch" | **high** if a retained `ResourcePlan` is used from two threads | `MaterializeResources(const ResourcePlan&, ...)` writes this scratch even though the plan is `const`. p1 calls it only under the owning `SourceEntry::mutex` (exclusive). A worker must never run `MaterializeResources` on the cached plan; it gets a captured specialization instead (plan item 4). Moving to per-call scratch would let it run lock-free |
| `ir/Value.h:173-174` (`Inst::evaluation_index`, `definitions`), `ir/Block.h:84` (`Block::definition`) | per-instruction evaluation memo | low | live inside one `Program`/IR owned by one translation, so a worker owning its IR is fine. Never share an IR between threads |

## 3. Config getters

`Config::` is read in exactly these translator-adjacent places: `pipelineCache.cpp:115,133` (dump
enabled + log folder; the folder getter returns a copy), `:152` (`ShaderValidationEnabled`), `:390`
(`GetShaderLogDirection` -> `options.dump_ir`), `shader.cpp:347` (`GraphicsDebugDumpEnabled`), and
`host_gpu/renderer/debug.cpp:57` (`graphics_debug_dump_enabled`: `GraphicsDebugDumpEnabled` +
`GetPrintfDirection`, called from `CreatePipelineInternal` in `shaders.cpp`). No Config getter is
called inside `graphics/shader/recompiler/`.

Risk: low. `g_config` (`emulatorConfig.cpp`) is written once in `Config::Load` and read-only
afterwards; getters are plain reads (path getters copy). Required for workers: workers must be
joined before `Config::Shutdown()` resets `g_config` (subsystem destruction order), and tests must
not call `Config::Load` while a worker runs. Better for the job format: read the flags once on the
GPU thread and put them into the captured job (`dump_ir`, `validate`, `dump folder`).

## 4. Borrowed memory: what a job must copy

These are the actual blockers; none is a static.

| What | Where | Why it blocks | Fix |
|---|---|---|---|
| `ShaderParams::code` is a `std::span` over guest memory | `shader/shaderCompiler.h:17`; consumed by `PrepareShaderSource` (`ShaderRecompiler.cpp:584`), `TranslateProgram(span)` (`:693`), `DecodeFusedProgram`/`back_code` (`:457`) | guest can overwrite or unmap while a worker decodes | copy `code` and `back_code` into the job when capturing (they are small) |
| `CompileOptions::user_data`, `back_code` spans | `ShaderRecompiler.h` (`CompileOptions`); `pipelineCache.cpp` `Get()` | spans into the caller's `ShaderParams` array | job owns a `std::vector<uint32_t>` copy |
| `CompileOptions::input_info` is a `ShaderStageInputInfo` **union of raw pointers** to the caller's `ShaderVertexInputInfo` / `ShaderPixelInputInfo` / `ShaderComputeInputInfo` (`shader/shader.h:200`) | read by `TranslateProgram` (`ShaderRecompiler.cpp:766-781`, embedded vertex fetch), `CollectShaderInfo` (`:877`) and `Spirv::EmitProgram` (`:888`) | pointee lives on the draw's stack; `Get()` also writes `input_info.stage` (program/resources pointers) back into it | job holds a by-value copy of the input-info struct and `options.input_info` is re-pointed at it on the worker. `Get()`'s write-back stays on the GPU thread |
| `SrtRuntime::user_data`, `shader_base`, `read_specialization_memory` | `ir/passes/SrtWalker.h:14`; callback is `ReadShaderGuestMemory` -> `Libs::LibKernel::Memory::TryReadBufferBacking` (`kernel/memory.cpp:879`) | live guest-memory reads | only `MaterializeResources` (`ResourceMaterialization.cpp:1071`, reads at `:180-195`) and `RefreshShaderSource` (`ShaderRecompiler.cpp:605`) use it. `TranslateProgram`/`CompileProgram` do not use `SrtRuntime` (confirmed: `read_specialization_memory` appears nowhere in the recompiler outside `ir/passes/`). Keep both on the GPU thread |
| `S_SWAPPC_B64` call targets | `PrepareCallTarget` (`ShaderRecompiler.cpp:484`), `RefreshShaderSource` (`:605`) | callee code is re-read from guest memory on every draw and may change (`revision`) | p1 makes the published `Linked` snapshot immutable (`shared_ptr<const Linked>`), so a worker translating from a snapshot is not invalidated by a later refresh. Treat these shaders as non-replayable (R3) |

## 5. Cache-entry aliasing in `ProgramCache::Get`

* `input_info.stage.resources = &entry.resources` (stock `pipelineCache.cpp:359,431,440`) points at state overwritten by
  the next `Get()` of the same program (`MaterializeResources` runs every draw). The unit test
  `workgroup table cache-hit storage reuse` (`tests/ShaderRecompilerComputeTests.cpp`) relies on
  that pointer staying stable, so it must keep working. Result: two threads must not consume
  concurrent `Get()` results for the same `ProgramKey`. Safe today (one GPU thread). Fix = the
  captured-job split of phase 2; do not "fix" it by copying `resources` per draw.
* `Permutation*` handed out, then `permutations.push_back` (stock `:436`) could reallocate the
  vector (`reserve(8)` only delays it). p1: `std::deque`.
* `RefreshShaderSource` edited `source.linked` in place (`optional<Linked>`, including the
  `observed_function` buffer on every refresh, even on a hit). p1: immutable shared snapshot,
  scratch moved to `ShaderSource`.

## 6. Other dependencies of the worker-side calls

* Logging: `LOGF`/`Log::Write` take `g_logger_mutex` (`common/logging/log.cpp:73,83,103,173`), so they
  are thread-safe. Translator phase logs (`ShaderRecompiler.cpp:66,725-788`, `CompileProgram`) will
  interleave across workers. Cosmetic.
* `EXIT*` / `EXIT_NOT_IMPLEMENTED` inside the translator terminate the whole process from whichever
  thread hits them (same as today, now on a worker). They are hit on unsupported shaders, so a worker
  crash looks like a GPU-thread crash. Worth catching at the job boundary later for better logs.
* Vulkan: `CompileSPV` (`host_gpu/vulkanCommon.cpp:127`) is a bare `vkCreateShaderModule`
  (`createShaderModule`); no externally-synchronised object is touched. The driver `VkPipelineCache`
  is created with flags 0 (`pipelineCache.cpp` `InitializeDriverCache`), i.e. without
  `EXTERNALLY_SYNCHRONIZED_BIT`, so concurrent `vkCreate*Pipelines` on it is allowed by the spec (the
  council note asked to confirm this). `PipelineCache::Save()` destroys the cache; it must run only
  after workers are quiesced. `CreatePipelineInternal` also creates descriptor/pipeline layouts and
  reads `GraphicContext` limits; reads only. Descriptor-set/heap state (`DescriptorHeap`, keyed by
  layout) is touched later by the draw, on the GPU thread.
* `m_graphics_pipelines` / `m_compute_pipelines` maps in `PipelineCache` are unsynchronised (header
  says the renderer serialises access, `pipelineCache.h:102`). Phase 2 needs a mutex or a
  GPU-thread-only publish step.
* Determinism (for the R3 two-boot replay diff): the translator keys several `unordered_map`/`set`s by
  raw `IR::Inst*`/`Block*` (`Tessellation.cpp:165,215`, `backend/spirv/spirvEmitterProgram.cpp:54,59`,
  `ir/passes/SsaRewrite.cpp:51,290`, `ReadLaneElimination.cpp:39,62`, `spirvEmitterInternal.h:250-251`).
  A grep found only lookups, no range-for over them, so output order should not depend on pointer
  values, but this was not proven: the replay diff must compare SPIR-V byte for byte.

## 7. Verdict against gate G1 (audit half)

"statics fixable in days": yes. There is nothing to fix in the translator proper; the work is in the
job capture (section 4) and in finishing the entry/aliasing split (section 5). Estimated
extra work before the async port: switch the 4 plain `bool` flags to atomics only if `*_check` ever
leaves the GPU thread; copy `input_info`/spans into a job struct; give `PipelineCache` pipeline maps
a publish step. The timing half of G1 (translation + creation >= 50% of spike time,
MaterializeResources / S_SWAP_PC not dominant) still needs a real game run with
`--shader-timing-log`.
