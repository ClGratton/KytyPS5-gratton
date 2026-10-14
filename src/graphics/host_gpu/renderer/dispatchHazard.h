#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHHAZARD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHHAZARD_H_

// Pure rule for --overlap-dispatches: what a compute dispatch reads and writes, and whether two
// dispatches can run without a barrier between them. Unit tested in tests/DispatchHazardTests.cpp.
//
// Every direct dispatch was followed by a compute -> all-commands barrier (and a writing one was
// preceded by an all-commands -> compute barrier), so consecutive dispatches never overlapped on the
// one queue, although the guest's own GPU runs them concurrently unless the guest orders them.
// Three quarters of the level frame's dispatches are followed by another dispatch. With the flag, a
// dispatch whose accesses do not conflict with those of the dispatches still waiting for their
// barrier joins the group: the barrier is recorded once, before the next command that is not such a
// dispatch (CommandBuffer::Handle). A dispatch whose accesses are not fully known (addresses, GDS)
// is treated as touching everything, so it never overlaps a writer, and falls back to the full
// barrier.
//
// Ported from shadowbeat070/KytyPS5@fdfd266e5 (GPL-2.0): the DispatchAccess range sets and the
// conflict rule. The collection of what a dispatch touches and the command buffer part are written
// for this tree.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct DispatchAccess {
	enum class Space : uint8_t {
		Guest,      // guest virtual addresses
		HostBuffer, // byte ranges of one host VkBuffer, keyed by its handle
		HostImage,  // one host VkImage as a whole, keyed by its handle
	};

	struct Range {
		uint64_t key   = 0;
		uint64_t begin = 0;
		uint64_t end   = 0;
		Space    space = Space::Guest;
		bool     write = false;
	};

	std::vector<Range> ranges;
	bool               reads_all  = false; // may read anything (addresses)
	bool               writes_all = false; // may write anything

	void Clear() {
		ranges.clear();
		reads_all  = false;
		writes_all = false;
	}

	void Add(Space space, uint64_t key, uint64_t begin, uint64_t size, bool write) {
		if (size == 0) {
			return;
		}
		const uint64_t end = size > UINT64_MAX - begin ? UINT64_MAX : begin + size;
		ranges.push_back({key, begin, end, space, write});
	}

	[[nodiscard]] bool Empty() const { return ranges.empty() && !reads_all && !writes_all; }

	[[nodiscard]] bool HasWrites() const {
		return writes_all ||
		       std::any_of(ranges.begin(), ranges.end(), [](const Range& r) { return r.write; });
	}

	// True when running the two without a barrier between them could be observed: one may write
	// what the other reads or writes.
	[[nodiscard]] bool ConflictsWith(const DispatchAccess& other) const {
		if ((writes_all && !other.Empty()) || (other.writes_all && !Empty())) {
			return true;
		}
		if ((reads_all && other.HasWrites()) || (other.reads_all && HasWrites())) {
			return true;
		}
		for (const auto& a: ranges) {
			for (const auto& b: other.ranges) {
				if ((a.write || b.write) && a.space == b.space && a.key == b.key &&
				    a.begin < b.end && b.begin < a.end) {
					return true;
				}
			}
		}
		return false;
	}

	// The accesses of a group: a repeated range is kept once, writes win.
	void Merge(const DispatchAccess& other) {
		for (const auto& range: other.ranges) {
			const auto same = std::find_if(ranges.begin(), ranges.end(), [&](const Range& r) {
				return r.space == range.space && r.key == range.key && r.begin == range.begin &&
				       r.end == range.end;
			});
			if (same == ranges.end()) {
				ranges.push_back(range);
			} else {
				same->write = same->write || range.write;
			}
		}
		reads_all  = reads_all || other.reads_all;
		writes_all = writes_all || other.writes_all;
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHHAZARD_H_
