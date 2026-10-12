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
- `gpu-incremental` was merged as pushed (488ba49c, already inside `next-small`). The branch has newer **unpushed** commits
  (K14 `--elide-barriers`, K22 `--known-fills`, K20 `--clean-guest-reads` / `--readback-queue`, drain-cause stats): re-merge
  `gratton/gpu-incremental` into `gratton-next` once they are pushed.
- Build: `-DKYTY_MARCH=x86-64-v3 -DKYTY_LTO=ON` (K54, +2.1 %), ccache. Full `ctest -j2`: 79/79 (needs the `kyty_tests`
  target built first; the first ctest run after a plain `cmake --build` reports the excluded-from-all tests as "Not Run").

## Recommended play flags and why
| flag | value | why (evidence in `bench/RESULTS.md`) |
|---|---|---|
| `--submit-coalesce` | true | K13: submits per frame 360 -> 47, fps +5.8 % (3 vs 3), pacing better, timestamps unchanged |
| `--gc-clock` | frames | K43, measured together with K13 (the pair is the +5.8 %) |
| `--async-pipelines` | stall | the draw waits for its pipeline on a worker pool; no correctness difference seen (stall-only runs normal) |
| `--pipeline-lookahead` | true | halves the GPU-thread wait on pipeline jobs (16.9 -> 8.4 s) and removes most first-play stutter; hang path fixed in code (not reproduced in 17+ runs); **the desert "lemon" colour state is not cleared** (about 1 run in 3-4, stock also showed it 1 in 4; 1 of 4 here). Fall back to `false` if the colour bothers you |
| `--gpu-timestamps` | gpu (`--gpu-time-scale 1.0`) | real GPU timestamps for titles that size resolution from GPU time; neutral on Astro Bot here (fps 44.4 / 46.4 vs legacy 45.0 / 45.2, lemon 0 of 2 vs 1 of 2). An earlier 6 of 7 vs 3 of 8 lemon correlation did not reproduce |
| `--pad-speaker-volume` | 15 | already the code default (quiet DualSense speaker); passed explicitly |
| `--debug-hang-watchdog` | 15 | diagnostics only: reports a stuck frame counter |
| VRAM | defaults | the reclaim + retry allocation ladder and the LDS shrink are always on; `--vram-paging` stays off (vram.md: not needed to survive, 5.7 vs 4.2 fps at a 7 GB ballast does not justify a default change) |

Left off on purpose: `--pipelines-unoptimized-first` (K33: no gain on NVIDIA 610.57), `--skip-ir-validation` (K29: 0.6 s per run),
`--bda-sync-skip` (K15: under 1 % here), `--gc-buffer-own-share` (K42: for 8 GB cards, set 25 there).

## Validation (headless 60 Hz, fresh cache unless noted; binary 812e2f89)
- **Smoke (skip3 to s14), 4 runs:** no crash, desert and Astro correct by eye, tail-45 s fps 44.4-46.4, kyGpu median 41-42 %, 46 submits per frame.
- **Level entry (save C, route lvl9), 2 runs (play flags and bare flags): the GPU hangs at the level transition**
  (`NVRM Xid 109 CTX SWITCH TIMEOUT`, frame counter stuck, right after a mesh-shader pipeline compile and the "primitive restart is
  not implemented for mesh shaders" warning). No VRAM exit and no allocation failure before it (process VRAM peak 8.3 GiB, VMA usage
  6.3 of a 10.2 GiB budget). The same hang occurs without any play flag, so it is the known base bug of vram.md, not a regression.
- **Warm cache (chained from a fresh smoke run):** 98 s run: stutter time over 50 ms 27.0 s -> 6.7 s, worst frame 4.35 s -> 1.67 s, fps 47.9.
  kyGpu phase shares at steady state, warm (desert windows, `--gpu-phase-stats`): drain_tick 59 %, pipeline 13 %, descriptors 8 %, draw 6 %,
  pm4 6 %, srt 3 %, vkcmd 3 %, vkalloc 2 %; pipeline_job 0.1 % (cold: 18 %). Compiles are gone; what limits fps is the GPU thread waiting
  in synchronous readback drains, then pipeline key lookup.
