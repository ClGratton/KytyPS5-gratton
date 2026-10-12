# spirv-compile-bench: offline driver-compile timing

Question it answers: is the multi-second first-play pipeline compile caused by the shape and size of the
SPIR-V we emit, or is it inherent driver cost? It times `vkCreateComputePipelines` (and
`vkCreateShadersEXT`, and a graphics VS/PS pair) on dumped SPIR-V without running the emulator.

## Pieces (all in `tools/spirv-compile-bench/`)
- `main.cpp` (target `spirv-compile-bench`): reflects the module (descriptor decorations, push constants,
  LocalSize, stage), builds a matching layout (push-descriptor set 0 like the emulator, 128 byte push range),
  creates the pipeline with **no pipeline cache**, repeats N times and prints one JSON line per shader.
  Each compile gets a unique `OpModuleProcessed` string so no cache can answer it, and
  `__GL_SHADER_DISK_CACHE=0` is set before the driver loads. The device enables every core feature the GPU
  supports and the same robustness features as the emulator (`--robustness emu`; `none|rb1|rb2` for A/B).
  `--mode shader-object`, `--parallel K [--distinct]`, `--gfx vs ps`, `--exec` (run once, dump buffers).
- `spvtool.py`: SPIR-V parser plus the rewrites (stdlib only, ids found from the Khronos grammar JSON).
  `nobounds` (all bounds-check ifs removed, diagnostic), `robust` (buffer checks removed, rely on
  robustBufferAccess2), `ldsclamp` (LDS check as index select + dummy slot, no branch), combinations.
- `run_matrix.py`: `manifest` (top-N slow compute shaders of a `--shader-timing-log` run to dump files),
  `matrix` (variants, spirv-val, spirv-opt with CPU time, timing), `parallel`, `selfcheck`, `graphics`, `gfxtime`.
- `test_variants.py` + `tests/guards.spvasm`: no-GPU checks (ctest `spirv_compile_bench_variants`).

## Getting the SPIR-V
`--graphics-debug-dump true --shader-log-folder <dir>` writes every compiled module as
`NNNN_new_shader_<stage>_<hash>.spv` (NNNN = shader id - 1). Dumps are game-derived: keep them under `bench/`.

## Build and run
    cmake -S tools/spirv-compile-bench -B build/compile-harness -G Ninja -DKYTY_THIRD_PARTY_DIR=<3rdparty>
    # or from the top level: -DKYTY_BUILD_TOOLS=ON
    spirv-compile-bench --reps 5 shader.spv
    run_matrix.py manifest|matrix|parallel|selfcheck ...
Hold the shared bench lock while timing (the harness compiles on the GPU).

## Safety of the rewrites
- `robust`: with robustBufferAccess2, out-of-bounds storage-buffer loads return 0 and stores are discarded
  (VK_EXT_robustness2), which is the GCN behaviour the emitter's `if (idx < OpArrayLength)` produces. Differences:
  the range is rounded up to `robustStorageBufferAccessSizeAlignment` (a partial tail dword is in bounds, GCN
  would return 0 for it), and atomics are not specified, so atomic accesses must keep their explicit check.
  `--exec` + `run_matrix.py selfcheck` compares results of the rewritten hand-written shader on the GPU.
- `ldsclamp`: loads become `select(in_bounds, lds[clamped], 0)`, stores go to a dummy slot (array length + 1);
  exact for loads, stores and atomics.

## Results (2026-10-12, RTX 3080 Ti, NVIDIA 610.57)
See `bench/RESULTS.md` section "Compile harness". Summary: compile ms = 7.2 x kwords + 324 x kblocks - 760;
removing the buffer bounds-check ifs (robustBufferAccess2) cuts cs98 by 37 % and the top-12 compute total by 18 %;
spirv-opt and VK_EXT_shader_object change nothing; parallel compiles scale 1.5x (2 threads) / 2.2x (4).
Known limits: graphics timing uses minimal fixed state (no blend/depth variants); `--exec` supports storage
buffers only; the top-level `KYTY_BUILD_TOOLS` hook and the ctest entry were not exercised (standalone build is).
