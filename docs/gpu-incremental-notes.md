# gpu-incremental notes

Branch `gpu-incremental` (on top of `gratton-shaders`): small, default-off GPU-thread changes ported or
written under the fork-adoption plan (`forks/decision.md`). Each section: what, flags, verification, limits.

## Desert colour (lemon state): K41 and K09 verdict (2026-10-12)

Question: the Astro Bot desert sometimes renders saturated yellow ("lemon", mean colour about
255,251,62) instead of sand (about 242,191,102). Stock was lemon in 1 of 4 runs, `--pipeline-lookahead`
`all` in 3 of 3. Did the texture-GC walk fix (K41, `283b26bb`) or the unlaunched-lane read fix (K09,
Bry@067e88265) remove it?

Setup: route `routes/skip3.json` (fixes the bimodal start), lookahead `all` + `--async-pipelines stall`,
fresh cache, headless output, `--stop-after s14`. Score = mean colour of checkpoints s12-s14
(`bench/lemon_score.py`). Binaries: A = `df2a3c13` (log only, old GC walk), C = `283b26bb` (K41),
K09 = `gpuinc-k09` (K41 + K09 adapted).

| arm | runs (UTC stamp) | result |
|---|---|---|
| A, old walk | 231315 / 233056 / 233604 | LEMON / normal / normal |
| C, K41 walk | 230812 / 233300 / 233906 | normal / LEMON / normal |
| K09 on top of K41 | 001358 / 002137 | LEMON / normal |

(The first A run, 225916, is invalid: the monitor was missing.)

Verdict: **neither fix removes the lemon state.** Both arms show lemon in 1 of 3 runs, K09 in 1 of 2.
With a base rate near one third, 2-3 runs per arm cannot show a small effect, but the lemon runs are not
rarer with K41 or K09, so neither is the cause. Observations for the next attempt:
- In the K41 build, the `--debug-texture-gc` log shows frees of GPU-written images (20-25 per run, all
  `safe=1 downloaded=1`, none maybe-CPU-dirty), in the normal and the lemon run alike.
  The lemon run does not differ in what the GC frees.
- The lemon runs are bright at s12 already (s12 mean R above 220), the normal ones are still fading in
  (s12 mean R 80-180): the lemon state coincides with a faster story-time progress at equal route
  offsets. That points at timing (async pipelines, lookahead, frame pacing), the stop rule's next step,
  rather than at the cache contents.
- K09 is kept on the scratch branch `gpuinc-k09` (local, not pushed). The adapted port omits the
  wave-halves handling of Bry's tree. Its unit case `VectorWaveOrReduceWithUnlaunchedLanesW64T20` fails
  on NVIDIA in the 2-lane (wave64-on-32) emulation: the case expects the high half to read the low half's
  OR, which only holds with a 64-wide host subgroup. The combined OR is right; K09 would need that case
  made host-aware before it can merge, and it has no effect on the lemon state.

Next (stop rule): look at async timing, with `--async-pipelines skip` / `off` vs `stall` on the same
route and checkpoint scoring, 3 runs each.

## GPU-thread profile (K52, `--gpu-phase-stats`) and the K17 gate (2026-10-12)

Run `20261012T003352Z-gpuinc-prof` (merge `42e9bad5`, skip3 route to s15, lookahead `all` + `--async-pipelines
stall`, pin `--gpu-timestamps gpu --gpu-time-scale 1.0`, `--gpu-phase-stats 10`, headless 60 Hz, fresh cache).
Shares of kyGpu wall time over the four windows after the desert starts (t >= 107 s, 52.7 s, 1976 frames,
37.5 fps with the timers on, 362 submits per frame). A run with the timers is for shares, not fps.

| phase | share | | phase | share |
|---|---|---|---|---|
| drain_tick (wait for a GPU tick) | 35.9 % | | srt | 2.4 % |
| pipeline_job (wait for a worker) | 18.1 % | | vkcmd | 2.3 % |
| pipeline (lookup/creation on kyGpu) | 11.1 % | | vkalloc | 2.2 % |
| descriptors | 7.7 % | | idle | 2.4 % |
| draw (recording minus sub-phases) | 6.6 % | | drain_flush/finish, gc | 0.1 % |
| pm4 (parsing, handlers) | 5.8 % | | submit (vkQueueSubmit) | 5.3 % |

- **K17 gate (vkCmd + driver >= 25 %): not met.** vkcmd + vkalloc + submit = 9.8 %; adding draw recording and
  descriptor writes (the widest reading of "vkCmd + driver") gives 24.1 %, still under 25 %. kyGpu is
  waiting rather than computing: drain_tick plus pipeline_job is 54 %.
- Drains (tick waits) are about 6000 per 10 s window, charged mostly to draw packets (0x2d DRAW_INDEX_AUTO,
  0x35 DRAW_INDEX_OFFSET_2), indirect dispatch (0x16) and indirect draw (0x25): K20-K22 (drains > 3 ms per
  frame) are justified: 36 % of a 16.7 ms frame is 6 ms.
- The 11 % in `pipeline` with lookahead on is program/pipeline lookup and creation misses on kyGpu itself;
  worth a look before any restructure.
- PM4 + SRT + tracker work is under 10 %, so the "one-build experiment" condition for K18/K19 (above 50 %)
  does not hold either.

## K13 submit coalescing + K43 GC clock, 1 vs 1 (3 vs 3) (2026-10-12)

Binary `kyty_emulator.gpuinc-m` (merge `42e9bad5`), same pin, `--submit-stats 10` (counters only). Off =
default flags; On = `--submit-coalesce true --gc-clock frames` (max 16 events, age = vblank period / 8).
Last 45 s of frames.csv, desert; alternated off/on, 3 runs each.

| | off (3 runs) | on (3 runs) |
|---|---|---|
| Vulkan submits per frame | 354-362 | 45.8-47.7 (-7.7x) |
| fps | 41.46 / 41.31 / 42.64 (mean 41.8) | 44.22 / 43.89 / 44.59 (mean 44.2, +5.8 %) |
| frames at 16.7 ms / at 33 ms | 69-71 % / 17-19 % | 78-80 % / 10-11 % |
| frames over 50 ms | 25 / 27 / 37 | 25 / 28 / 27 |
| kyGpu CPU, p90 of 1 s samples | 57 / 58 / 57 | 52 / 52 / 53 |
| guest GPU timestamp lag behind parse time (mean) | 1.67 / 1.65 / 1.85 ms | 1.61 / 1.65 / 1.60 ms |

Verdict: success criteria met (submits -5x or more, fps +5 %, kyGpu p90 lower), vblank pacing better,
no VRAM exit, no timestamp ring overflow or unavailable query with either setting. The timestamp lag is
the same on and off, so batching does not move the K51 EOP timestamps. Caveats: the lemon state (above)
hit 5 of the 6 runs (off 3, on 2; the one sand-coloured run was an on run and had the same fps), so every comparison is in
the same scene state; the route reached its first checkpoint at 52-108 s with no pattern by arm. Default
stays off until a play session with the pad on confirms input latency and the GC age change (K43) on a
long route; the interval knob is `--submit-coalesce-us` / `--submit-coalesce-max`.
