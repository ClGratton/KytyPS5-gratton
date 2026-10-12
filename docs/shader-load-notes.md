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

### Background priority: idle-only, in first-use order
The first measurement of the replay (bench/RESULTS.md) showed the cost of treating this as ordinary low-priority work. The 84 stored
variants were queued in the registration burst at boot (795 registrations in the first second) and four of them ran at a time beside the
draws' own compiles. The driver's compile throughput is shared (eight concurrent compiles double each one's time): the compile the first
frame waited for took 7.2 s instead of 3.7 s and the first frame 10 s instead of 7 s, with no gain later. Load jobs are therefore
**idle-only**: `SubmitLowPriority(job, idle_only=true, priority)` starts them only while no normal job is running or queued (still within the
low-priority slot limit), and the pool wakes a worker when it goes quiet. They run in ascending `priority`; replay uses the time the
variant was first needed in the previous session (`first_use_ms`, an optional trailing field of the record), so the earliest-needed
variants are built first.

### Graphics pipelines (`--pipeline-variants-replay` only)
The measurement window of a first play is dominated by graphics pipelines (0 of the 22 frames over 50 ms in the post-skip window wait for a compute
pipeline; 10 wait for graphics ones, 5.4 of 6.2 s). A graphics replay needs more than a hash:
* **Record**: the first draw that uses a vertex/mesh or pixel permutation puts its stage program into a side table
  (`gfx_programs`, program id to `ProgramVariant`: program key with its static-state words, push-data cursor, specialization, and the stage
  input info as raw bytes). The first use of a graphics pipeline stores the vertex and pixel programs plus the pipeline key
  (`EncodePipelineKey`, written field by field). Tessellation, S_SWAP_PC and fused back-half stages are not recorded.
* **Replay**: `NoteShaderRegistered` also sees graphics binaries. When a stored graphics pipeline needs the binary its code is copied
  (`CaptureGraphicsRegistration`); once every binary of a pipeline has been seen, one idle-only job (`GfxLoadJob`) rebuilds the stage programs
  (`BuildStoredStage`: translate, resource plan, compile with the stored specialization and cursor), decodes the key, fills in the new
  program ids, and queues the `GraphicsPipelineJob`. `graphicsVariantKeys.h` holds the encodings; the input infos are raw bytes behind a
  layout fingerprint (`VertexInfoLayout()`, `PixelInfoLayout()`) and a sanity check, so a file written by another layout is ignored.

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

## Findings (details and run directories: `bench/RESULTS.md`, section "shader-load prefetch")
* **Lead is there.** The game registers 1123 shaders (795 compute registrations, 660 distinct compute binaries) in the first second
  and uses 79 compute binaries in the measured route. Registration to first request: median 14 s, 58 of 79 above 5 s, 33 above 20 s;
  21 are under 1 s (the boot-critical ones, which no registration hook can help).
* **`--pipeline-variants-replay` works.** With the previous session's variants and empty driver and NVIDIA caches, 77 of 84 compute
  programs and 130 of 143 graphics pipelines were ready before their first dispatch or draw (a few more were joined in flight).
  Frames over 50 ms in the whole run: 30 s down to 6.5 s, the post-skip window 8.6 s down to 0.9 s, worst frame outside the first second
  165 ms. What is left is the first second: the shaders the boot needs are registered less than 0.4 s before the first frame needs them.
* **Background work must stay out of the way.** The first replay slowed the boot compile (7.2 s instead of 3.7 s) because the
  replay jobs competed for the driver; idle-only jobs fixed that (see above).
* **Two pitfalls found by running it:** an NGG vertex program with a GS back half has a program-key hash combining both binaries (the
  guest registers the front one under its own hash), and a graphics pipeline's expensive vertex programs are exactly these.
* **`--precompile-on-load` (the guess) does not pay on this title.** Every compute binary the route uses has typed buffers, bindless
  descriptors or a specialization that needs guest memory, so `GuessSpecialization` refuses all 79 of them; the 170 guesses it did build
  were for binaries the route never dispatches. 0 hits, no stutter change, and it translates binaries the game may never use (8
  of 685 hit translator checks; they run on an isolated speculative thread so they cannot end the emulator or the lookahead). Default
  off, kept as an experiment; the persisted variants are the mechanism that works.
* **Sharing.** The variants file holds keys only (hashes, translator state, specializations, stage input structs), no code and no
  SPIR-V, and nothing title-specific in the code; a file recorded on one machine is a valid starting point for another with the same
  game build, so it could be distributed as optional data.

## Limits
* The first frames are not covered: the boot needs a handful of compute pipelines (3.7 s the longest) within 0.4 s of their
  registration. Earlier knowledge would need the shader code or a persisted driver cache (`--pipeline-cache-key schema`).
* Recording happens at first use of a variant, so only what a session actually used is replayed; a new level's shaders are
  recorded on that first visit.
* Tessellation pipelines and S_SWAP_PC programs are not recorded or replayed.
* The replay builds every stored variant that the guest registers, used again or not; with idle-only jobs the cost is spare CPU and
  driver time. A very large library may want an ageing policy (the store itself is capped at 50000 variants).
* Variant records are tied to the translator's static-key layout and to the input-info struct layouts through fingerprints
  (`kSchema`, `VertexInfoLayout()`, `PixelInfoLayout()`); a mismatch skips the record. Anything the translator changes in what
  it puts in the program key (BuildStageStaticKey) without changing that layout makes old records stop matching, which wastes
  work and never changes what is drawn.
