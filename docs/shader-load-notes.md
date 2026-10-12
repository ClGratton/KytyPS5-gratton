# Shader-load prefetch (`--precompile-on-load`, `--pipeline-variants-replay`)

Branch `shader-load-prefetch` (from `gratton/gratton-next` 47902cb3). Both flags default off and need
`--async-pipelines stall|skip` (the builds run on the worker pool).

## Why
At the pinned settings the desert runs at the 60 Hz cap in about 90 % of frames; what is left is first-play
compile spikes. Compute pipelines dominate (84 pipelines, 53.7 s of driver compile, 4.0 s the longest),
graphics are 142 pipelines and 35 s. PM4 lookahead (`--pipeline-lookahead`) sees a pipeline about a frame
before its draw, which is nothing against a 4 s driver call. This work asks for seconds of lead by building
the pipeline when the game *registers* the shader, long before it dispatches it.

## The hook
`AgcCreateShader` (`libs/agc.cpp`) is the earliest point Kyty sees a shader binary: it maps the binary
(`ShaderMapUserData`, which hashes the code), patches the program address into the header's SH register
list, and returns the header to the game. After the patch, `PipelineCache::NoteShaderRegistered` gets the
registration (code address and hash, `dispatch_modifier` of the header's `ShaderSpecialRegs`, and the register
list). On the guest thread it only decodes the register list, copies the code and queues a job; everything
else runs on a worker.

What is known at registration, what is not (compute):

| program key part | at registration |
|---|---|
| code hash and size | yes |
| user SGPR count, LDS size, thread-group size, float mode, TGID/TG-size enables | yes: the header's `COMPUTE_PGM_RSRC1/2`, `COMPUTE_NUM_THREAD_*` decoded by `HW::ApplyComputeShaderRegister` (the PM4 decoder's own function) |
| wave size, thread-dimension mode | guessed from `dispatch_modifier` (the bits the guest library ORs into `DISPATCH_INITIATOR`) |
| queue (async compute) | no: guessed graphics queue |
| resource specialization (buffer stride/format/OOB mode, image class/dimension/mips/format) | **no**: it comes from descriptors the guest has not built yet |

The last row is why a registration-time build is a guess. The SPIR-V (and therefore the 4 s driver compile)
depends on the specialization, so a wrong guess builds a module nobody uses.

## Load jobs
`ProgramCache::LoadJob` (kind `Program`) on the pool's **low-priority queue** (at most half of the workers, only when the
normal queue is empty): translate (a call-source / `S_SWAP_PC` program is left to the GPU thread), extract the resource
plan and insert it as a program entry (first insert wins, so the first dispatch finds a known source even when no variant
could be built), compile one specialization, publish the permutation like a program job, queue the
`ComputePipelineJob` on the low queue. A dispatch with an equal key finds the program and the pipeline ready or in flight
(it joins the load job or the pipeline job; `WaitFor` promotes a low-priority job to the front of the normal queue, commit
"WaitFor promotes a low-priority job"). A dispatch that needs another specialization or static state never waits for a
load job: it builds its own, exactly as without the flag.

* `--precompile-on-load`: with no stored variant for the binary, build the **guess**. `IR::GuessSpecialization` is the specialization of a
  cleared descriptor per class (plain byte buffer, default OOB mode; float images of the instruction's dimension with one mip). It refuses
  typed buffers, bindless tables and programs that read guest memory to specialize (`skipped_unguessable`).
* `--pipeline-variants-replay`: the first dispatch that uses a compute permutation records its key in
  `_PipelineCache/<TITLE>.variants` (`Variants::VariantStore`: binary hash and size, static translator state, specialization; no
  code, no SPIR-V; checksummed append-only records, torn-tail repair, schema versioned; format in `variantStore.h`). The next session
  builds **every stored variant** of a binary when the game registers it, exactly as used. Combined with `--precompile-on-load`, a
  binary with stored variants gets only those; a binary without gets the guess.

Dedupe: an active load job with the same program key and the same stored variant (or both guesses) is not queued twice; a job whose
specialization is already published stops (`skipped_known`); a stored variant that no longer fits the translated plan is dropped
(`skipped_stale`, `SpecializationFits`).

## Counters and rows
Exit line `Pipeline load: ...` (and `summary_load` row): registrations, jobs (stored/guess), skips by reason, programs built, duplicates, pipelines
queued, `program_hit` (first dispatch used a load-built program), `program_other` (the binary was loaded but this dispatch's variant
differs), `pipeline_hit_ready` / `_inflight`, `recorded`, and the lead (register to first dispatch) of the hits. Timing-log rows:
`shader_register`, `load_program`, `load_hit`, `load_pipeline_hit`, `load_miss`, `compute_variant` (the full variant of every first
use, text), `variants_init`. `bench/shader_load_stats.py RUN` prints the join.

## Tests
`pipeline_variants` (store format, torn tail, corrupt record, schema, dedupe), `async_pipeline` (promotion), `shader_cfg`
(variant key conversions: the rebuilt inputs give the same `ProgramKey` static state; header register decode),
`resource_materialization` (guess), `shader_load_precompile` (GPU: registration builds program and pipeline, the first dispatch
hits both, another queue is another variant, a repeated registration builds nothing, record then replay of an async-queue variant),
`shader_recompiler_compute_shader_load` (the whole GPU suite with tracking on).

## Credits
The idea of a persisted shader/pipeline journal replayed on background threads at the next launch comes from the community forks
surveyed in `forks/` (TheCruZ 6101f2da6 / 47f13b286, BryanKAdams 9a458e104 / f79a100d5, Jetsku `shaderPrecompile` / `KYPLST1`,
cx recipes). Nothing is ported: this is a re-implementation around Kyty's own program cache, with its own file format.

## Measured
(see `bench/RESULTS.md`, section "shader-load prefetch")
