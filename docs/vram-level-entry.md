# VRAM at level entry: the hidden local memory, and a reclaim path that returns memory

Branch `vram-level-entry` (on `ram-release`). Measured on an RTX 3080 Ti (12 GiB, driver 610.57).

## Symptom
Entering the first level of a game at 4K guest resolution exited with
`Image allocation failed: ErrorOutOfDeviceMemory` and `Image allocation retry after reclaim: ErrorOutOfDeviceMemory`.
`nvidia-smi` showed the process at about 10.6 GiB while VMA and `VK_EXT_memory_budget` reported 7.1 GiB, and
the reclaim freed 830 MiB of allocations but only 73 MiB of blocks.

## Cause 1: a Function-storage LDS array of 32 KiB per invocation
Non-compute shaders (pixel, vertex) emulate LDS as a SPIR-V `Function` array of 8192 dwords (32 KiB per
invocation). The NVIDIA driver backs Function arrays with per-thread local memory and reserves
`bytes per thread x resident threads` of VRAM: 32 KiB x 80 SMs x 1536 threads = 3.75 GiB on a 3080 Ti.
That memory is not a Vulkan allocation: VMA and `VK_EXT_memory_budget` `usage` do not include it, but the
driver subtracts it from the `budget`, which is why the budget fell from 10.4 to 6.7 GiB at the same instant.
In the runs it appeared as one step, when the first pipeline with such an array was used, and was never returned.

Evidence (`--vram-stats`, run dirs under `bench/runs/`): `20261011T211343Z-lvlC` (`gpu_procs.csv`: process
3973 -> 8556 MiB within one 5 s sample while VMA usage moved 3.95 -> 4.69 GiB, budget 10461 -> 6689 MiB);
`20261011T212559Z-lvlE` (log: two pixel shaders with `function_lds=32768 B`); `20261011T213757Z-lvlG` onward (same
route, shrunk arrays: process 6.0 GiB, budget stays 10.1-10.4 GiB).

## Fix 1: size the array to what the shader can touch
`ShaderInfo::function_lds_dwords` records the span of the shader's LDS accesses (largest immediate byte offset
plus data size, plus one dword per lane of the wave, because the dynamic base is a per-lane address).
`IR::ReducedLdsDwords()` rounds it up to a power of two (min 64 dwords) and the emitter wraps the index with a
mask (`WrapLdsIndex`); the array is private to the invocation, so aliasing modulo the size is harmless when the
size covers the lane's addresses. Shaders with FLAT accesses that can reach LDS, and spans above half the full
array, keep the 8192-dword array and exact bounds. The two pixel shaders went from 32 KiB to 1 KiB and 2 KiB.
The translator logs `Shader local memory: ...` for every shader with at least 1 KiB of Function LDS or scratch.
Code: `src/graphics/shader/recompiler/ir/ldsSpan.h` (tests: `tests/LdsSpanTests.cpp`).

## Fix 2: reclaim that returns memory
- Images estimated at 8 MiB or more get a dedicated `VkDeviceMemory` (at most 1024 live), so freeing one returns
  exactly its size. VMA blocks are 64 MiB instead of 256 MiB, so freed images empty more blocks.
- The allocation failure path is `RunAllocationLadder`: allocate, on failure reclaim once and retry, then the
  host-memory last resort (any memory type, `--vram-paging` or not), then the old exit. In the baseline
  repro the reclaim alone recovered most allocation failures, but fragmentation made it fail in the user's session.
- The texture GC thresholds (`ComputeGcThresholds`) are derived from the driver budget and already follow
  the budget drop. Code: `src/graphics/host_gpu/vramPolicy.h` (tests: `tests/VramPolicyTests.cpp`).

## Diagnostics added
- `--vram-stats`: `driver usage minus VMA blocks`, pipeline counts, VMA block/allocation counts, dedicated and
  host-fallback image counts, and `Pipeline compile begin/slow` lines for graphics pipelines.
- `Shader local memory:` lines (always on, a few per run).
- Host threads carry OS-visible names (`kyGpu`, `kyPrioOps`, `kyPresent`, `kyWindow`, `kyGuestMain`, ...).
- `KYTY_ALLOW_PTRACE=1` lets `gdb`/`eu-stack` attach to a hung emulator under `kernel.yama.ptrace_scope=1`.

## Known limits
- A GPU hang (`NVRM Xid 109 CTX SWITCH TIMEOUT`, frame counter stuck, CPU idle) follows the first frames of the
  level in every run that gets past the load. It also happens with the LDS shrink and the mesh-restart draws disabled, the
  baseline binary stops at the same point (frame counter stuck), and stock gratton-main stopped the same way in an earlier
  session; so it is not caused by this branch and needs its own investigation.
- Non-compute shaders whose Function LDS span is unknown (FLAT access) still cost 32 KiB per thread.
- Function-storage scratch arrays (`function_scratch`) have the same per-thread cost and are not shrunk.
