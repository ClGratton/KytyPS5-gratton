#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONCOUNTERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONCOUNTERS_H_

// The guest side of ZPASS_DONE occlusion counters, without any Vulkan: the dump layout and the
// bookkeeping that turns host occlusion queries into the counters a guest reads.
//
// Adapted from TheCruZ/KytyPS5-GTA 7a0e8c55b (upstream KytyPS5 PR #1259, GPL-2.0), the Segments
// design and the dump layout; the host side here is written for this tree's renderer.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::OcclusionCounters {

constexpr uint32_t DbCount  = 16;
constexpr uint64_t DbStride = 16;
// Bytes a dump may write.
constexpr uint64_t DumpSize  = (DbCount - 1u) * DbStride + sizeof(uint64_t);
constexpr uint64_t ReadyBit  = uint64_t {1} << 63u;
constexpr uint64_t ValueMask = ReadyBit - 1u;
// Added to a segment whose samples could not all be counted, so its queries read visible.
constexpr uint64_t UncountedSamples = uint64_t {1} << 24u;

// The value a dump writes for one DB: the whole count lives in DB 0, the others stay at zero.
[[nodiscard]] constexpr uint64_t DbValue(uint32_t db, uint64_t total) {
	return ReadyBit | (db == 0 ? (total & ValueMask) : 0u);
}

// Splits the stream of draws into segments at every dump. The samples of a segment are counted
// by host queries only while a guest query is open (a begin dump at A that has not seen its end
// dump at A + 8); the value published for a dump is the running total of every earlier segment,
// so any begin/end pairing (also nested or interleaved queries) reads the samples between its
// dumps.
class Segments {
public:
	struct Dump {
		uint64_t address    = 0;
		uint64_t first_slot = 0; // host query slots of the segment the dump ends, [first, end)
		uint64_t end_slot   = 0;
		bool     uncounted  = false; // some samples of the segment had no host query
	};

	// `capacity`: slots of the host query pool; `max_open_frames`: frames after which a begin
	// dump that never saw its end stops keeping the counters running.
	explicit Segments(uint32_t capacity, uint32_t max_open_frames = 8)
	    : m_capacity(capacity), m_max_open_frames(max_open_frames) {}

	[[nodiscard]] Dump OnDump(uint64_t address) {
		Dump dump {.address    = address,
		           .first_slot = m_segment_first,
		           .end_slot   = m_next_slot,
		           .uncounted  = m_uncounted};
		m_segment_first  = m_next_slot;
		m_uncounted      = false;
		const auto begin = std::find_if(m_open.begin(), m_open.end(), [&](const Open& open) {
			return open.address + sizeof(uint64_t) == address;
		});
		if (begin != m_open.end()) {
			m_open.erase(begin);
		} else {
			m_open.push_back({address, m_frame});
		}
		return dump;
	}

	// Whether the samples of draws are needed now.
	[[nodiscard]] bool Counting() const noexcept { return !m_open.empty(); }

	// A slot for a host query of the current segment; false when every slot waits for its
	// results, and then the segment reads visible.
	[[nodiscard]] bool AllocateSlot(uint32_t& slot) {
		if (m_next_slot - m_released >= m_capacity) {
			m_uncounted = true;
			return false;
		}
		slot = static_cast<uint32_t>(m_next_slot % m_capacity);
		m_next_slot++;
		return true;
	}

	// Slots before `end_slot` are free again.
	void Release(uint64_t end_slot) { m_released = std::max(m_released, end_slot); }

	void OnFrame() {
		m_frame++;
		std::erase_if(
		    m_open, [this](const Open& open) { return open.frame + m_max_open_frames < m_frame; });
	}

	[[nodiscard]] uint32_t Capacity() const noexcept { return m_capacity; }
	[[nodiscard]] size_t   OpenQueries() const noexcept { return m_open.size(); }

private:
	struct Open {
		uint64_t address = 0;
		uint64_t frame   = 0;
	};

	uint32_t          m_capacity        = 0;
	uint32_t          m_max_open_frames = 0;
	uint64_t          m_next_slot       = 0;
	uint64_t          m_released        = 0;
	uint64_t          m_segment_first   = 0;
	uint64_t          m_frame           = 0;
	bool              m_uncounted       = false;
	std::vector<Open> m_open;
};

} // namespace Libs::Graphics::OcclusionCounters

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONCOUNTERS_H_
