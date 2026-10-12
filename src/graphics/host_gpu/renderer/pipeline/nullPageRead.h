#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_

#include <algorithm>
#include <cstdint>
#include <span>

namespace Libs::Graphics {

/// No guest memory is mapped in the first 64 KiB of the address space.
inline constexpr uint64_t GuestNullPageEnd = 0x10000;

/**
 * @brief Satisfies a shader table read that goes through a null pointer.
 *
 * A draw the game leaves with an unset table pointer (during level loads) makes the resource
 * evaluation load from an address like 0x10. The GPU reads zeros from unmapped memory the shader
 * does not actually use; the host would fault. Reads that lie wholly inside the null page are
 * answered with zeros.
 *
 * @return true when the read was answered (the values are zero).
 */
inline bool ReadGuestNullPage(uint64_t address, std::span<uint32_t> values) {
	if (address >= GuestNullPageEnd || values.size_bytes() > GuestNullPageEnd - address) {
		return false;
	}
	std::ranges::fill(values, 0u);
	return true;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_NULLPAGEREAD_H_
