# Per-draw lookup cost: `--draw-key-cache` and `--srt-table-walk` (branch `draw-cache`)

Both flags default off. Measured on Astro Bot, headless 60 Hz, warm driver cache, pin `--gpu-timestamps gpu
--gpu-time-scale 1.0`, play flags of `bench/config-play-next.json`, route `skip3` to s14 (desert window).
Numbers and run ids: `bench/RESULTS.md` ("2026-10-12: draw-key-cache").

## What the 15 % "pipeline-key lookup" really was
The `pipeline` phase of `--gpu-phase-stats` covered everything between the PM4 draw packet and the pipeline
handle. Splitting it (new sub-phases `pipe_prepare`, `pipe_programkey`, `pipe_materialize`, `pipe_keybuild`,
`pipe_find`, `pipe_entrylock`, `pipe_srtread`, `pipe_permsearch`, exclusive times as before) shows, warm desert,
share of kyGpu wall time:

| sub-phase | share | what it is |
|---|---|---|
| `pipe_materialize` | 11.7 % | `MaterializeResources`: refresh of the shader's scalar-read table ("flat SRT"), descriptor values, resource specialization, per stage and draw (about 620 calls a frame, ~36 guest reads each) |
| `pipe_srtread` | 2.6 % | the guest-memory reads of that walk (about 21 000 a frame, ~20 ns each) |
| `pipe_prepare` | 0.9 % | `PrepareProgram`: shader map hit, params, static vertex/pixel input info |
| `pipe_find` | 0.7 % | hashing the pipeline key and probing the pipeline map |
| `pipe_entrylock` | 0.5 % | waiting for a program entry's lock (the lookahead thread holds it while it materializes) |
| `pipe_programkey` | 0.2 % | building the static-state key of the program cache and finding the source entry |
| `pipe_keybuild` | 0.1 % | `BuildGraphicsKey` |
| `pipe_permsearch` | 0.1 % | scanning the permutations of a program |

So the key itself (build, hash, map probe) is under 1 %; the cost is in the program cache's per-draw
resource materialization. A cache keyed on repeated results does not help: compared with the previous result
of the same program, only 3.5 % of the materializations had identical user data and descriptors (flattened
SRT identical 54 %, descriptors identical 9 %); every draw points at different constants and descriptors.

Inside `MaterializeResources` (cycle counters, temporary): the flat SRT refresh is 90 %; for the programs without
any conditional resource path (4 reads, 0.4 us) it is cheap, for the rest ("control flow", about 55 % of the
calls, ~80 scalar reads and ~50 blocks each) it is 12-17 us per call, spread over graph-evaluator overhead
(about 630 `EvaluateWide` calls and 190 instruction evaluations a refresh) and the branch conditions, not over
memory reads. The lookahead thread does the same work for the draws it predicts, so the total is about twice
what kyGpu pays.

## What was built
1. **Phase sub-timers** (observation only, free when `--gpu-phase-stats` is off).
2. **`DrawKeyMemo`** (`pipeline/drawKeyMemo.h`, unit tested): the last four graphics pipelines that hit, compared by
   `operator==` on the key before it is hashed and looked up. The memo returns a value only for a key equal to the
   one that earned it, so a changed key field can only miss. 85 % of the lookups hit. Credit: the "remember the
   last pipeline" idea is BryanKAdams/BryKytyPS5 c497660e5 (GPL-2.0, re-implemented with more ways and tests).
3. **Table walk of the SRT** (`--srt-table-walk`, `SrtRuntime::table_walk`):
   - for programs the native (Xbyak) walker accepts: the pointer tree is also kept as a flat `SrtTable` and run
     through the reader callback (the native walker needs raw loads and `read_memory == nullptr`, which the
     renderer never has, since its reader returns zeros for unmapped guest memory);
   - for programs with control flow: each scalar read whose pointer words are constants, user-data dwords or
     other reads carries precomputed address arithmetic (`SrtFastRead`) and skips the graph evaluator.
   Both run the same address checks and the same reader and seed the same memo entries; any failed check or read
   returns to the graph walk, so the answer is always the graph walk's.
4. **`--draw-key-cache true`** enables 2 and 3 together; `--srt-table-walk true` enables 3 alone;
   **`--debug-draw-key-verify true`** re-runs the map lookup on every memo hit (stops on a different pipeline)
   and redoes every table-walked refresh with the graph walk on a fresh memo (stops on a different flattened SRT).

## Measured (flags on = `--draw-key-cache true`)
Warm cache (each run warmed from the same fresh run of the same binary), 6 vs 6 alternated, window = checkpoint s01 to
s14 of the route (42 s of desert), headless 60 Hz, pin as above. The route is bimodal (state A: the pause-menu sync
waits under 4 s, state B: it waits 29-35 s and the window shows other content), so the like-for-like comparison is
state A only:

| | off (5 runs) | on (3 runs) |
|---|---|---|
| fps | 54.66 54.64 54.68 54.69 54.33, mean 54.60 | 55.04 55.09 55.21, mean 55.11 (+0.9 %) |
| frame p50 / p99 | 16.67 ms / 35.7-37.4 ms | 16.67 ms / 36.1-36.3 ms |
| frames at 16.7 ms | 87-88 % | 88-89 % |
| kyGpu CPU median | 46-48 % | 44-45 % |

State B (off 1 run: 54.16; on 3 runs: 57.43, 57.16, 54.57) is not comparable (other content, one arm only), so the
all-runs mean (+2.2 %) is not claimed. The gain is real but small because kyGpu spends 30-40 % of its time waiting
for the GPU: shorter CPU work moves its busy time, less its frame rate.

kyGpu phase shares, warm desert windows with `--gpu-phase-stats 10` (timers on, never an fps baseline), off -> on:
`pipe_materialize` 11.7 -> 9.3 %, `pipe_find` 0.7 -> 0.2 %, `pipe_entrylock` 0.5 -> 0.3 %, all `pipe_*` 16.7 -> 14.3 %
(the old `pipeline` phase: 15 %); drain_tick 32.7 -> 30.6 %, pm4 17.2 -> 23.1 % (the waiting moved into work), the rest
within 1 point. Per materialization on the GPU thread: 3.2 -> 2.5 us.

## Verification
- Unit tests: `draw_key_memo` (hit returns the pipeline of an equal key, any single field changed misses, LRU,
  agreement with a map on a 20 000-draw random stream) and `resource_materialization` (table and fast reads
  against the graph walk on values, set of guest reads and captured ranges, for two inputs and for missing words,
  truncated user data, null root, 48-bit wrap, a computed pointer word that must stay on the graph walk).
- Verify run (`config-pk-verify.json`, 127 s, binary of the A/B runs): 1.3 M memo hits all identical to the map, every
  table-walked refresh equal to its graph walk, no stop.
- Rendering checked by eye: checkpoints of off and on runs of the same route state show the same scenes
  (state A: the same frames up to timing). The desert lemon colour, the known intermittent state, appeared in 5 of
  6 off runs and 4 of 6 on runs; no other difference seen.
- Full `ctest -j2` on the build of the A/B binary: 84/84.

## Known limits
- The control-flow refresh still costs most of the old time: the fast reads cover the scalar reads, not the
  branch conditions or the descriptor evaluation. The next step is a compiled form of the whole refresh
  (conditions and descriptor expressions as a linear tape over memo slots); the profile above is its target.
- Verify mode reads twice and duplicates captured ranges; it is a debug aid.
- The memo holds pipeline pointers: it relies on the pipeline cache never erasing a `Pipeline` node while it lives
  (documented in `pipelineCache.h`).
