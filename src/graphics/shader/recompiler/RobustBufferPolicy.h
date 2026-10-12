#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ROBUSTBUFFERPOLICY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ROBUSTBUFFERPOLICY_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler {

// --spirv-robust-bounds (docs/emitter-shape-notes.md): which storage-buffer loads the SPIR-V emitter
// may leave to VK_EXT_robustness2 robustBufferAccess2 instead of wrapping each one in an
// `if (index < OpArrayLength)` block. robustBufferAccess2 makes an out-of-bounds load return zero,
// which is what the explicit check does; the blocks only cost driver compile time (the offline
// harness measured -37 % on the largest compute shader, -53 % blocks over a whole first-play run).
//
// Loads only. Stores keep their explicit check: robustBufferAccess2 would drop an out-of-bounds
// store too, but NVIDIA rounds a dword access up to whole dwords of an odd-sized range (it writes
// the tail dword the explicit check dropped) and a pixel shader with unguarded stores hung the GPU.
// Atomics (out-of-bounds atomics are unspecified), typed accesses (whole-record check with format
// default values) and everything that is not a storage buffer also keep their explicit checks.
//
// Loads of 32-bit elements are checked exactly by the driver when
// robustStorageBufferAccessSizeAlignment is at most 4. Loads of 8/16-bit elements need alignment 1,
// otherwise the driver rounds the range up to the alignment.
struct RobustBufferPolicy {
	bool dword_elements    = false; // loads of 32-bit storage elements
	bool subdword_elements = false; // loads of 8/16-bit storage elements

	[[nodiscard]] bool Any() const { return dword_elements || subdword_elements; }
	bool operator==(const RobustBufferPolicy&) const = default;
};

// `requested`: the user asked for it (--spirv-robust-bounds). `robust_buffer_access2`: the device
// was created with robustBufferAccess2 enabled. `storage_alignment`:
// VkPhysicalDeviceRobustness2PropertiesEXT::robustStorageBufferAccessSizeAlignment (0 = unknown).
[[nodiscard]] constexpr RobustBufferPolicy MakeRobustBufferPolicy(bool requested,
                                                                  bool robust_buffer_access2,
                                                                  uint32_t storage_alignment) {
	RobustBufferPolicy policy;
	if (!requested || !robust_buffer_access2 || storage_alignment == 0u) {
		return policy;
	}
	policy.dword_elements    = storage_alignment <= 4u;
	policy.subdword_elements = storage_alignment == 1u;
	return policy;
}

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_ROBUSTBUFFERPOLICY_H_ */
