#include "graphics/host_gpu/renderer/pipeline/nullPageRead.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::GuestNullPageEnd;
using Libs::Graphics::ReadGuestNullPage;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "NullPageReadTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	// The null page ends where the guest address space starts (0x40000, the first address the
	// guest memory manager hands out), the same boundary as the shader resource walk.
	Check(GuestNullPageEnd == 0x40000, "the null page must end at the start of the guest address space");
	std::array<uint32_t, 4> values {1, 2, 3, 4};
	Check(ReadGuestNullPage(0x10, values) && values == std::array<uint32_t, 4> {0, 0, 0, 0},
	      "a read through a null table pointer must read zeros");
	values = {1, 2, 3, 4};
	Check(ReadGuestNullPage(0, std::span<uint32_t>(values.data(), 1)) && values[0] == 0 &&
	          values[1] == 2,
	      "only the requested words are zeroed");
	values = {1, 2, 3, 4};
	Check(ReadGuestNullPage(0x20000, values) && values[3] == 0,
	      "a null base plus a large table offset is still below the guest address space");
	values = {1, 2, 3, 4};
	Check(!ReadGuestNullPage(GuestNullPageEnd, values) && values[0] == 1,
	      "the first mapped page must not read as the null page");
	Check(!ReadGuestNullPage(GuestNullPageEnd - 8, values) && values[0] == 1,
	      "a read crossing out of the null page must go to the real read");
	Check(ReadGuestNullPage(GuestNullPageEnd - 16, values),
	      "a read ending at the page end is inside it");
	Check(!ReadGuestNullPage(UINT64_MAX - 3, values), "no wraparound at the top of the address space");
	Check(Common::IsGuestNullRange(0, 0) && !Common::IsGuestNullRange(GuestNullPageEnd, 0),
	      "an empty range is null only below the start of the address space");
	std::puts("NullPageReadTests: all cases passed");
	return 0;
}
