#include "graphics/host_gpu/renderer/pipeline/nullPageRead.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::ReadGuestNullPage;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "NullPageReadTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	std::array<uint32_t, 4> values {1, 2, 3, 4};
	Check(ReadGuestNullPage(0x10, values) && values == std::array<uint32_t, 4> {0, 0, 0, 0},
	      "a read through a null table pointer must read zeros");
	values = {1, 2, 3, 4};
	Check(ReadGuestNullPage(0, std::span<uint32_t>(values.data(), 1)) && values[0] == 0 &&
	          values[1] == 2,
	      "only the requested words are zeroed");
	values = {1, 2, 3, 4};
	Check(!ReadGuestNullPage(0x10000, values) && values[0] == 1,
	      "the first mapped page must not read as the null page");
	Check(!ReadGuestNullPage(0x10000 - 8, values) && values[0] == 1,
	      "a read crossing out of the null page must go to the real read");
	Check(ReadGuestNullPage(0x10000 - 16, values), "a read ending at the page end is inside it");
	Check(!ReadGuestNullPage(UINT64_MAX - 3, values), "no wraparound at the top of the address space");
	std::puts("NullPageReadTests: all cases passed");
	return 0;
}
