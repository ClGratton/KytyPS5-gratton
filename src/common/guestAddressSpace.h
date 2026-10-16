#ifndef EMULATOR_SRC_COMMON_GUESTADDRESSSPACE_H_
#define EMULATOR_SRC_COMMON_GUESTADDRESSSPACE_H_

#include <algorithm>
#include <cstdint>
#include <span>

namespace Common {

/// The guest address space starts here: nothing below it is ever mapped, on any host. A pointer
/// into [0, kGuestAddressSpaceStart) is a null pointer plus a small offset, not a place in memory.
inline constexpr uint64_t kGuestAddressSpaceStart = 0x40000;

/// Whether [address, address + size) lies wholly below the start of the guest address space.
[[nodiscard]] constexpr bool IsGuestNullRange(uint64_t address, uint64_t size) {
	return address < kGuestAddressSpaceStart && size <= kGuestAddressSpaceStart - address;
}

/**
 * @brief The value of a table read through a null pointer: zeros.
 *
 * The resource walk evaluates the loads of a shader that the shader may never execute, and a draw
 * the game leaves with an unset table pointer (during level loads) makes it load from an address
 * like 0x18. The GPU reads zeros from memory that is not mapped, and a zeroed descriptor already
 * means "no resource"; the host would fault on a direct read. A read that lies wholly inside the
 * null page is answered with zeros and never reaches memory.
 *
 * @return true when the read was answered (the values are zero).
 */
inline bool ReadGuestNullPage(uint64_t address, std::span<uint32_t> values) {
	if (!IsGuestNullRange(address, values.size_bytes())) {
		return false;
	}
	std::ranges::fill(values, 0u);
	return true;
}

} // namespace Common

#endif // EMULATOR_SRC_COMMON_GUESTADDRESSSPACE_H_
