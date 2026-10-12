#ifndef KYTY_COMMON_INDIRECTDRAWPOLICY_H_
#define KYTY_COMMON_INDIRECTDRAWPOLICY_H_

// --gpu-indirect-draws (K21): which indirect draws may be built on the GPU, and why the others take
// the CPU path. Pure; unit tested in tests/IndirectDrawPolicyTests.cpp. The command processor
// fills Inputs from its state, takes the route, counts it, and logs each fallback reason once.

#include <array>
#include <cstddef>
#include <cstdint>

namespace Common::IndirectDraw {

enum class Route : uint8_t {
	Gpu,          // built on the GPU from the guest argument block
	CpuNotGpuData, // the arguments were not written by the GPU: the CPU read costs no drain
	CpuNotIndexed, // DRAW_INDIRECT (non-indexed) is not built on the GPU yet
	CpuPrimitive,  // not mesh-emulated and not a point/line/triangle list (strips, rects, quads)
	CpuNoFirstInstance, // the device cannot take a first instance from an indirect command
	CpuIndex8,     // 8-bit indices are widened on the CPU
	CpuNoIndexLimit, // INDEX_BUFFER_SIZE unknown: the index buffer extent cannot be registered
	CpuCountInMemory, // the draw count comes from guest memory (a CPU read)
	CpuOffsetRegisters, // the program reads the user SGPRs the CP patches from the arguments
	Count,
};

inline constexpr std::array<const char*, static_cast<std::size_t>(Route::Count)> kRouteNames {
    "gpu",
    "cpu: arguments not GPU-written",
    "cpu: not an indexed draw",
    "cpu: primitive type needs the CPU path (not a list, not mesh-emulated)",
    "cpu: device lacks drawIndirectFirstInstance",
    "cpu: 8-bit indices",
    "cpu: INDEX_BUFFER_SIZE unknown",
    "cpu: draw count read from guest memory",
    "cpu: program reads the patched offset SGPRs",
};

struct Inputs {
	bool     indexed              = false;
	bool     arguments_gpu_data   = false; // the argument range is GPU-modified
	bool     mesh_emulated        = false;
	bool     list_primitive       = false; // point, line or triangle list
	bool     first_instance_ok    = false; // drawIndirectFirstInstance
	uint32_t index_element_bytes  = 0;     // 1, 2 or 4
	uint32_t index_buffer_size    = 0;     // elements; 0 = unknown
	bool     count_in_memory      = false;
};

// A "not GPU data" draw is the normal CPU path and not a fallback worth logging.
[[nodiscard]] constexpr Route Decide(const Inputs& in) {
	if (!in.arguments_gpu_data) {
		return Route::CpuNotGpuData;
	}
	if (!in.indexed) {
		return Route::CpuNotIndexed;
	}
	if (!in.mesh_emulated && !in.list_primitive) {
		return Route::CpuPrimitive;
	}
	if (!in.mesh_emulated && !in.first_instance_ok) {
		return Route::CpuNoFirstInstance;
	}
	if (in.index_element_bytes != 2 && in.index_element_bytes != 4) {
		return Route::CpuIndex8;
	}
	if (in.index_buffer_size == 0) {
		return Route::CpuNoIndexLimit;
	}
	if (in.count_in_memory) {
		return Route::CpuCountInMemory;
	}
	return Route::Gpu;
}

// The draw packet names the user SGPRs the command processor patches with the base vertex, first
// instance and first index of each draw (SH register offsets; SH_NOP = not patched). The GPU path
// leaves those SGPRs unpatched, so it is only exact when the vertex program never reads them. Its
// registers are the merged GS stage's user-data window; anything else cannot be reasoned about.
inline constexpr int32_t kNotPatched    = -1;
inline constexpr int32_t kUnknownPatch  = -2;

[[nodiscard]] constexpr int32_t PatchedSgpr(uint32_t reg, uint32_t nop_reg, uint32_t gs_user_data_0,
                                            uint32_t gs_user_data_count) {
	if (reg == nop_reg) {
		return kNotPatched;
	}
	if (reg >= gs_user_data_0 && reg - gs_user_data_0 < gs_user_data_count) {
		return static_cast<int32_t>(reg - gs_user_data_0);
	}
	return kUnknownPatch;
}

// `regs` are the absolute user-data registers the program reads (program.info.user_data_registers,
// which include the stage's base register); `user_data_base` is the base the patched SGPR indices
// are relative to.
template <typename Registers>
[[nodiscard]] constexpr bool PatchedOffsetsRead(const int32_t (&patched)[3], uint32_t user_data_base,
                                                const Registers& regs) {
	for (const int32_t sgpr: patched) {
		if (sgpr == kNotPatched) {
			continue;
		}
		if (sgpr == kUnknownPatch) {
			return true;
		}
		const auto absolute = user_data_base + static_cast<uint32_t>(sgpr);
		for (const auto reg: regs) {
			if (static_cast<uint32_t>(reg) == absolute) {
				return true;
			}
		}
	}
	return false;
}

// Fallbacks are reported once each; a draw whose arguments the CPU wrote is the normal path.
[[nodiscard]] constexpr bool IsFallbackWorthLogging(Route route) {
	return route != Route::Gpu && route != Route::CpuNotGpuData;
}

} // namespace Common::IndirectDraw

#endif // KYTY_COMMON_INDIRECTDRAWPOLICY_H_
