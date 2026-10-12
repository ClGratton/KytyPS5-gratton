# gratton-next: the integration branch for play

`gratton-next` = `next-small` (gpu-incremental 488ba49c + K42, K54, K15, K03) + `gratton-shaders` (cee2b2b7: K02 short-read
fallback, crash-triage diagnostics, de-flaked async tests) + `stutter-next` (K11, K29, K33) + `vram-level-entry`
(pad speaker, level-entry docs; the LDS shrink and the allocation ladder were already in the base through `ram-release`) +
the docs of `res-fps-controls`. All new behaviour is behind flags that default to off; the recommended set below is passed on
the command line (`bench/config-play-next.json` in the bench tree).

## Merge notes
- `gratton-shaders` merged without conflicts; `stutter-next` conflicted in five places, all additive on both sides
  (`CMakeLists.txt` tests, `emulatorConfig.cpp` accessors, `main.cpp` help and parser, `shaders.cpp`, `AsyncPipelineTests.cpp`):
  both sides kept. In `shaders.cpp` the graphics path keeps the `--vram-stats` compile-time log and goes through K33's
  `CreatePipelineHandle`.
- `gpu-incremental` final (4a11b55c: drain-cause stats, K14 `--elide-barriers`, K22 `--known-fills`, K20a `--clean-guest-reads`,
  `--readback-queue`) merged on top (e4728ccb); two additive conflicts (`emulatorConfig.cpp` accessors, `bufferCache.cpp` includes), both kept.
  Re-merge `gratton/gpu-incremental` only if it moves again.
- Build: `-DKYTY_MARCH=x86-64-v3 -DKYTY_LTO=ON` (K54, +2.1 %), ccache. Full `ctest -j2`: 83/83 at the final merge (79/79 before the drain cuts) (needs the `kyty_tests`
  target built first; the first ctest run after a plain `cmake --build` reports the excluded-from-all tests as "Not Run").

## Recommended play flags and why
| flag | value | why (evidence in `bench/RESULTS.md`) |
|---|---|---|
| `--submit-coalesce` | true | K13: submits per frame 360 -> 47, fps +5.8 % (3 vs 3), pacing better, timestamps unchanged |
| `--gc-clock` | frames | K43, measured together with K13 (the pair is the +5.8 %) |
| `--elide-barriers` / `--known-fills` / `--clean-guest-reads` | true | drain cuts (K14, K22, K20a): 3 vs 3 at the pin +6.2 % fps (45.4 -> 48.2), no VRAM exit, tests; here tail-45 s fps 48.6 / 48.8 (before: 44.4-46.4) and about 450 of 870 guest barriers a frame elided |
| `--async-pipelines` | stall | the draw waits for its pipeline on a worker pool; no correctness difference seen (stall-only runs normal) |
| `--pipeline-lookahead` | true | halves the GPU-thread wait on pipeline jobs (16.9 -> 8.4 s) and removes most first-play stutter; hang path fixed in code (not reproduced in 17+ runs); **the desert "lemon" colour state is not cleared** (about 1 run in 3-4, stock also showed it 1 in 4; 1 of 4 here). Fall back to `false` if the colour bothers you |
| `--gpu-timestamps` | gpu (now the default; no flag needed) | real GPU timestamps for titles that size resolution from GPU time; neutral on Astro Bot here (fps 44.4 / 46.4 vs legacy 45.0 / 45.2, lemon 0 of 2 vs 1 of 2). An earlier 6 of 7 vs 3 of 8 lemon correlation did not reproduce |
| `--pad-speaker-volume` | 15 | already the code default (quiet DualSense speaker); passed explicitly |
| `--debug-hang-watchdog` | 15 | diagnostics only: reports a stuck frame counter |
| VRAM | defaults | the reclaim + retry allocation ladder and the LDS shrink are always on; `--vram-paging` is now the default (docs/defaults.md: neutral in normal play, 35 vs 17 fps at a forced 5.7 GB budget) |

Left off on purpose: `--pipelines-unoptimized-first` (K33: no gain on NVIDIA 610.57), `--skip-ir-validation` (K29: 0.6 s per run),
`--readback-queue` (K20: no gain), `--bda-sync-skip` (K15: under 1 % here), `--gc-buffer-own-share` (K42: left at 0, it hurts with paging on, docs/defaults.md).

## Validation (headless 60 Hz, fresh cache unless noted; binary e4728ccb = all merges, flags above)
- **Smoke (skip3 to s14), 6 runs over the two builds:** no crash, desert and Astro correct by eye. Final build, 2 runs: tail-45 s fps
  48.6 / 48.8, 92-94 % of frames at one vblank, 38 submits a frame, kyGpu median 43 %, desert colour normal. The build before the
  drain cuts (4 runs): 44.4-46.4 fps, one lemon run in four.
- **Level entry (save C, route lvl9), 4 runs, no pass.** Two runs (play flags, bare flags) reached the transition and **hung the GPU**
  (`NVRM Xid 109 CTX SWITCH TIMEOUT`, frame counter stuck about 187 s after the first frame, right after a mesh-shader pipeline compile and
  the "primitive restart is not implemented for mesh shaders" warning; no flag involved, the known base bug of vram.md). The two runs
  with the final flags never left the galaxy map (the image anchor missed by 1 of 30, so the dive input was not sent): no level, no hang,
  VMA usage 6.0-6.3 of a 10.2 GiB budget, process VRAM 6.2 GiB, ~50 fps on the map for 250 s, MemAvailable min 0.3-0.5 GiB. Before the hang:
  VRAM peak 8.3 GiB process / 6.3 GiB VMA, no allocation failure. So: **the level load itself was not passed on this build; the hang is open.**
- **Warm cache (chained from a fresh smoke run), final build:** fps 53.2 (fresh 48.7), stutter over 50 ms 28 s -> 4.9 s, worst frame 7.0 s -> 0.7 s.
  kyGpu phase shares, warm, desert windows (`--gpu-phase-stats 10`, timers on): drain_tick 48 %, pipeline 15 %, pm4 10 %, descriptors 8 %,
  draw 6 %, srt 3 %, vkcmd 3 %, vkalloc 1 %; pipeline_job about 1 % (cold: 18 %). Compiles are gone; fps is limited by the GPU thread waiting
  in readback drains (down from 59 % before the drain cuts; the remaining drains are real GPU->CPU data dependencies: indirect-draw
  arguments and SRT tables), then pipeline-key lookup (15 %: a candidate for caching the key per draw state).
