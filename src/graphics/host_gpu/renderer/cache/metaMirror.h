#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METAMIRROR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METAMIRROR_H_

// --lockfree-image-checks: a copy of the texture cache's surface-metadata table (address -> clear mask) that any thread can read
// without the cache lock.
//
// TextureCache::m_surface_metas is a std::map under the cache lock. The GPU thread asks it once per depth draw (IsMetaCleared),
// and every ask took the lock and walked a red-black tree. The map stays the owner of the data; this open-addressed table holds
// the same (address, mask) pairs, is changed only together with the map (one writer at a time, under the cache lock), and is read
// with plain loads.
//
// Consistency: an insert or an erase (which moves entries) is bracketed by a sequence word that is odd while it runs; a reader
// that sees the word odd or changed while it probed starts again (a few times, then answers Unknown and the caller takes the
// lock). A mask update of an entry already there is a single atomic store and needs no bracket. So an answer is the table as it
// was at one instant, which is what a thread that took the lock just before or just after a concurrent change would see.
// Unit tested in tests/MetaMirrorTests.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Libs::Graphics {

class MetaMirror {
public:
	static constexpr size_t kCapacity   = 4096; // slots, a power of two
	static constexpr size_t kMaxEntries = kCapacity / 2;

	MetaMirror(): m_slots(new Slot[kCapacity]) {}
	MetaMirror(const MetaMirror&)            = delete;
	MetaMirror& operator=(const MetaMirror&) = delete;

	// ---- Writer (one thread at a time) ----

	// Adds `address` with `mask`, or sets the mask when it is there. False when the table is full or the address cannot be a key;
	// the entry is then missing and Complete() turns false for good (readers use the map).
	bool Set(uint64_t address, uint32_t mask) {
		if (address == kEmpty) {
			m_complete.store(false, std::memory_order_release);
			return false;
		}
		for (size_t index = Home(address);; index = Next(index)) {
			Slot&          slot = m_slots[index];
			const uint64_t key  = slot.key.load(std::memory_order_relaxed);
			if (key == address) {
				slot.mask.store(mask, std::memory_order_relaxed);
				return true;
			}
			if (key != kEmpty) {
				continue;
			}
			if (m_used >= kMaxEntries) {
				m_complete.store(false, std::memory_order_release);
				return false;
			}
			BeginChange();
			slot.mask.store(mask, std::memory_order_relaxed);
			slot.key.store(address, std::memory_order_relaxed);
			m_used++;
			EndChange();
			return true;
		}
	}

	// Removes `address` when it is there (the entries after it in its probe run move up to close the gap).
	void Erase(uint64_t address) {
		if (address == kEmpty) {
			return;
		}
		size_t hole = Home(address);
		while (true) {
			const uint64_t key = m_slots[hole].key.load(std::memory_order_relaxed);
			if (key == address) {
				break;
			}
			if (key == kEmpty) {
				return;
			}
			hole = Next(hole);
		}
		BeginChange();
		size_t next = hole;
		while (true) {
			next                = Next(next);
			const uint64_t key  = m_slots[next].key.load(std::memory_order_relaxed);
			if (key == kEmpty) {
				break;
			}
			// The entry at `next` may move into the hole only if its home is not in the cyclic range (hole, next].
			const size_t home    = Home(key);
			const bool   between = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
			if (between) {
				continue;
			}
			m_slots[hole].mask.store(m_slots[next].mask.load(std::memory_order_relaxed), std::memory_order_relaxed);
			m_slots[hole].key.store(key, std::memory_order_relaxed);
			hole = next;
		}
		m_slots[hole].key.store(kEmpty, std::memory_order_relaxed);
		m_used--;
		EndChange();
	}

	// False once an entry could not be stored: the table lacks entries the map has, and a reader must not trust "absent".
	[[nodiscard]] bool   Complete() const { return m_complete.load(std::memory_order_acquire); }
	[[nodiscard]] size_t Used() const { return m_used; } // writer's view

	// ---- Reader (any thread) ----

	enum class Found : uint8_t { Absent, Present, Unknown };

	// Present: `address` has `*mask`. Absent: it has no entry. Unknown: the table is incomplete or kept changing; use the map
	// under the lock.
	[[nodiscard]] Found Lookup(uint64_t address, uint32_t* mask) const {
		if (address == kEmpty || !Complete()) {
			return Found::Unknown;
		}
		for (int attempt = 0; attempt < kRetries; attempt++) {
			const uint32_t before = m_sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0) {
				continue;
			}
			Found    result = Found::Unknown;
			uint32_t value  = 0;
			size_t   index  = Home(address);
			for (size_t probes = 0; probes < kCapacity; probes++, index = Next(index)) {
				const uint64_t key = m_slots[index].key.load(std::memory_order_relaxed);
				if (key == address) {
					value  = m_slots[index].mask.load(std::memory_order_relaxed);
					result = Found::Present;
					break;
				}
				if (key == kEmpty) {
					result = Found::Absent;
					break;
				}
			}
			std::atomic_thread_fence(std::memory_order_acquire);
			if (m_sequence.load(std::memory_order_relaxed) != before) {
				continue;
			}
			if (result == Found::Present) {
				*mask = value;
			}
			return result;
		}
		return Found::Unknown;
	}

	// Every (address, mask) pair, in table order (tests, the verifier). Writer side: the caller holds the cache lock.
	template <typename Func>
	void ForEach(Func&& func) const {
		for (size_t i = 0; i < kCapacity; i++) {
			const uint64_t key = m_slots[i].key.load(std::memory_order_relaxed);
			if (key != kEmpty) {
				func(key, m_slots[i].mask.load(std::memory_order_relaxed));
			}
		}
	}

private:
	static constexpr uint64_t kEmpty   = 0;
	static constexpr int      kRetries = 16;

	struct Slot {
		std::atomic<uint64_t> key {kEmpty};
		std::atomic<uint32_t> mask {0};
	};

	[[nodiscard]] static size_t Home(uint64_t address) {
		return static_cast<size_t>(((address >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & (kCapacity - 1);
	}
	[[nodiscard]] static size_t Next(size_t index) { return (index + 1) & (kCapacity - 1); }

	void BeginChange() {
		m_sequence.store(m_sequence.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
	}
	void EndChange() { m_sequence.store(m_sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

	std::unique_ptr<Slot[]> m_slots;
	std::atomic<uint32_t>   m_sequence {0}; // odd while an insert or an erase runs
	size_t                  m_used = 0;     // writer only
	std::atomic<bool>       m_complete {true};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_METAMIRROR_H_
