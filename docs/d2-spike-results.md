# D2-C1b spike results (Step 0 + Step 1): VK_EXT_graphics_pipeline_library on Astro Bot

Branch `d2-spike-581` (upstream `581dad7` + `phase1-581` + this spike), worktree `kyty/wt-d2-spike`, emulator build `kyty/build/d2-spike`, benchmark build `kyty/build/d2-spike-gplbench`. Nothing here is a production GPL path: the only emulator change is the default-off `--dump-pipelines <dir>` flag. Council text: `shaders.md` Decision 2, `council-notes/2026-10-11-shader-stutter.md` (D2 sections, R4 chair safeguards).

## 1. Verdict

**The Step 2 gate fails (on hideability). Recommendation: do not build GPL (Step 3); go to D2-C2 (PM4 lookahead), keep stall-never-skip as the default.**

| Gate (threshold from shaders.md) | Measured | Verdict |
|---|---|---|
| D2-S0 layout not invasive | Superset layout needs ~5 touch points; per-pipeline layout and push-vs-heap flag already exist; 11% of graphics layouts already use the DescriptorHeap path (section 3.1) | PASS |
| D2-S0 stalls not rare/short (p95 at least ~1 frame; n at least 30) | Cold: n=105 misses, p95 = 1048 ms (63 frames), p99 = 1798 ms (108 frames). Warm (driver cache hit): p95 = 25.8 ms (1.55 frames), p99 = 47 ms (2.8 frames), n=105 | PASS cold; warm is marginal |
| D2-S1a translation under half of miss time | Cold: 3.1% of miss time (3.8% for graphics). **Warm: 92.8%** (see 3.2) | PASS cold only |
| D2-S2a fast link under ~1/10 of full compile | fast link mean 0.005 ms (max 0.01 ms) vs 60.7 ms mean monolithic: ratio 0.0001. But libraries + fast link = **1.01x** the monolithic compile | PASS, but vacuous (cost moved into the libraries) |
| D2-S2b hideable in 1-2 frames, miss to usable pipeline including library compiles | New-shader miss (translation + libs on parallel workers + link): p50 27 ms, **p95 154 ms (9.2 frames)**, p99 296 ms. Within 2 frames: 46/60 (77%) vs 28/60 (47%) today. With library sharing in capture order: 49/60 within 2 frames, p95 163 ms | **FAIL** (p95 more than 4x the 2-frame bound) |
| D2-S2c translation under half of miss time | 11% of a GPL new-shader miss | PASS |
| Layout identity (libraries, fast link, optimized link) | One superset layout object, and also distinct but identically defined layout objects (push-descriptor and plain), all link on all 60 pipelines. No validation layer installed, so spec compliance is driver-accepted, not layer-checked | PASS (limited) |
| Image diff, no visible divergence | 60 pipelines rendered offscreen: 0 bytes differ between monolithic, fast-linked and optimized-linked, own and superset layouts. Only 17/60 draw anything on screen with dummy data | PASS (limited coverage; 1 driver) |

Why GPL does not pay on this driver, in one paragraph. On NVIDIA 610.57 the libraries are the full shader compiles. Pre-raster (VS) plus fragment-shader library compile time equals the monolithic compile time (3685.8 ms vs 3640.5 ms over 60 pipelines), and the fast link is free (0.005 ms). A brand-new shader still pays the whole compile; only state-only variations that reuse existing libraries get cheaper. And GPL cannot touch 83% of the cold creation time at all: 49.1% is compute pipelines (GPL does not apply) and 33.6% is six mesh-shader graphics pipelines (five take 1.1-1.3 s, one 0.27 s; their pre-raster library costs the same as the monolithic pipeline). Even the optimistic model (every library compiled once and shared, pre-raster and fragment libraries compiled in parallel) removes only about 1.7 s of the 16.7 s cold creation time, i.e. roughly 10%, and nothing in a warm session.

## 2. Method

Machine and software: Ryzen 7 9700X, RTX 3080 Ti, NVIDIA driver 610.57.04, Vulkan 1.4.341, kernel 7.2.3-arch1, Hyprland, emulator fullscreen 2560x1440. Route `routes/intro150v2.json` (150 s of the intro cinematic), cold run = fresh dir, per-run NVIDIA cache, game files evicted from the page cache (bench harness controls). Everything ran under `flock bench/.emu.lock`.

Commits on `d2-spike-581` (none pushed): `f56428e` and `b483536` the `--dump-pipelines` flag; `5064654`, `eb25196` the benchmark tool and data; the results commit is the tip of the branch. The emulator binary used for all game runs is `b483536` (`KytyPS5-2026-10-01-4479808-14-gb483536`, not dirty). The later commits only add `tools/` and `docs/`.

Thresholds were taken from shaders.md before measuring (S0: p95 at least ~1 frame; S1a: translation under 50%; S2: fast-link under 1/10, 1-2 frames, translation under half). Two operational choices were fixed by me before any GPL number existed: a sample floor of 30 per stratum (p99 on a stratum below 100 samples is not meaningful, only the pooled n=105 is), and "hideable in 1-2 frames" is read as p95 of miss-to-usable at most 2 frames (33.3 ms at 60 Hz), reported next to p50 and the share within 2 frames. The stall run (phase1-timing) already existed when I fixed them.

Tools (all in `tools/gpl-bench/`):

| file | purpose |
|---|---|
| `src/graphics/host_gpu/renderer/pipeline/pipelineDump.{h,cpp}` | `--dump-pipelines <dir>`: at every graphics/compute pipeline miss writes `spv/<shader_id>.spv`, `pipelines/<seq>.json` (stages, vertex input, IA, tessellation, viewport depth-clip control, rasterization incl. depth-clip/provoking-vertex, MS, depth/stencil, blend + color-write, dynamic states, dynamic-rendering formats, layout bindings, push-constant range, push-descriptor flag, in-game driver time), `compute/<seq>.json`, generated rect-list TCS/TES |
| `gpl_bench.cpp`, `diff.inc` | standalone Vulkan program (system loader, Vulkan-Headers from the submodule, nlohmann json): replays the dump on a device created with the emulator's own feature/extension set plus `VK_EXT_graphics_pipeline_library` |
| `run_bench.sh`, `run_extra.sh`, `run_diff.sh` | cold wrappers (`__GL_SHADER_DISK_CACHE=0`, isolated `__GL_SHADER_DISK_CACHE_PATH`; the cache dir was empty after every run, so nothing was served from disk) |
| `analyze_layout.py`, `analyze_bench.py`, `step0_stall.py`, `summarize_diff.py` | analysis of dump, CSVs and timing logs |

Measurements: one driver call per timed step, on a fresh worker thread, no `VkPipelineCache` (cold), pipelines in capture order, one variant per process (so the driver's in-process state of one variant cannot help another). A reverse-order process checks order effects (monolithic sum 3640.5 vs 3627.7 ms, GPL fast-link unchanged). Libraries are created with `LIBRARY | RETAIN_LINK_TIME_OPTIMIZATION_INFO`; "fast" = link without `LINK_TIME_OPTIMIZATION`; "opt" = link with it. The standalone monolithic times reproduce the game: per pipeline bench/game ratio p50 0.94, Pearson 0.89, and the 60 non-mesh pipelines sum to 3.64 s standalone vs 2.87 s in game (the game's VkPipelineCache dedupes identical pipelines).

Data: `docs/d2-spike-data/run1/` (CSVs, logs, `analysis_*.txt`, `step0_stall_*.txt`). The raw dump (15 MB, translated SPIR-V of the game, not committed) is `bench/runs/20261011T162348Z-d2-dump/pipelines_dump`.

Runs used: cold timing `20261011T160000Z-phase1-timing` (phase1-581 binary) and `20261011T162348Z-d2-dump` (d2-spike binary, dump + timing); warm `20261011T164032Z-d2-dump-warm2` (harness valid=True, driver cache loaded, 8.6 MB). A first warm run `...162631Z-d2-dump-warm` was marked invalid by the harness only because I had edited a tracked tool file between runs (source tree dirty); its numbers agree with the clean one.

## 3. Step 0

### 3.1 Superset pipeline layout

Code reading (581dad7 + phase1):

- One descriptor set (set 0) per pipeline. Binding number = `NativeBinding(stage, kind) = kind + 50 * group` with group 0 = VS/LS/ES/mesh, 1 = PS, 2 = TCS, 3 = TES; 50 `DescriptorBindingKind`s (Buffers, 43 image kinds, Samplers, Gds, BdaPagetable, FaultBuffer, FlattenedSrt, ShaderData) = 200 possible bindings. Descriptor types used: STORAGE_BUFFER, SAMPLED_IMAGE, STORAGE_IMAGE, SAMPLER. Array size = `binding.resources.size()` of that shader (`NativeDescriptorCount`), also baked into the SPIR-V array type (`spirvEmitterModule.cpp:186-250`). Push constants: always 128 bytes at offset 0, stage mask = union of the pipeline's stages.
- Consumers of layout / set-layout / push flag: `shaders.cpp` (`AddLayoutBindings`, `CreateDescriptorLayout` where `uses_push_descriptors = total descriptors <= maxPushDescriptors`, graphics pipeline layout, compute pipeline layout), `pipelineCache.cpp:640-641` (destroy), `descriptors.cpp` `CommitBindings` (push-constants and the push-vs-`DescriptorHeap::Commit` split, selected per pipeline by `pipeline.uses_push_descriptors`), `renderDraw.cpp:1121` (mesh push constants), `descriptorHeap.{h,cpp}` (sets batched per `VkDescriptorSetLayout`, pools of 1024 sets / 8192 storage buffers / 8192 sampled images / 1024 storage images / 1024 samplers). Precedent for fixed shared layouts: `blitHelper.cpp`, `tiler.cpp`, `faultManager.cpp`, `swapchain.cpp` overlay (each one fixed push-descriptor layout).
- Because layout and the push-vs-heap flag are already per-pipeline fields, a GPL-eligible pipeline could simply carry the superset layout and `uses_push_descriptors = false`; `CommitBindings` needs no change, and non-eligible pipelines keep their own layout. The optimized replacement does not have to be an LTO link: a monolithic pipeline on its own layout is also valid, since bindings are re-committed per draw.

Sizes from the capture (66 graphics pipelines, cold intro): per-layout descriptors p50 18, p90 32, max 58; bindings per layout p50 5, max 12. 7/66 (11%) layouts already exceed `maxPushDescriptors` and use the DescriptorHeap path today.

Superset = max count per binding over all 66 pipelines: 16 bindings (of 200) and **86 descriptors**: STORAGE_BUFFER 26, SAMPLED_IMAGE 51, SAMPLER 8, STORAGE_IMAGE 1. The largest contributors are one pixel shader with 43 float 2D textures and Buffers arrays of 15 (PS) and 7 (VS). Full per-kind table: `docs/d2-spike-data/run1/analysis_layout.txt`. Compute (39 pipelines) would need 39 descriptors, but compute is not GPL.

Device limits (vulkaninfo): `maxPushDescriptors` = 32, `maxPushConstantsSize` = 256, `maxBoundDescriptorSets` = 32, per-stage and per-set descriptor limits ~1M, `maxPerSetDescriptors` 4G. So the superset fits every limit except push descriptors: **it cannot use the push path (86 > 32)**, every GPL pipeline goes through the DescriptorHeap path. That is not a kill (the plan says push overflow is not automatic).

Cost of the DescriptorHeap path for the superset (`gpl_bench --variant desc`, CPU only, 2000 iterations per pipeline, 60 pipelines; mean per draw): push-descriptor 0.059 us, heap with the pipeline's own layout 0.036 us (update + bind), heap with the superset layout 0.14 us; set allocation 0.02-0.03 us per set. These are sub-microsecond. The real cost is pool pressure: one set from the superset layout consumes 26 + 51 + 8 + 1 descriptors, so a pool serves at most 128 sets before exhaustion (limited by the 1024-sampler budget), vs about 407 sets for today's mean layout. The hit path performs ~457 shader lookups per frame (2,839,916 hit-path materialize calls over 6209 frames), so the pool would cycle about 3.6 times per frame instead of 1.1; raising the pool sizes removes that. The caps for unseen scenes would need headroom (the capture only sees the intro); a pipeline exceeding the caps falls back to the monolithic own-layout path.

Alternative costed: **independent sets** (`VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`), set 0 = VS group, set 1 = PS group, each sized to its own shader (no superset, no waste). Needs `DescriptorSet` decoration per stage group (one literal at `spirvEmitterModule.cpp:186`), two sets in `CommitBindings` (only one set per layout may be a push set), and layouts become shader-keyed, so libraries can be cached by shader. Not tried in the benchmark; recorded as the better layout design if GPL were ever pursued.

Verdict for the layout half of D2-S0: not invasive; push overflow is real but already handled by the existing path.

### 3.2 Stall distribution (per-miss `pipeline_create_*`)

Source: `--shader-timing-log` rows. Cold runs: two independent runs (phase1-timing and d2-dump) agree within a few percent; numbers below are the phase1-timing run (n = 105 misses: 66 graphics, 39 compute). Frame = 16.67 ms.

| stratum (cold) | n | sum | p50 | p90 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| all creation | 105 | 16.4 s | 27.9 ms | 273 ms | 1024 ms (61 fr) | 1756 ms (105 fr) | 3546 ms |
| graphics creation | 66 | 8.35 s | 26.5 ms | 273 ms | 990 ms | 1369 ms | 1375 ms |
| compute creation | 39 | 8.06 s | 31.9 ms | 270 ms | 584 ms | 2872 ms | 3546 ms |
| translation (127 rows) | 127 | 0.50 s | 1.5 ms | 12.2 ms | 20.7 ms | 37.5 ms | 48 ms |
| whole miss (translate + create) | 105 | 16.9 s | 30.1 ms | 291 ms | 1048 ms (63 fr) | 1798 ms (108 fr) | 3594 ms |

Warm session (driver `VkPipelineCache` loaded, all 105 misses still occur at the Kyty level): creation p50 0.25 ms, p95 1.5 ms, max 2.7 ms; **whole miss p50 1.6 ms, p95 25.8 ms (1.55 frames), p99 47.2 ms (2.8 frames), max 60.5 ms, and 92.8% of the time is translation.** So in a warm session the remaining stall is translation, which C1a (workers, stored SPIR-V) addresses and GPL cannot.

Miss mix (R4 safeguard 4): 89/105 misses (85%) involve a new shader (translation of at least one new program; 15.8 s of 17.3 s), 16 (15%) are known programs with new state or permutation (1.46 s). Graphics only: 51 new-shader (7.4 s), 15 known-shader/new-state (1.46 s; p50 0.1 ms because identical pipelines dedupe in the VkPipelineCache, p95 466 ms). Strata below 30 (known-shader graphics n=15, known-shader compute n=1) are reported but not used for conclusions. Time bins: 76 misses in the first 30 s, 29 in the next 30 s, none after 60 s in this 150 s route.

Composition of cold creation time by class (from the dump, in-game driver time, 16.7 s total):

| class | n | sum | share | p50 | p95 | max | GPL applicable |
|---|---|---|---|---|---|---|---|
| compute | 39 | 8.19 s | 49.1% | 31 ms | 575 ms | 3730 ms | no |
| mesh-shader graphics | 6 | 5.61 s | 33.6% | 1270 ms | 1406 ms | 1412 ms | excluded by plan; also no gain (below) |
| plain graphics (VS+PS) | 60 | 2.87 s | 17.2% | 25 ms | 266 ms | 485 ms | yes |

(No rect-list or tessellation pipelines occurred in this route.)

D2-S0 verdict: layout not invasive, stalls neither rare nor short (cold p95 = 63 frames; warm p95 = 1.55 frames) -> proceed to Step 1.

## 4. Step 1

### 4.1 Driver properties

`graphicsPipelineLibraryFastLinking` = **true**; `graphicsPipelineLibraryIndependentInterpolationDecoration` = **true** (so the VS-to-PS interpolation decorations need not be duplicated on both sides on this driver; other drivers may differ). `VK_EXT_graphics_pipeline_library` and `VK_KHR_pipeline_library` are available on the device.

### 4.2 Timings (60 non-mesh graphics pipelines, cold, one worker thread, ms)

| step | sum | mean | p50 | p90 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| monolithic (own layout, today's path) | 3640.5 | 60.7 | 46.5 | 149.4 | 255.7 | 348.2 | 456.0 |
| monolithic (superset layout) | 3630.1 | 60.5 | 46.9 | 147.4 | 248.0 | 348.1 | 458.8 |
| vertex-input library | 0.5 | 0.01 | 0.01 | 0.01 | 0.01 | 0.02 | 0.03 |
| pre-raster library (VS) | 1529.5 | 25.5 | 21.9 | 40.2 | 139.0 | 151.2 | 167.4 |
| fragment-shader library (PS) | 2155.4 | 35.9 | 24.0 | 103.0 | 125.9 | 274.4 | 324.2 |
| fragment-output library | 0.4 | 0.01 | 0.01 | 0.01 | 0.01 | 0.01 | 0.01 |
| **fast link** | **0.3** | **0.005** | 0.00 | 0.01 | 0.01 | 0.01 | 0.01 |
| optimized link (LTO) | 1418.0 | 23.6 | 22.2 | 39.0 | 121.4 | 136.7 | 136.9 |
| libraries + fast link | 3686.1 | 61.4 | 46.8 | 146.9 | 264.4 | 363.5 | 464.1 |

Superset-layout numbers for the libraries and links equal the own-layout ones within noise. Cold vs warm (second creation with the same `VkPipelineCache`): monolithic 0.12 ms p50 (1.6 ms max), libraries 0.01 ms, fast link 0.00 ms.

Observations: libraries + fast link = 1.013x monolithic. Optimized link = 0.39x monolithic on top (so a full optimized pipeline costs 1.40x). `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` has no effect on this driver (monolithic 3610 vs 3640 ms; pre+fs libraries 3680 vs 3685 ms). The six mesh pipelines: monolithic 273 / 1288 / 1243 / 1259 / 1286 / 1099 ms, pre-raster library 272 / 1178 / 1211 / 1207 / 1018 / 1051 ms, fast link 0.01-0.14 ms, i.e. no gain either.

### 4.3 End-to-end: miss to usable pipeline (R4 safeguard 3)

Translation times are joined from the game's timing log by shader id; library compiles are measured; the fast link is included.

| path (60 plain graphics pipelines, cold) | mean | p50 | p90 | p95 | p99 | max | within 1 frame | within 2 frames |
|---|---|---|---|---|---|---|---|---|
| today: translate + monolithic create | 65.7 | 50.1 | 165.2 | 278.0 | 375.7 | 492.6 | 20/60 | 28/60 |
| GPL, brand-new shader pair, translation parallel, libraries on parallel workers | 42.1 | 26.7 | 117.9 | 153.5 | 295.8 | 348.5 | 22/60 | 46/60 |
| GPL, brand-new shader pair, translation serial | 43.8 | 28.1 | 121.5 | 163.3 | 301.9 | 360.8 | n/a | n/a |
| GPL with library sharing in capture order, libraries on 1 worker | 47.5 | 23.6 | 99.2 | 273.2 | 392.6 | 500.7 | 27/60 | 42/60 |
| same, libraries on 2+ workers | 36.0 | 20.1 | 77.6 | 162.7 | 301.9 | 360.8 | 29/60 | 49/60 |

Pure library wall clock (no translation): 4 concurrent workers p50 24.9 / p95 147 / max 322 ms; 2 workers p50 24.6 / p95 146 ms. The bound is the slowest of the pre-raster and fragment libraries. Translation is 11% of a GPL new-shader miss.

Library reuse in the capture (what sharing can save): of 60 pipelines, 21 distinct vertex-input, 30 distinct pre-raster, 47 distinct fragment-shader, 34 distinct fragment-output libraries. 28 pipelines need both a new pre-raster and fragment library (2095 ms of monolithic time), 21 need one (933 ms), **11 need none (link only, 612 ms = 16.8% of the plain-graphics time, 9 of them known shaders)**. Total GPL compile work with perfect sharing = 2626 ms vs 3641 ms monolithic (72%).

Parallel worker scaling (makespan of the 60 pipelines on N concurrent workers, no GPL vs GPL fast path): N=1 3627 / 3721 ms, N=2 1926 / 1924, N=4 1054 / 1073, N=8 687 / 704. The driver scales to 5.3x at 8 workers on its own; GPL adds nothing to that throughput. Plain worker parallelism (C1a) and early scheduling (lookahead) are the levers, not GPL.

### 4.4 Layout identity

- Same `VkPipelineLayout` object (own, then the shared superset) for all four libraries and both links: 60/60 create and link; with the own layout this includes push-descriptor layouts (59 of the 66 captured layouts).
- Distinct but identically defined layout objects for pre-raster and fragment libraries and a third for the link (`--dup-layout`, push-descriptor flag preserved): 60/60 link.
- Mesh pipelines: the vertex-input library must be given empty vertex-input and input-assembly state; with null pointers (as the monolithic create-info has for mesh) the driver segfaulted.
- Not tested: layout mismatch negative controls, other drivers. No Vulkan validation layer is installed, so conformance is "driver accepts and renders identically", not "layer clean".
- VS-to-PS location stability across independently compiled libraries: the SPIR-V for both stages is produced independently today and the 17 informative renders match, but the captured pairs cover only the intro.

### 4.5 Image diff

Method: for each non-mesh pipeline (60), create monolithic (own non-push layout), fast-linked and optimized-linked pipelines, draw 3000 vertices x 2 instances into 128x128 colour targets with the captured formats (+ depth/stencil), identical deterministic pseudo-random data for vertex buffers, push constants, storage buffers (BDA page table and fault buffer zeroed) and sampled images (float/uint/sint 1D/2D/array/3D; MSAA, depth-compare and storage images bound as robustness2 null descriptors), all dynamic state set to fixed values; byte compare of every colour attachment. One process per pipeline so a GPU fault cannot kill the sweep; a second monolithic render is a determinism control.

Result (own and superset layouts, `diff_own.log`, `diff_super.log`): 60/60 rendered, determinism control clean, **0 differing bytes** monolithic vs fast, monolithic vs optimized, fast vs optimized. Coverage is the weak point: only **17/60** pipelines put any pixel on screen (238,708 texels over those 17); the other 43 render nothing with dummy resources (positions come from SRT/BDA data, not from vertex attributes), so for them the diff only shows that all three pipelines run identically to a blank image. Depth is not compared (only colour). Single driver, so the "2+ drivers" requirement is **not met**.

## 5. Limitations

- **One driver.** The machine has an AMD iGPU (Granite Ridge) but no RADV ICD installed (`vulkan-radeon` needs root; I did not download a driver). So RADV, the council's required second driver, was not measured. The conclusion "fast link is free but the libraries cost the full compile" is NVIDIA-specific; another driver may split the work differently (for example by deferring more to the link), which could change the verdict there. That is unmeasured. `graphicsPipelineLibraryIndependentInterpolationDecoration` and fast-link support differ per driver.
- One title, one 150 s route (the intro cinematic); 105 misses, 66 graphics, 6 mesh and no rect-list/tessellation pipelines. The 2+ title requirement of G1 is also open. Per-class strata are small (mesh n=6, known-shader graphics n=15); p99 is meaningful only for the pooled sets.
- Standalone timings are single-process, sequential, in capture order; the game compiles concurrently with a loaded CPU and an in-process shared `VkPipelineCache`, which dedupes identical pipelines (e.g. mesh seq 37: 0.02 ms in game, 1259 ms standalone). The 60 non-mesh pipelines match the game within ~6% in aggregate.
- No GPL validation layer; offscreen diff coverage is 17/60 informative with dummy resources; colour only.
- The sharing model assumes libraries are cached per (shader, state) key as defined in `analyze_bench.py`; a real implementation may share less or more.
- Warm-session numbers use one warm run (plus one flagged duplicate). The warm run still performs all 105 Kyty-level misses because ProgramCache is in-memory only.
- The superset caps come from this capture only.

## 6. What this means for the plan

1. GPL is out for NVIDIA: it does not shorten a first-encounter stall (libraries cost the monolithic compile), it covers 17% of cold creation time, and it brings the DescriptorHeap pool pressure. Do D2-C2 (lookahead + early parallel scheduling) as the shaders.md fallback says.
2. Where the stall really is: compute pipelines (49%, single creations up to 3.7 s) and the mesh-shader graphics pipelines (34%, ~1.2 s each). Both need to be created early and in parallel (the driver gives 5.3x on 8 workers), neither is helped by GPL.
3. Warm sessions are translation-bound (93%): stored SPIR-V / persisted jobs from C1a address that; GPL is irrelevant there.
4. If GPL is ever revisited (e.g. for RADV), use independent sets (per-stage layouts) rather than a superset, keep mesh and compute on the monolithic path, and require a second-driver run before Step 3.
5. Open question worth a cheap check: whether the mesh pipelines (guest geometry shaders compiled to mesh stage) can be made cheaper at translation time; they are 34% of cold creation time on their own.

## 7. Reproduce

    export PATH=$HOME/.local/share/mise/installs/ninja/1.13.2:$PATH; L=/home/claudio/Documents/ps5-emulator/bench/.emu.lock
    cd kyty
    flock $L cmake -S wt-d2-spike -B build/d2-spike -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
    flock $L cmake --build build/d2-spike --target kyty_emulator -j8
    cd ../bench     # config-d2-dump.json = config-phase1.json + this binary/source_dir + --shader-timing-log + --dump-pipelines pipelines_dump
    flock $L ./run.sh --label d2-dump --config config-d2-dump.json --timeline routes/intro150v2.json --chain-warm 1
    cd ../kyty
    flock $L cmake -S wt-d2-spike/tools/gpl-bench -B build/d2-spike-gplbench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
    flock $L cmake --build build/d2-spike-gplbench
    D=../bench/runs/<run>/pipelines_dump
    flock $L wt-d2-spike/tools/gpl-bench/run_bench.sh build/d2-spike-gplbench/gpl_bench $D out
    flock $L wt-d2-spike/tools/gpl-bench/run_extra.sh build/d2-spike-gplbench/gpl_bench $D out
    flock $L wt-d2-spike/tools/gpl-bench/run_diff.sh build/d2-spike-gplbench/gpl_bench $D out/diff_own.log own
    GPLB_DUMP=$D python3 wt-d2-spike/tools/gpl-bench/analyze_bench.py out <timing run dir>
    python3 wt-d2-spike/tools/gpl-bench/analyze_layout.py $D 32
    python3 wt-d2-spike/tools/gpl-bench/step0_stall.py <timing run dir>
