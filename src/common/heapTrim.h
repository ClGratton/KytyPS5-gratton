#ifndef EMULATOR_COMMON_HEAPTRIM_H_
#define EMULATOR_COMMON_HEAPTRIM_H_

#include <cstdint>

// Policy of --heap-trim: when the host allocator (glibc malloc) holds a lot of freed memory in its
// arenas, hand the free pages back to the kernel. Free memory in an arena is not returned by free()
// when it sits below other live chunks (fragmentation after shader and pipeline compilation, a
// burst of short-lived buffers); malloc_trim() madvises those free pages away.
// Pure, so it can be unit-tested (tests/HeapTrimTests.cpp).
namespace Common::HeapTrim {

constexpr uint64_t MiB = 1024ull * 1024;

struct HeapState {
	uint64_t arena_bytes = 0; // bytes the arenas obtained from the system (non-mmapped)
	uint64_t free_bytes  = 0; // free chunks inside them (mallinfo2 fordblks)
};

struct Policy {
	// Trim only when this much is free, and it is at least this share of the arenas: a small heap
	// with a few free chunks is not worth a pass over every arena.
	uint64_t min_free_bytes = 64 * MiB;
	uint32_t min_free_percent = 20;
	// At most one trim per this long, however large the free space stays.
	uint64_t min_interval_ms = 5000;
};

[[nodiscard]] inline bool ShouldTrim(const HeapState& heap, const Policy& policy,
                                     uint64_t ms_since_last_trim) {
	if (ms_since_last_trim < policy.min_interval_ms) {
		return false;
	}
	if (heap.free_bytes < policy.min_free_bytes || heap.arena_bytes == 0) {
		return false;
	}
	return heap.free_bytes * 100 >= heap.arena_bytes * policy.min_free_percent;
}

} // namespace Common::HeapTrim

#endif /* EMULATOR_COMMON_HEAPTRIM_H_ */
