# GPU-resident indirect draws (K21, branch gpu-indirect)

Flag `--gpu-indirect-draws <true|false>`, default **off**. Part of the GPU-thread drain work
(`docs/gpu-drains.md`): it removes the last big synchronous readback of a frame.

## Problem
`DRAW_INDEX_INDIRECT` takes its arguments (`DrawIndexedIndirectArgs`: index count, instance count,
first index, base vertex, first instance) from guest memory. Astro Bot writes them with a compute
pass earlier in the same frame. The command processor read them on the GPU thread to record the
draw, which faulted on the GPU-modified pages and waited for the whole queue (a full drain,
about 0.75 ms and the work queued before it).

## What runs on the GPU instead
The command processor sends the draw to the executor as a *GPU-args draw*
(`DrawIndexArgs::indirect_args`). The executor reserves a 48-byte slot in a device-addressable record
ring (`BufferCache::GetDrawRecordBuffer`, 4 MiB, created on first use) and records a one-thread
compute pass (`mesh_indirect_args.comp`, class `MeshIndirectArgs`) that reads the argument block and
writes the draw command into the slot:

| draw | pass output | draw call |
|---|---|---|
| plain indexed list (point, line, triangle) | `VkDrawIndexedIndirectCommand` with the count clamped to INDEX_BUFFER_SIZE (as the CPU path clamps it); base vertex and first instance only for an embedded fetch | `vkCmdDrawIndexedIndirect` |
| mesh-emulated indexed | the mesh draw record (the 6 dwords a direct mesh draw pushes) and a `VkDrawMeshTasksIndirectCommandEXT` from the host limits | `vkCmdDrawMeshTasksIndirectEXT` |

The index buffer is bound whole (`INDEX_BUFFER_SIZE` elements) and the first index travels in the
command, so nothing about the draw is read on the CPU. The arithmetic is modelled in
`indirectDrawArgs.h` (unit tests, and a GPU test that runs the shader and compares) .

Mesh shaders, when the flag is on, load their draw parameters from a record by device address
(push constant dwords 0 and 1) instead of from push constants (`ShaderMeshInputInfo::
draw_record_address`, part of the shader key; the emitter loads through a physical pointer). Direct
mesh draws write their record into the same ring. With the flag off nothing about mesh shaders changes.

## What stays on the CPU (each logged once, counted in `--gpu-phase-stats`)
`common/indirectDrawPolicy.h` decides, with these reasons:
arguments not GPU-written (no drain to avoid, the normal path) · not an indexed draw · strips, rects,
quads (primitive restart scan, tessellation) · 8-bit indices (widened on the CPU) · INDEX_BUFFER_SIZE
unknown · the draw count comes from guest memory (`count_addr`) · the device lacks
`drawIndirectFirstInstance` (now requested when supported) · **the vertex program reads the user
SGPRs the packet patches**.

### The user-SGPR offset patch (`PatchIndirectDrawOffsets`)
The CPU path writes base vertex, first instance and first index into the user SGPRs the packet names.
The GPU path cannot patch those values (it does not know them). It is exact when the vertex program
never reads the patched SGPRs: the draw packet's registers are mapped to merged-stage user SGPR
indices and compared with `program.info.user_data_registers` in the executor
(`Common::IndirectDraw::PatchedOffsetsRead`, unit tested). If a program reads one, the executor refuses
the draw before recording anything and the command processor takes the CPU path for that packet. In
Astro Bot's scene the packet names no registers (`SH_NOP` for all three) and the shader has an embedded
fetch, so the offsets reach the shader through the Vulkan base vertex and first instance, which the
argument pass copies.

### Instance count
A later draw that relies on the NUM_INSTANCES state an indirect draw set reads it on demand from guest
memory (`CommandProcessor::NumInstances`, a faulting read, rare).

## Credits
Ported from BryanKAdams/BryKytyPS5@1a6817502 (mesh-emulated draws built on the GPU) and @d8449bd8e
(plain indexed draws, count clamp), GPL-2.0, re-implemented with a pure model, the policy, the SGPR
check and tests.

## Verification
Unit tests (all in ctest, 72/72 with the shared lock, -j2):
- `indirect_draw_args`: the CPU model (clamp, grids, host limits, record offsets, 64-bit address carry,
  plain command).
- `indirect_draw_policy`: routing table, packet-register to SGPR mapping, patched-SGPR-read check.
- `shader_recompiler_compute` (`--mesh-indirect-only`, and in the full run): the argument pass on the
  GPU against the model for 11 argument blocks (mesh and plain).
- `shader_recompiler_compute --polygon-mode-only`: an indexed draw built from a GPU-args block gives
  the CPU path's pixels; a packet that patches a read user SGPR is refused with nothing recorded.
- `shader_cfg`: the by-address mesh shader validates and loads through a pointer; the default one does not.

Bench (Astro Bot, headless 60 Hz, pin `--gpu-timestamps gpu --gpu-time-scale 1.0`, skip3 to s15, fresh
cache, `--submit-coalesce true --gc-clock frames --elide-barriers true --known-fills true
--clean-guest-reads true` in both arms, binary `1bbe95a6`): see `bench/RESULTS.md`. Summary: full drains
per frame 5.1 -> 2.6 (the GPU thread's argument read is gone: the GPU-thread fault site and the
DRAW_INDEX_INDIRECT drain, 0.2-0.3 s per 10 s window, are gone), but the `drain_tick` share of kyGpu is
**31.3 % in both arms** and **fps is unchanged within noise** (47.7 -> 48.0, 3 vs 3). Why: the frame still
needs a GPU-written table on the CPU a little earlier (the DISPATCH_INDIRECT dependency read below,
1.1-2.4 s of drain per 10 s window, the largest wait in both arms), so the CPU still waits for the GPU
backlog once per frame; removing the later wait only moves where the GPU thread blocks. At the pin the
game also already sits at the 60 Hz cap in 90 % of frames, and the losses are pipeline-compile spikes.

## Known limits
- Astro Bot's one GPU-args draw per frame is a plain indexed list draw; its mesh-emulated indirect draws
  have CPU-written arguments, so the mesh path is covered by unit and GPU tests and exercised for
  parameter loading only (every mesh draw with the flag on), not for GPU-written mesh arguments.
- The remaining GPU-thread drain per frame is a shader-dependency read (`SrtReadCapture::ReadStrict`
  of a 0.1 KiB GPU-written table, drained during a DISPATCH_INDIRECT, 3.3 ms per call in the last
  windows); it is not an argument read. Removing it needs the descriptor table resolved on the GPU
  (a dynamic SRT walk), a much larger feature. Until it goes, K21 is a prerequisite with no fps of its
  own on this title.
- Game-thread faults on GPU-written memory (2-3 per frame) are unchanged and take longer when the GPU
  queue is deeper, so part of the saved wait reappears on the game thread.
- `DISPATCH_INDIRECT`: arguments are already used GPU-resident (`vkCmdDispatchIndirect`) unless the
  packet asks for thread dimensions or the shader keeps LDS in a storage buffer sized from the
  workgroup counts (`BindSharedMemory`); those two read the arguments on the CPU and are not changed.
