#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PENDINGWRITES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PENDINGWRITES_H_

// Pure rule for --defer-draw-write-barriers: the buffer ranges that draws wrote while the barrier
// that makes those writes visible is still pending, and when a later draw has to wait for it.
// Unit tested in tests/PendingWritesTests.cpp; the render executor owns one instance.
//
// A draw whose shaders write buffers (a vertex shader's per-draw feedback, counters, atomic
// maxima) ended the rendering instance and recorded a shader-write barrier right after itself.
// With thousands of such draws a frame, each one cut the render pass. The barrier is only needed
// by something that reads the written bytes later, and everything that is not a draw already ends
// rendering before it records (dynamic rendering allows no copy, dispatch, layout transition or
// barrier inside an instance), which records the pending barrier. So the barrier is left pending
// while draws continue in the same instance, and a later draw ends the instance first (which
// records the barrier) when it may touch the written bytes:
//  * a buffer binding, vertex, index or argument range that overlaps a pending write;
//  * any access through addresses (BDA): unknown ranges, always a conflict.
// The one exception is two accesses that are both atomics whose order does not matter (see
// BufferResource::order_free_atomics): they are coherent without a barrier and their result does
// not depend on the order.
//
// Ranges are guest virtual addresses (buffer sources are guest ranges, one host buffer per range
// of guest memory), so two views of the same bytes overlap here as they do in memory.
//
// Ported from BryanKAdams/KytyPS5@08c3b6c0b (GPL-2.0): the pending set and the overlap rule.
// Re-implemented as a testable class; the order-free atomics condition is stricter than the
// original (it also requires an unused atomic result and a commutative operation).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

class PendingBufferWrites {
public:
	// More pending entries than this end the instance (bounds the per-draw overlap checks).
	static constexpr size_t MaxEntries = 64;

	[[nodiscard]] bool Empty() const noexcept { return m_writes.empty(); }
	[[nodiscard]] size_t Size() const noexcept { return m_writes.size(); }
	[[nodiscard]] bool Full() const noexcept { return m_writes.size() > MaxEntries; }

	void Clear() { m_writes.clear(); }

	// A draw wrote [begin, begin + size). `order_free` is true when it only touched the range with
	// order-free atomics. Empty ranges are ignored.
	void Add(uint64_t begin, uint64_t size, bool order_free) {
		if (size == 0) {
			return;
		}
		const uint64_t end = Saturating(begin, size);
		for (auto& write: m_writes) {
			if (write.begin == begin && write.end == end) {
				write.order_free = write.order_free && order_free;
				return;
			}
		}
		m_writes.push_back({begin, end, order_free});
	}

	// Whether an access to [begin, begin + size) must wait for the pending barrier. `order_free` is
	// true when the access is only order-free atomics.
	[[nodiscard]] bool Conflicts(uint64_t begin, uint64_t size, bool order_free) const {
		if (size == 0) {
			return false;
		}
		const uint64_t end = Saturating(begin, size);
		return std::any_of(m_writes.begin(), m_writes.end(), [&](const Write& write) {
			return begin < write.end && write.begin < end && !(order_free && write.order_free);
		});
	}

private:
	struct Write {
		uint64_t begin      = 0;
		uint64_t end        = 0;
		bool     order_free = false;
	};

	static uint64_t Saturating(uint64_t begin, uint64_t size) {
		return size > UINT64_MAX - begin ? UINT64_MAX : begin + size;
	}

	std::vector<Write> m_writes;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PENDINGWRITES_H_
