# The level-load GPU hang (NVRM Xid 109) and the GDS append bug behind it

## Symptom
Entering the first level (Sky Garden) hung the GPU about 187 s after the first frame: the frame counter stopped, every
host thread idle, `kyGpu` blocked in `vkWaitSemaphores`, and `NVRM: Xid ... 109 ... CTX SWITCH TIMEOUT` in the kernel log.
It was independent of every runtime flag. The last log lines were a mesh-shader pipeline compile and the "primitive restart is
not implemented for mesh shaders" warning, which turned out to be unrelated: they only happen to precede the level load.

## How it was found
`--debug-gpu-ops checkpoints` (see below) named the hung work: always the same submission, six compute dispatches and a
fullscreen draw, and with NV diagnostic checkpoints exactly one dispatch: `cs 657ad04626bf9d55`, 4096 x 1 x 1 groups.
Its SPIR-V has a loop that walks a linked list: node `idx` is `{float depth; uint next}` in an 8 MiB buffer and the loop is
`while (idx != 0) idx = node[idx].next`. Dumping the buffer of the hung dispatch (`--debug-dump-shader`) showed the lists had
**cycles**: 5 nodes whose `next` points at themselves and one 4-node cycle (a healthy run: 0). A cycle never reaches 0, so
every thread spins until the driver gives up.

The lists are built by about twenty pixel shaders. Each does a wave-aggregated allocation of a node index from a **GDS
counter** (`ds_append` / `ds_add gds`), then `imageAtomicExchange` on a head-pointer texture and stores the node. A node index
that is handed out twice makes `exchange(head, idx)` return `idx` itself: a self loop.

## The bug (GDS DS_APPEND with fragment helper invocations)
`ds_append` on GDS is emulated wave-aggregated: a ballot of the active lanes gives the count and the first lane, the first lane
does one `atomicAdd(counter, count)`, its result is broadcast and every lane's index is `base + rank`. In a pixel shader the
ballot **includes fragment helper invocations** (the padding lanes of a partially covered quad), but a host drops the stores and
atomics of helper invocations. When the lowest lane of a subgroup is a helper, which is common at triangle edges, the atomic
is never performed: the counter does not advance and all lanes of the subgroup get the same stale base (0). Two subgroups then
allocate overlapping node ranges, the head exchange returns an index that is already in the list, and the list closes on itself.

The counter resets are fine: they are `vkCmdFillBuffer` on the GDS buffer in the command stream (GPU order), with barriers.
The bug was in the atomic path, not the ordering.

Fix (`e53bc3ed`): ballots in pixel shaders, and the internal lane-activity ballot of swizzle/bpermute, mask out
`gl_HelperInvocation`; the input is declared only for pixel shaders that use a ballot. Ported from Jean-Philippe's
KytyPS5/KytyPS5#761 (as carried by BryanKAdams/BryKytyPS5 `5b0479adb`), with the fork's GPU tests.

Proof:
- `GraphicsDsAppendHelperQuad` (a real draw, `ctest shader_recompiler_helper_quad`): DS_APPEND in a 2x2 target with a helper
  quad. Without the fix on this NVIDIA GPU the counter stays at 10 (delta 0) and every lane gets base 0 with ranks 1, 2, 3.
  With the fix base 10, ranks 0, 1, 2 and the counter advances by 3. `Wave64AppendIndexFromExecRank` covers the compute path.
  (The fork's tests used M0 = 8; our GDS region check needs the M0 size to exceed the DS offset, so they use 0x40.)
- Level run without the loop guard: the level loads, 15 checkpoints and 120+ s in the level, no Xid, no wait. The node buffer
  of the last `cs 657ad046` dispatch: 94,208 nodes, **0 cycles, 0 self loops, 0 out-of-range next pointers**.

## Diagnostics added (all behind flags, off by default)
- `--debug-gpu-ops <off|ring|checkpoints>`: a host-side ring of the last 32768 draws and dispatches (shader hashes, primitive,
  counts, mesh groups, indirect-args address and the guest's view of its counts) with the timeline tick of the carrying
  submission. It is printed for the submissions the GPU has not completed from the hang-watchdog report, when a semaphore wait
  lasts 5 s (then 10 s, 20 s ...), and on device loss. `checkpoints` also enables `VK_NV_device_diagnostic_checkpoints` and
  marks every op, so the queue's reached checkpoints name the op the GPU was in. Also prints each shader's buffer bindings
  once and the GDS DMAs. (Ideas from Yotei Fork 5a1f94e86 and ogeidexe 5ca09b4b2; re-implemented.)
- `MasterSemaphore::Wait` waits in 1 s slices: a lost device ends in a fatal error naming the wait instead of a frozen process.
- `--debug-dump-shader <hash,...>`: SPIR-V and guest binary of those shaders; with `--debug-gpu-ops`, the whole input buffers
  of each dispatch of that shader (`_dispatch_latest_buf<N>.bin`, overwritten, so after a hang they are the hung dispatch's).

## The loop guard (`--shader-loop-limit`, on by default)
A safety net, not the fix. Each shader invocation gets a budget of loop-header executions shared by all of its loops (default
1,048,576); when it is used up the exit edges of its structured loops are taken, so a loop over corrupt data costs a bounded
time instead of an Xid. A trip is never silent: invocations that used at least limit/64 iterations record the count into
a host-only tail of the GDS buffer (`atomicMax`, one dword per shader slot, at every return) and the buffer cache logs, once per
shader, `LOOP GUARD TRIPPED: shader <stage> <hash> ... please report it`, plus high-water marks. A trip means an emulation bug.
`common/loopGuard.h` holds the logic and `tests/LoopGuardTests.cpp` tests it; `TestShaderLoopLimitGuard` (shaderCfgTests)
checks the SPIR-V (budget variable, exit edges, nested loops, the GDS binding, the report).

Limit value from data: runs with the limit at 65536 (report threshold 1024) over the title and intro, the desert gameplay route and
the whole level load plus the level reported a single shader above 1024: `cs 01d6f21611218e74`, with a high-water mark of
2,049 loop iterations in the level and **48,201** on the intro/desert route (legitimate as far as we can tell: it is far from
the limit and nothing is wrong on screen). The list walker that hung stays under 1,024 with healthy data. The default is
therefore 1,048,576, 21x the largest legitimate use observed; at a trip that is on the order of a second of GPU time
for a walker thread, below what the driver tolerates, where an endless loop was an Xid. 65536 would have been only 1.36x
above the desert route's use.
Programs that need the dispatcher fallback (no structured control flow) are not guarded.

## Route
`bench/routes/lvl23.json` (closed loop, because the ship's speed is per game frame and a recorded stick timing is not
repeatable): the fly anchor compares only the left 76 % of the frame (the desktop toast column and the R2 BOOST HUD on the
right made the old full-frame anchor sit at 30-38 against a limit of 30), the map is reached by pressing A every 0.6 s while
flying, and on the map the stick is nudged for 250 ms with every A press (`pulse` option of `tap_until_image`) until the
Sky Garden banner (DIVE IN) is on screen.

## Known limits
- Lit geometry (the planet and ship on the galaxy map, rocks and birds in the level) renders as black silhouettes in these runs,
  with or without the fix; the node lists are healthy, so it is a separate defect.
- The level runs at about 6 fps in these runs (58 submits a frame); the hang fix does not change that.
