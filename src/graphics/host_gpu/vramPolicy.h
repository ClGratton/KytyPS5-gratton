#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_

// Pure, dependency-free VRAM policy: which images get their own allocation, the texture GC
// thresholds derived from the device budget, and the order of the image allocation fallbacks.
// Kept separate from the Vulkan code so it can be unit-tested (tests/VramPolicyTests.cpp).

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics::VramPolicy {

constexpr uint64_t MiB = 1024ull * 1024;
constexpr uint64_t GiB = 1024ull * MiB;

// Images at least this large get a dedicated VkDeviceMemory, so freeing one returns exactly its
// size to the driver instead of leaving a hole in a shared block.
constexpr uint64_t DedicatedImageMinBytes = 8 * MiB;
// The driver limits the number of live allocations (maxMemoryAllocationCount, 4096 on NVIDIA).
constexpr uint32_t MaxDedicatedImages = 1024;

struct ImageShape {
	uint32_t width      = 1;
	uint32_t height     = 1;
	uint32_t depth      = 1;
	uint32_t layers     = 1;
	uint32_t mip_levels = 1;
	uint32_t samples    = 1;
	int      format     = 0; // VkFormat value
};

// Rough size of an image (all mips and layers), only used to choose the allocation strategy.
[[nodiscard]] inline uint64_t EstimateImageBytes(const ImageShape& shape) {
	const int format          = shape.format;
	double    bytes_per_texel = 4.0;
	if (format >= 131 && format <= 184) { // block compressed (BC, ETC2, EAC, ASTC)
		const bool half = format == 131 || format == 132 || format == 133 || format == 134 ||
		                  format == 139 || format == 140; // BC1 and BC4
		bytes_per_texel = half ? 0.5 : 1.0;
	} else if ((format >= 9 && format <= 15) || format == 127) { // R8, S8
		bytes_per_texel = 1.0;
	} else if ((format >= 16 && format <= 22) || (format >= 70 && format <= 76) ||
	           format == 124) { // RG8, R16, D16
		bytes_per_texel = 2.0;
	} else if ((format >= 91 && format <= 97) || (format >= 101 && format <= 103)) { // RGBA16, RG32
		bytes_per_texel = 8.0;
	} else if (format >= 107 && format <= 109) { // RGBA32
		bytes_per_texel = 16.0;
	}
	const double levels = shape.mip_levels > 1 ? 4.0 / 3.0 : 1.0;
	return static_cast<uint64_t>(static_cast<double>(shape.width) * shape.height * shape.depth *
	                             shape.layers * bytes_per_texel * levels *
	                             static_cast<double>(std::max(shape.samples, 1u)));
}

[[nodiscard]] inline bool UseDedicatedAllocation(uint64_t estimated_bytes,
                                                 uint32_t live_dedicated_images) {
	return estimated_bytes >= DedicatedImageMinBytes && live_dedicated_images < MaxDedicatedImages;
}

struct GcThresholds {
	uint64_t trigger  = 0; // below this the texture GC does nothing
	uint64_t pressure = 0; // above this it evicts older and more images per tick
	uint64_t critical = 0; // above this it is aggressive
};

// All three follow the usable device budget (the driver budget minus a safety margin), never a
// fixed size: on a card of 8 GiB or less the trigger is 0 (always collecting).
[[nodiscard]] inline GcThresholds ComputeGcThresholds(int64_t budget) {
	const int64_t gib       = static_cast<int64_t>(GiB);
	const int64_t threshold = std::min<int64_t>(budget, 8 * gib);
	GcThresholds  result;
	result.pressure = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - gib), gib + gib / 2));
	result.critical = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - gib / 2), 3 * gib));
	result.trigger = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	return result;
}

enum class AllocStage {
	Normal,       // DEVICE_LOCAL required
	AfterReclaim, // DEVICE_LOCAL required, after the caches gave memory back
	HostFallback, // any memory type: a slow image beats a dead emulator
};

struct AllocOutcome {
	bool       ok    = false;
	AllocStage stage = AllocStage::Normal; // the stage that succeeded, or the last one tried
	bool       reclaimed = false;
};

// Image allocation ladder: normal, then (if `can_reclaim`) reclaim and retry once, then the
// host-memory last resort. `try_allocate(stage)` returns true on success; `reclaim()` frees what
// the caches can spare. Returns what happened so the caller can log it.
template<class TryAllocate, class Reclaim>
[[nodiscard]] AllocOutcome RunAllocationLadder(TryAllocate&& try_allocate, Reclaim&& reclaim,
                                               bool can_reclaim) {
	AllocOutcome outcome;
	if (try_allocate(AllocStage::Normal)) {
		outcome.ok = true;
		return outcome;
	}
	if (can_reclaim) {
		reclaim();
		outcome.reclaimed = true;
		outcome.stage     = AllocStage::AfterReclaim;
		if (try_allocate(AllocStage::AfterReclaim)) {
			outcome.ok = true;
			return outcome;
		}
	}
	outcome.stage = AllocStage::HostFallback;
	outcome.ok    = try_allocate(AllocStage::HostFallback);
	return outcome;
}

} // namespace Libs::Graphics::VramPolicy

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VRAMPOLICY_H_ */
