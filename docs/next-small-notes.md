# next-small notes

Branch `next-small` (on top of `gpu-incremental` 488ba49c): three small, default-off changes from the
fork-adoption plan (`forks/decision.md`) plus the K03 hazard fix that LTO needs. Each section: what, flags,
verification, limits. All numbers: Astro Bot, headless 60 Hz output, fresh cache, 1080p scene (pinned with
`--gpu-timestamps gpu --gpu-time-scale 1.0`), `--submit-coalesce true --gc-clock frames` in every arm.

## K03: rbx clobber (cherry-picked)

`RunOnGuestStack` declares `rbx` clobbered (chenxiao07/KytyPS5@2780eb24c, picked with `-x` from
`gratton-shaders` f4e28ac5). `gpu-incremental` had merged an earlier `gratton-shaders` that did not have it
yet, so it is picked here. The hazard is compiler dependent (a cached TLS address in RBX under LTO/PGO); the
v3+ThinLTO build below passes the full ctest including `guest_stack_exit`.

## K42: buffer GC gate on the cache's own bytes

- **Problem:** `BufferCache::RunGarbageCollector` decided `aggressive` (download dirty buffers, one GPU drain
  per batch, shorter ages, double the limit) from the whole-device VRAM usage. The texture cache shares that
  usage and can pin it above the critical mark with images it cannot evict, so the buffer cache stays
  aggressive for pressure its own frees cannot relieve.
- **Change:** `BufferGcGate::Aggressive(device_used, critical, own_bytes, share_percent)` (pure, in
  `renderer/cache/bufferGcGate.h`, tests in `tests/BufferGcGateTests.cpp`) also requires the buffer cache's
  registered bytes (`m_registered_bytes`, kept in `ChangeRegister`) to be at least `share` percent of the
  critical budget. The share is relative to the budget-derived critical mark, so it scales with the card.
- **Flag:** `--gc-buffer-own-share <0..100>`, default 0 = stock rule. The fork used an env var and a fixed 25.
- **Counters:** one console line per 1024 collector runs past the trigger, `BufferGC: runs= aggressive=
  freed_clean= downloaded_dirty= drains= own= device= critical=`.
- **Measured** (forced budget: `--vram-ballast-mb 5000 --vram-paging off --vram-stats on`, skip2 route,
  231 s, 1 run per arm, alternated):

| | off (stock) | `--gc-buffer-own-share 25` |
|---|---|---|
| collector runs past the trigger | 67584 | 67584 |
| aggressive runs | 45839 (68 %) | 0 |
| dirty buffers downloaded / GPU drains from the buffer GC | 82 / 25 | 0 / 0 |
| buffers freed (clean) | 910 | 586 |
| buffer cache bytes at the end (device usage, critical 3912) | 573 MiB (4388) | 856 MiB (4704) |
| VRAM heap usage peak (incl. 5000 MiB ballast) | 9417 MiB | 9729 MiB |
| frames / mean fps | 8302 / 35.9 | 8398 / 36.2 |
| frames over 50 ms (count / seconds) | 74 / 31.0 | 73 / 31.9 |
| last 45 s fps | 33.15 | 33.34 |
| allocation failure or exit | none | none |

- **Reading:** the gate does what it says (all aggressive runs and all buffer-GC drains gone, no VRAM exit,
  stalls unchanged) but on this title the buffer GC only drained 25 times in 231 s, so there is nothing to
  win in fps. The cost is that buffers live longer when not aggressive: +300 MiB peak in this pair (one run
  per arm, so the size of that is not established). The fork's benefit (UFC 5, 8 GB card: 1 to 3-6 fps) comes
  from titles with many GPU-modified buffers, which the bench does not have.
- **Default:** stays 0. Reasonable to turn on at 25 for cards at or below 8 GB; needs a title that shows the
  drains to prove it.

## K54: build options (KYTY_MARCH, KYTY_LTO)

- **Change:** `cmake/KytyOptimization.cmake` (ported from Yugendren/Kestrel@5a17583cd, options as in the
  chenxiao07 and Jetsku forks). `-DKYTY_MARCH=<level>` adds `-march=<level> -ffp-contract=off` (native is
  refused as non-portable); `-DKYTY_LTO=ON` is ThinLTO with lld and a ThinLTO cache in the build dir;
  `-DKYTY_LTO_JOBS=N` bounds the backend threads. All empty/off by default, so an unconfigured build is unchanged.
- **Build cost (this machine, -j8, ccache cold for the new flags):** full v3+ThinLTO build of
  `kyty_emulator` 1 min 48 s wall (13 min user); binary 22.4 MB vs 22.2 MB.
- **Measured** (v3 + ThinLTO together vs the plain build of the same commit 6b567393; skip3 route to s14,
  alternated base, LTO, base, LTO; the pin has `--submit-coalesce true --gc-clock frames`):

| run | tail-45 s fps | kyGpu CPU median / p90 (last 45 s) | kyGpu whole-run median |
|---|---|---|---|
| base 1 | 43.90 | 44 / 54 | 34 |
| v3+LTO 1 | 44.81 | 41 / 50 | 31 |
| base 2 | 43.99 | 44 / 55 | 36 |
| v3+LTO 2 | 44.93 | 41 / 53 | 34 |
| mean base / v3+LTO | 43.95 / 44.87 (+2.1 %) | 44 / 54.5 vs 41 / 51.5 | 35 vs 32.5 |

- **Reading:** the GPU-thread CPU load drops about 3 points of a core (7 % relative) in all four runs, and
  the fps follows by about 2 % because the scene is also limited by GPU execution (the K51 finding). It is the
  low end of the +4-5 % the forks report. v3 and ThinLTO were not separated.
- **Not done:** PGO (`-fprofile-generate`/`-fprofile-use`). It needs a training run of a game with an
  instrumented binary; the profile is game-derived, so it stays under `bench/` and out of git, and it is
  stale after code changes. The forks report +11 % for PGO + AOT (cx) and larger on Windows (Yug).
- **Smoke:** the full ctest passes on the v3+ThinLTO build (see RESULTS.md).

## K15: skip unchanged BDA syncs

- **Problem:** every draw or dispatch whose shaders address memory through raw pointers
  (`RenderContext::PrepareBda`) synchronizes every buffer of every mapped range.
- **Change:** a full pass leaves all pages clean and write-protected, so the next one has work only if pages
  became CPU-modified, a buffer was (un)registered, or the mapped ranges changed. Three generations track
  those (`BdaSync::g_cpu_dirty_generation` bumped in `RegionManager::ChangeState<Cpu, true>` and on region
  creation, `BufferCache::BufferSetGeneration`, `RenderContext::m_mapped_generation`); `BdaSync::SkipGate`
  (pure, `host_gpu/bdaSyncGate.h`, tests in `tests/BdaSyncGateTests.cpp`) skips while all three equal the values
  read BEFORE the last full pass. Idea and design from ogeidexe/KytyPS5@4a24f1d2d.
- **Flag:** `--bda-sync-skip <true|false>`, default false. A console line per 16384 calls:
  `GPU: BDA sync calls= full= skipped= full_pass_ms=`.
- **Measured** (skip3 route to s14, same pin, 3 runs per arm of which 2 with the timer; counters cumulative
  from start):

| | off | on |
|---|---|---|
| PrepareBda calls per run | 163840 - 245760 | 114688 - 245760 |
| skipped | 0 | 90 % (103097/114688, 223408/245760, 147250/163840) |
| time in full passes per run | 1652 / 1718 / 2110 ms (about 10 us a call) | 1177 / 860 ms (full passes are the dirty ones, 40-50 us) |
| tail-45 s fps | 44.64 / 44.47 / 44.62 | 45.77 / 44.88 / 42.55 |
| kyGpu CPU p90 (last 45 s) | 56 / 53 / 53 | 55 / 53 / 58 |

- **Reading:** the skip works (90 %), but this scene makes only about 27 BDA calls a frame, so the walks cost
  about 1.7 s of a 100-130 s run, i.e. at most 1 %, and the fps/kyGpu differences are inside the run-to-run
  spread. ogei's 15 % of the GPU thread was a scene with hundreds of BDA draws per frame (sand trail). No crash or
  visible change in the runs with the flag on (no pixel comparison was made).
- **Default:** stays off; worth a try on a title or scene with many BDA draws.
