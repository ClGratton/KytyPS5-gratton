# emitter-shape notes: `--spirv-robust-bounds`, and the skip store-safety rule

Branch `emitter-shape`, from `gratton/gratton-next` (47902cb3). Council 2026-10-12 (first-play stutter): option B, make the
emitted SPIR-V cheap for the driver, gated on the offline compile harness (`tools/spirv-compile-bench`, branch
`compile-harness`). The harness said GO: NVIDIA's pipeline compile time is ms = 7.2 x kwords + 324 x kblocks - 760, so the lever is block
count, and the per-access `if (index < OpArrayLength)` blocks were 62 % of the blocks of the largest compute shader.

## `--spirv-robust-bounds <true|false>` (default true after the measurement, see below)
A plain storage-buffer **load** is emitted without its bounds block; VK_EXT_robustness2 `robustBufferAccess2` (already enabled on the
device) returns zero for an out-of-bounds load, which is what the block did. The `OpArrayLength` is only emitted when a check asks for it.
- Covered: `LoadWordPrepared`, `LoadSubwordPrepared`, scalar buffer reads (`EmitReadConstBuffer`), when `RobustBufferPolicy` allows the
  element size: 32-bit elements at `robustStorageBufferAccessSizeAlignment <= 4`, 8/16-bit elements only at 1. The policy is computed once
  from the device (`GraphicContext::SpirvRobustBufferPolicy`) and travels in `CompileOptions::robust_buffers` into `Spirv::EmitProgram`.
- Kept explicit: **all stores**, atomics (out-of-bounds atomics are unspecified), typed/formatted accesses (whole-record check with
  format default values), LDS/GDS/scratch, BDA. With the flag off, or on a device that does not qualify, the SPIR-V is bit for bit what it was.
- The scalar element index is a 32-bit sum of dword offsets; above 2^30 the driver's byte offset (index * 4) wraps into the buffer. It is
  clamped to 0x3fffffff (always out of bounds) before the access. Found by `ScalarBufferOffsetAlignmentAndCarry` in the GPU suite.

### Why not stores (measured)
1. NVIDIA 610.57 rounds a dword access up to whole dwords of an odd-sized range although it reports alignment 1: with a descriptor range of
   14 bytes a dword store at byte 12 writes all four bytes (`--robust-store-probe dword R`: exact for R multiple of 4, tail dword written
   otherwise; byte stores exact).
2. Unguarded dword stores **hang the GPU** (Xid 109 CTX SWITCH TIMEOUT, deterministic at frame ~437-536, 35 s into the route) in one pixel
   shader of the first-play route: a per-pixel linked list (GDS counter, image atomic exchange, node stores, discard at the end). Bisect: only
   the dword-store site hangs (load sites, sub-dword store site: fine), then by shader hash down to that one shader; every other shader
   with unguarded stores is fine (run with that shader excluded: valid). Rounding the host ranges down to whole dwords (Bryan's way, below)
   does not cure it, so it is not the tail rounding. Root cause not found (the shader has no loops; suspect ordering of the stores around
   the image atomic once the control flow is gone).
3. Rounding the host range down loses data: `RenderExecutorStencilBindingDiscovery` (a captured real shader's 16-bit buffer with an exact
   odd tail) fails, and the loss would hit every 8/16-bit access to the last 1-3 bytes of an odd-sized buffer.

### Known limit (loads)
A dword load that straddles the end of a descriptor range whose size is not a multiple of 4 reads the whole dword (the driver rounds the
range up), where the explicit check returned 0. A load past a range that is a multiple of 4, or far outside it, reads zero (tests).

### Relation to BryanKAdams/KytyPS5 B26 / B27 (forks/group-B.md)
B26 (`4dc0d5f8f`, `4cbea8bd3`, setting `hardware-buffer-bounds`, default on) leaves **word loads and stores** to robustBufferAccess2, rounds the
bound range down to whole dwords and binds a range under one dword as a null descriptor. We reproduced that variant (commit dropped, see
bench/RESULTS.md): on NVIDIA it hangs the linked-list shader and breaks the 16-bit exact-tail binding, so we keep stores explicit. B27
(branch-free `in ? load : 0` selects) is superseded for loads; for the checks that stay (stores, atomics, typed) it would turn blocks into
selects only for loads, so it was not needed. Credit for the idea of leaving range checks to the device: Bryan; implementation, tests and the
store analysis here are ours.

## `--async-pipelines skip`: never skip a draw whose shaders write memory
Skip mode dropped every graphics draw whose pipeline was not ready. A draw whose shaders store to a buffer or a storage image, do an image
atomic or touch GDS changes memory later frames read, so dropping it corrupts later frames. `DrawSkipSafety.h` decides from the decoded
instruction list (no translation): buffer/typed-buffer/flat stores, image stores, image atomics, GDS accesses, `DS_ORDERED_COUNT` and
undecodable instructions make a shader unsafe; buffer atomics alone, LDS and ALU do not. The verdict is per (hash, size), logged once, ANDed
over all stages of the draw in `GetGraphicsPrograms` and carried to the pipeline-job skip as `GraphicsPrograms::may_skip`. Such draws wait for
the pipeline like in stall mode. Rule and idea: BryanKAdams/KytyPS5@17da0ad52, @5949de94d (GPL-2.0), re-implemented.
Test: shader_cfg `TestDrawSkipSafety`.

## Tests
- shader_cfg: `TestRobustBufferPolicy`, `TestRobustBufferBoundsEmission` (loads lose the block; stores/atomics bit-identical),
  `TestDrawSkipSafety`; CLI ctests `emulator_spirv_robust_bounds_cli/_invalid`.
- GPU suite with `KYTY_TEST_ROBUST_BOUNDS=1` (device created with robustBufferAccess2): ctest `shader_recompiler_compute_robust_bounds` and
  `..._async_stall`; new cases `BufferOutOfBoundsLoadsZeroStoresDropped`, `BufferDwordStraddlingRangeEnd` (explicit mode only: with robust
  bounds the driver's tail rounding applies to loads), diagnostic `--robust-store-probe`.
- Full ctest 87/87.

## Verification and numbers
See bench/RESULTS.md, "emitter-shape". Offline harness on the real dumped modules (explicit vs robust loads): cs98 3450 -> 2182 ms (-37 %),
top-12 compute 15.9 -> 13.4 s (-15 %); over a whole first-play route the emitted modules shrink from 100164 to 46673 blocks (-53 %),
23755 -> 300 OpArrayLength. Flag off: all 255 modules of the previous binary's dump are bit-identical.
