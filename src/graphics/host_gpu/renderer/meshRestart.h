#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHRESTART_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHRESTART_H_

#include <cstdint>
#include <cstring>
#include <vector>

namespace Libs::Graphics {

/// A run of indices between two primitive-restart markers: `count` indices starting at index
/// number `first` of the draw's index buffer.
struct MeshRestartSegment {
	uint32_t first = 0;
	uint32_t count = 0;
};

/**
 * @brief Splits an indexed strip or fan draw at its primitive-restart markers.
 *
 * Mesh-emulated strips and fans assemble primitives from the draw's first index on: a fan's centre
 * is vertex 0 of the draw and a strip's winding follows the primitive number. Neither knows about
 * restart, which hardware honours (a marker ends the strip or fan, the next index starts a new one
 * with fresh winding and a new centre). Drawing each run between markers as a draw of its own gives
 * exactly that, and the markers are never read as vertices.
 *
 * @param indices       Index data (little endian, `element_size` bytes per index).
 * @param element_size  1, 2 or 4.
 * @param count         Number of indices.
 * @param marker        The restart index masked to `element_size` (all bits set for the markers
 *                      the renderer enables).
 * @return              Runs of at least one index, in order; empty runs are dropped.
 */
inline std::vector<MeshRestartSegment> SplitAtPrimitiveRestart(const void* indices,
                                                               uint32_t element_size,
                                                               uint32_t count, uint32_t marker) {
	std::vector<MeshRestartSegment> segments;
	if (indices == nullptr || (element_size != 1u && element_size != 2u && element_size != 4u)) {
		return segments;
	}
	const auto* bytes = static_cast<const uint8_t*>(indices);
	uint32_t    first = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t index = 0;
		std::memcpy(&index, bytes + static_cast<uint64_t>(i) * element_size, element_size);
		if (index == marker) {
			if (i > first) {
				segments.push_back({first, i - first});
			}
			first = i + 1u;
		}
	}
	if (count > first) {
		segments.push_back({first, count - first});
	}
	return segments;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHRESTART_H_
