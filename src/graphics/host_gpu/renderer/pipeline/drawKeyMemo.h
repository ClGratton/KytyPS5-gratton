#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWKEYMEMO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWKEYMEMO_H_

// --draw-key-cache: a small most-recently-used memo in front of a map that is keyed by a large
// value (the graphics pipeline key, about 700 bytes). Consecutive draws overwhelmingly use the same
// few pipelines, so comparing the key with the last few that hit is cheaper than hashing it
// (XXH3 over the static parameters plus a mix per binding and attribute), taking the map's shared
// lock and probing the bucket chain.
//
// The memo never decides anything the map would not: a hit needs `key == remembered key`, i.e.
// exactly the equality the map itself uses, and then returns the value the map returned for that
// key. It is exact as long as the values it holds stay valid for the lifetime of the memo (the
// pipeline cache never erases a Pipeline node while it lives; Clear() exists for the owner that
// does). A miss in the memo falls through to the real lookup, so a changed key field can only
// ever miss, never return a stale value. Verify mode (see PipelineCache) re-runs the real lookup
// on every hit and counts disagreements.
//
// Pure data structure, no locking: one owner thread (the GPU thread). Unit tested in
// tests/DrawKeyMemoTests.cpp.
//
// Idea from BryanKAdams/BryKytyPS5 c497660e5 ("find the draw's graphics pipeline with less work",
// GPL-2.0): remember the last pipeline and its key. Re-implemented as a separate pure class with
// more than one way and tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

template <class Key, class Value, size_t Ways = 4>
class DrawKeyMemo {
	static_assert(Ways >= 1 && Ways <= 16);

public:
	struct Stats {
		uint64_t hits   = 0;
		uint64_t misses = 0;
	};

	// The value remembered for a key equal to `key`, or nullptr. A hit makes the entry the first
	// one compared next time (only a small index array moves, never the keys).
	[[nodiscard]] Value* Find(const Key& key) {
		for (size_t rank = 0; rank < m_used; ++rank) {
			const uint8_t slot = m_order[rank];
			if (m_entries[slot].key == key) {
				for (size_t i = rank; i > 0; --i) {
					m_order[i] = m_order[i - 1];
				}
				m_order[0] = slot;
				++m_stats.hits;
				return &m_entries[slot].value;
			}
		}
		++m_stats.misses;
		return nullptr;
	}

	// Remembers `value` for `key` as the most recent entry; the least recent entry falls out when
	// all ways are in use. Call it after a map lookup (or insertion) that Find() missed.
	void Remember(const Key& key, Value value) {
		uint8_t slot = 0;
		size_t  last = 0;
		if (m_used < Ways) {
			slot = static_cast<uint8_t>(m_used);
			last = m_used;
			++m_used;
		} else {
			slot = m_order[Ways - 1];
			last = Ways - 1;
		}
		for (size_t i = last; i > 0; --i) {
			m_order[i] = m_order[i - 1];
		}
		m_order[0]           = slot;
		m_entries[slot].key   = key;
		m_entries[slot].value = std::move(value);
	}

	// Forgets everything (the owner destroyed or replaced values the memo may hold).
	void Clear() { m_used = 0; }

	[[nodiscard]] size_t       Size() const { return m_used; }
	[[nodiscard]] const Stats& GetStats() const { return m_stats; }

private:
	struct Entry {
		Key   key {};
		Value value {};
	};

	std::array<Entry, Ways>   m_entries {};
	std::array<uint8_t, Ways> m_order {};
	size_t                    m_used = 0;
	Stats                     m_stats;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRAWKEYMEMO_H_ */
