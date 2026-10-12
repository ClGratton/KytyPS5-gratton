#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GDSCOPYPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GDSCOPYPLAN_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct GdsCopyChunk {
	uint64_t source      = 0;
	uint64_t destination = 0;
	uint64_t size        = 0;
};

/**
 * @brief Plans a copy inside one buffer (GDS to GDS DMA) as non-overlapping `vkCmdCopyBuffer`
 *        regions that, executed in order, give the result of memmove.
 *
 * A Vulkan buffer copy with overlapping source and destination is undefined, but a DMA between
 * two GDS ranges may overlap. A chunk no larger than the distance between the two ranges cannot
 * overlap its own destination; copying the chunks from the low end when the destination is below
 * the source (and from the high end otherwise) never reads bytes an earlier chunk has written.
 */
inline std::vector<GdsCopyChunk> PlanGdsCopy(uint64_t destination, uint64_t source,
                                             uint64_t size) {
	std::vector<GdsCopyChunk> chunks;
	if (size == 0 || destination == source) {
		return chunks;
	}
	const uint64_t distance = destination > source ? destination - source : source - destination;
	if (distance >= size) {
		chunks.push_back({source, destination, size});
		return chunks;
	}
	if (destination < source) {
		for (uint64_t done = 0; done < size;) {
			const uint64_t part = size - done < distance ? size - done : distance;
			chunks.push_back({source + done, destination + done, part});
			done += part;
		}
	} else {
		for (uint64_t left = size; left > 0;) {
			const uint64_t part = left < distance ? left : distance;
			left -= part;
			chunks.push_back({source + left, destination + left, part});
		}
	}
	return chunks;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GDSCOPYPLAN_H_
