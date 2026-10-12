# GPU-thread drains: classification, K14, K20, K21/K22 (branch gpu-incremental)

All flags default off. Measured on headless 60 Hz, pin `--gpu-timestamps gpu --gpu-time-scale 1.0`, route
`skip3` to s15, fresh cache, `--submit-coalesce true --gc-clock frames` (K13+K43) on in both arms.

## Classification (`--gpu-phase-stats`)
Every `CommandScheduler::Wait` on the GPU thread is tagged with a cause (`common/drainCauses.h`), the
report lists drains by cause (calls / full / ms), the hot readback regions, the call stacks of the GPU
thread's own page faults (raw offsets for addr2line against an unstripped link) and the guest barriers.

Desert, K13+K43 only: about 12 drains per frame, **all full** (a submit plus a wait for the whole queue),
about 0.75 ms each. Frame_pool waits are on the presentation thread, not kyGpu. By cause:

| cause | per frame | share of drain time | what it is |
|---|---|---|---|
| readback_texture_meta | 6 | 45 % | colour-surface metadata (DCC/CMask keys) read back to find clear colours |
| buffer_readback | 1 | 30 % | shader-dependency reads (SRT walk, `SrtReadCapture::ReadStrict`) of GPU-written memory |
| readback_fault_gpu | 3 | 21 % | the GPU thread's own faults: vertex tables/shader metadata on GPU-shared pages (2), indirect draw arguments (`DrawIndirectMulti`) |
| readback_fault | 2 | 4 % | game-thread reads of GPU-written memory |

## What was cut
- **K14 `--elide-barriers`:** a guest flush/partial-flush barrier is left out when nothing was recorded in
  the same command buffer since the previous one (`CommandBuffer::RecordedWork`, rule in
  `barrierElision.h`). About 52 % of the roughly 865 guest barriers per frame are elided.
- **K22 `--known-fills`:** the buffer cache remembers ranges last written by one GPU uniform dword fill
  (`knownFills.h`); `MaterializeColorClear` uses the known value instead of a readback, and drops the
  GPU-modified state of the slice it then overwrites (`TryDiscardGpuModified`). Removes all
  readback_texture_meta drains (0 left; hits are counted, discard refused 0 times).
- **K20 `--clean-guest-reads`:** shader metadata and the two vertex tables are read through the guest
  backing when no byte is GPU-modified (`BufferCache::TryReadCleanBytes`), one copy per table per draw.
  Removes the vertex-table faults.
- **K20 `--readback-queue` (no gain, keep off):** synchronous readbacks copy on a second queue after a
  timeline wait on the producer's tick (`readbackPlan.h`). 47 % of the readbacks have their producer
  still in the open command buffer and the rest wait about 7 ticks; the data is needed right after it is
  produced, so the wait is the GPU backlog whichever queue copies. Per-call wait 2.9 -> 3.6-4.0 ms.

## Remaining drains and why
About 4-5 per frame, mostly one big one: a compute pass writes the indirect draw arguments (and a small
table the SRT walk reads), the CPU needs them at once, so it waits for everything queued before. Only a
GPU-resident path removes it: K21 (mesh-emulated and plain indirect draws built on the GPU, see
BryanKAdams/BryKytyPS5 1a6817502 and d8449bd8e; Supermedo 7f7a2d2c1 covers plain indexed draws only). It
needs the mesh shader to load its draw record through a device address, a record ring and a one-thread
compute pass, and a decision for the user-SGPR offset registers that `PatchIndirectDrawOffsets` patches on
the CPU. Not started.

## Verification
Unit tests: `drain_causes`, `barrier_elision`, `known_fills`, `readback_plan`, the clean-reader hook in
`shader_vertex_metadata`. Full ctest 70/70 under the shared lock. Bench numbers are in `bench/RESULTS.md`.
