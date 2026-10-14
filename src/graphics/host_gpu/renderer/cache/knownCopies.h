#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNCOPIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNCOPIES_H_

// --known-copies: a compute dispatch that copies records from one buffer to another leaves the
// destination holding what the source held, so when the CPU can read the source without waiting for
// the GPU (nothing GPU-written in it) it knows the destination too. The titles that fast-clear color
// targets copy the color-metadata keys (DCC/CMask) of one surface to another with such a shader; the
// emulator reads the destination back to find the clear colour (a full GPU drain). Pure logic,
// unit tested in tests/KnownCopiesTests.cpp.
//
// ResolveKnownCopy decides whether a dispatch of a ShaderRecompiler::IR::UniformCopy shader is an exact
// dword copy: both descriptors have the same 32-bit unsigned integer format with 1..4 components (a
// bit-exact transfer through a formatted load and store), stride 4 * components, identity component
// selects, no swizzle, no index stride, no add-tid; the launch is a plain one-dimensional dispatch of
// whole groups whose invocation count does not exceed either buffer's record count (an invocation
// past the end of the source would load zeros, past the end of the destination its store would be
// dropped); the two byte ranges are disjoint.

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct CopyDescriptor {
	uint64_t base        = 0;
	uint32_t num_records = 0;
	uint32_t stride      = 0;
	uint32_t words       = 0; // components of its 32-bit unsigned integer format; 0: another format
	uint32_t dst_sel     = 0; // DstSelXYZW
	bool     swizzle     = false;
	bool     add_tid     = false;
	uint32_t index_stride = 0;
	bool     mapped      = false; // the whole extent is mapped guest memory
};

struct CopyLaunch {
	uint32_t groups[3]       = {0, 0, 0};
	uint32_t threads[3]      = {0, 0, 0}; // per group
	bool     thread_dimensions = false;  // the dispatch counts threads, not groups
	bool     use_plain_mode  = false;    // mode is the plain compute dispatch initiator (0x41)
};

struct KnownCopy {
	uint64_t dst   = 0;
	uint64_t src   = 0;
	uint64_t bytes = 0;
};

inline constexpr uint32_t kIdentityDstSel = 4u | (5u << 3) | (6u << 6) | (7u << 9);

[[nodiscard]] inline bool ResolveKnownCopy(const ShaderRecompiler::IR::UniformCopy& copy,
                                           const CopyDescriptor& src, const CopyDescriptor& dst,
                                           const CopyLaunch& launch, KnownCopy& out) {
	if (!copy.valid || src.words == 0 || src.words != dst.words || src.words > 4 ||
	    src.stride != 4 * src.words || dst.stride != 4 * dst.words || src.base == 0 || dst.base == 0 ||
	    (src.base & 3u) != 0 || (dst.base & 3u) != 0 || src.swizzle || dst.swizzle || src.add_tid ||
	    dst.add_tid || src.index_stride != 0 || dst.index_stride != 0 || src.dst_sel != kIdentityDstSel ||
	    dst.dst_sel != kIdentityDstSel || !src.mapped || !dst.mapped) {
		return false;
	}
	if (launch.thread_dimensions || !launch.use_plain_mode || copy.group_stride == 0 ||
	    launch.threads[0] != copy.group_stride || launch.threads[1] != 1 || launch.threads[2] != 1 ||
	    launch.groups[0] == 0 || launch.groups[1] != 1 || launch.groups[2] != 1) {
		return false;
	}
	const uint64_t invocations = static_cast<uint64_t>(launch.groups[0]) * launch.threads[0];
	if (invocations > src.num_records || invocations > dst.num_records) {
		return false;
	}
	const uint64_t bytes = invocations * dst.stride;
	if (src.base < dst.base + bytes && dst.base < src.base + bytes) {
		return false; // overlapping ranges: the result depends on the order the lanes run in
	}
	out = {dst.base, src.base, bytes};
	return true;
}

struct DwordRun {
	uint64_t offset = 0; // bytes from the start
	uint64_t bytes  = 0;
	uint32_t value  = 0;
};

// The maximal runs of equal dwords of `dwords`, or false when there are more than `max_runs` (a
// destination that is not mostly uniform is not worth tracking dword by dword).
[[nodiscard]] inline bool CollectRuns(std::span<const uint32_t> dwords, size_t max_runs, std::vector<DwordRun>& runs) {
	runs.clear();
	for (size_t i = 0; i < dwords.size();) {
		size_t j = i + 1;
		while (j < dwords.size() && dwords[j] == dwords[i]) {
			++j;
		}
		if (runs.size() >= max_runs) {
			return false;
		}
		runs.push_back({static_cast<uint64_t>(i) * 4, static_cast<uint64_t>(j - i) * 4, dwords[i]});
		i = j;
	}
	return !runs.empty();
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_KNOWNCOPIES_H_
