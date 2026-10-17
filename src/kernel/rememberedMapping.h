#ifndef EMULATOR_SRC_KERNEL_REMEMBEREDMAPPING_H_
#define EMULATOR_SRC_KERNEL_REMEMBEREDMAPPING_H_

// A thread's small cache of guest mapping entries for --backing-read-cache (kernel/memoryAddressSpace.inc).
//
// A guest read of the backing store has to find the mapping entry that holds the address (a std::map walk under
// the map's shared lock). An entry is [lo, hi) of guest addresses whose backing offset is address + delta, valid
// while the map's epoch (moved, under the unique lock, by every change of the map) is the one it was read at.
// Pure: no locks, no clocks; unit tested in tests/RememberedMappingTests.cpp.

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::LibKernel::Memory {

template <size_t Slots>
class RememberedMappings {
public:
	struct Entry {
		const void* owner = nullptr; // the store the entry came from
		uint64_t    epoch = 0;
		uint64_t    lo    = 0; // first guest address of the entry
		uint64_t    hi    = 0; // one past the last
		uint64_t    delta = 0; // backing offset = guest address + delta (modular)
	};

	// The entry of `owner` at `epoch` that holds all of [address, address + size), or nullptr.
	[[nodiscard]] const Entry* Find(const void* owner, uint64_t epoch, uint64_t address, uint64_t size) const {
		for (const auto& e: m_entries) {
			// address < hi first: hi - address wraps for an address above the entry
			if (e.owner == owner && e.epoch == epoch && address >= e.lo && address < e.hi &&
			    size <= e.hi - address) {
				return &e;
			}
		}
		return nullptr;
	}

	// Remembers the entry [lo, hi) of `owner` read at `epoch`; the oldest slot is replaced.
	void Remember(const void* owner, uint64_t epoch, uint64_t lo, uint64_t hi, uint64_t backing_offset) {
		m_entries[m_next] = {owner, epoch, lo, hi, backing_offset - lo};
		m_next            = (m_next + 1) % Slots;
	}

private:
	std::array<Entry, Slots> m_entries {};
	size_t                   m_next = 0;
};

} // namespace Libs::LibKernel::Memory

#endif // EMULATOR_SRC_KERNEL_REMEMBEREDMAPPING_H_
