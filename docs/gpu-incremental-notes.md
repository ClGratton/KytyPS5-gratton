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
