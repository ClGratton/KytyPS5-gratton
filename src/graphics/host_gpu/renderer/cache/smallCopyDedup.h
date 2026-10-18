#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace Libs::Graphics {

// --small-copy-dedup. BufferCache::ObtainBuffer copies a small CPU-written constant range into the stream ring on every
// request. 70 % of those copies hold the bytes the same guest address held in the draw before, so the second copy is the
// same bytes at a new ring offset. This keeps the bytes of recent copies and answers "the ring already holds exactly
// these bytes at offset X" so the caller can use X again.
//
// Exact by construction:
//  * a hit needs the same guest address and size, and a byte-for-byte equal range (memcmp against the bytes the ring was
//    written from; never a hash);
//  * the ring is written from the kept bytes, not from guest memory, so the kept bytes and the ring cannot differ;
//  * the copy must have been made in the same scheduler tick (the ring's reuse fence is one watch per tick: a copy made
//    in an earlier tick is only protected until that tick finishes, not until a later command buffer that reads it does)
//    and in the same ring lap (a wrap may have overwritten it).
//
// Pure data, no Vulkan types. GPU thread only; 4-way set associative on (address, size).
class SmallCopyDedup {
public:
	struct Stats {
		uint64_t lookups     = 0;
		uint64_t hits        = 0;
		uint64_t stores      = 0;
		uint64_t bytes_saved = 0;
		// Why a lookup missed: no entry for the (address, size); an entry of an earlier tick or lap; an entry of this tick with other bytes.
		uint64_t miss_absent  = 0;
		uint64_t miss_stale   = 0;
		uint64_t miss_content = 0;
	};

	// The ring offset that holds exactly `size` bytes equal to `data`, copied from `address` in this tick and lap.
	[[nodiscard]] std::optional<uint64_t> Find(uint64_t address, const void* data, uint32_t size, uint64_t tick,
	                                           uint64_t lap) {
		m_stats.lookups++;
		const auto* set   = &m_slots[SetIndex(address, size) * kWays];
		bool        known = false;
		bool        stale = false;
		for (size_t way = 0; way < kWays; ++way) {
			const auto& slot = set[way];
			if (!slot.valid || slot.address != address || slot.size != size) {
				continue;
			}
			known = true;
			if (slot.tick != tick || slot.lap != lap) {
				stale = true;
				continue;
			}
			if (std::memcmp(slot.bytes.data(), data, size) != 0) {
				continue;
			}
			m_stats.hits++;
			m_stats.bytes_saved += size;
			return slot.offset;
		}
		(known ? (stale ? m_stats.miss_stale : m_stats.miss_content) : m_stats.miss_absent)++;
		return std::nullopt;
	}

	// Takes the bytes of a copy about to be made and returns the kept copy: the caller writes the ring from the returned
	// pointer, then calls Commit with the ring offset. The slot holds nothing until then.
	[[nodiscard]] const uint8_t* Stage(uint64_t address, const void* data, uint32_t size) {
		auto* set    = &m_slots[SetIndex(address, size) * kWays];
		auto* victim = &set[0];
		for (size_t way = 0; way < kWays; ++way) {
			auto& slot = set[way];
			if (slot.valid && slot.address == address && slot.size == size) {
				victim = &slot; // the same range again: replace its entry
				break;
			}
			if (!slot.valid) {
				victim = &slot;
			} else if (victim->valid && (slot.lap < victim->lap || (slot.lap == victim->lap && slot.tick < victim->tick))) {
				victim = &slot; // otherwise the oldest entry
			}
		}
		victim->valid = false;
		victim->bytes.resize(size);
		std::memcpy(victim->bytes.data(), data, size);
		m_staged = victim;
		return victim->bytes.data();
	}

	void Commit(uint64_t address, uint32_t size, uint64_t offset, uint64_t tick, uint64_t lap) {
		auto& slot = *m_staged;
		slot.address = address;
		slot.size    = size;
		slot.offset  = offset;
		slot.tick    = tick;
		slot.lap     = lap;
		slot.valid   = true;
		m_stats.stores++;
	}

	void Clear() noexcept {
		for (auto& slot: m_slots) slot.valid = false;
	}

	[[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }

	// The set a range maps to (for tests).
	[[nodiscard]] static size_t SetOf(uint64_t address, uint32_t size) noexcept { return SetIndex(address, size); }

private:
	static constexpr size_t kSetBits = 10;
	static constexpr size_t kWays    = 4;
	static constexpr size_t kSets    = size_t {1} << kSetBits;

	[[nodiscard]] static size_t SetIndex(uint64_t address, uint32_t size) noexcept {
		return static_cast<size_t>(((address >> 2) ^ (static_cast<uint64_t>(size) << 17)) * 0x9e3779b97f4a7c15ull >>
		                           (64 - kSetBits));
	}

	struct Slot {
		uint64_t             address = 0;
		uint64_t             offset  = 0;
		uint64_t             tick    = 0;
		uint64_t             lap     = 0;
		uint32_t             size    = 0;
		bool                 valid   = false;
		std::vector<uint8_t> bytes;
	};

	std::array<Slot, kSets * kWays> m_slots;
	Slot*                           m_staged = nullptr;
	Stats                           m_stats;
};

} // namespace Libs::Graphics
