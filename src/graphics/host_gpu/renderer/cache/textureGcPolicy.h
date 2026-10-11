#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTUREGCPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTUREGCPOLICY_H_

// Pure, dependency-free policy of the texture garbage collector: which idle image may be freed,
// and the walk that spends the per-run deletion budget. Kept out of the Vulkan code so it can be
// unit-tested (tests/TextureGcPolicyTests.cpp).
//
// Two rules, both about images the collector must NOT free:
//  * An image the walk keeps (tiled and downloadable, depth, ...) does not use up the deletion
//    budget. The least recently used images are the ones nobody touches again, so a handful of
//    kept images at the front of the list would otherwise starve the collector for ever
//    (Ported from chenxiao07/KytyPS5@45beec32a; the walk, not the thresholds).
//  * An image the GPU wrote whose contents are not provably also in guest memory is the only
//    valid copy of that data. It is freed only when the guest memory copy is known to be newer
//    (a CPU or buffer write after the GPU write), or under critical memory pressure.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::TextureGcPolicy {

struct Candidate {
	bool registered       = true;
	bool has_depth_owner  = false; // a stencil association: freed together with its depth image
	bool gpu_modified     = false;
	bool tiled            = false;
	bool buffer_modified  = false; // a buffer shader wrote the guest range after the GPU write
	bool cpu_dirty        = false; // the CPU definitely wrote the guest range after the GPU write
	bool maybe_cpu_dirty  = false; // the CPU wrote a page that overlaps the range (unresolved)
};

enum class Action {
	Keep,             // leave the image alone, no budget used
	Free,             // free it; guest memory already holds everything that matters
	DownloadThenFree, // copy the GPU contents to guest memory first (may fail: then it is kept)
};

[[nodiscard]] constexpr bool SafeToDownload(const Candidate& c) {
	return c.gpu_modified && !c.buffer_modified && !c.cpu_dirty && !c.maybe_cpu_dirty;
}

// `pressured`: used memory is above the pressure threshold. `critical`: above the critical one.
[[nodiscard]] constexpr Action Classify(const Candidate& c, bool pressured, bool critical) {
	if (!c.registered || c.has_depth_owner) {
		return Action::Keep;
	}
	if (!c.gpu_modified) {
		return Action::Free;
	}
	if (SafeToDownload(c)) {
		if (c.tiled || !pressured) {
			return Action::Keep;
		}
		return Action::DownloadThenFree;
	}
	if (c.buffer_modified || c.cpu_dirty) {
		return Action::Free; // guest memory holds newer data than the image
	}
	// Only "maybe dirty": the GPU contents may be the only valid copy.
	return critical ? Action::Free : Action::Keep;
}

// Walks `candidates` in order while budget remains. Each non-Keep candidate reserves one unit of
// the budget before `execute(id, action)` runs; if execute returns false (a download failed, the
// image vanished) the unit is returned. `execute` may lower `budget` (the collector halves it
// when memory drops below a threshold). Returns the number of images freed.
template<class Id, class ClassifyFn, class ExecuteFn>
size_t Walk(const std::vector<Id>& candidates, size_t& budget, ClassifyFn&& classify,
            ExecuteFn&& execute) {
	size_t freed = 0;
	for (const auto& id: candidates) {
		if (budget == 0) {
			break;
		}
		const Action action = classify(id);
		if (action == Action::Keep) {
			continue;
		}
		--budget;
		if (execute(id, action)) {
			++freed;
		} else {
			++budget;
		}
	}
	return freed;
}

} // namespace Libs::Graphics::TextureGcPolicy

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTUREGCPOLICY_H_ */
