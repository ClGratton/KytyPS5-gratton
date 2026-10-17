#include "kernel/rememberedMapping.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::LibKernel::Memory::RememberedMappings;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RememberedMappingTests: failed: %s\n", message);
		std::abort();
	}
}

void TestHitInsideAnEntry() {
	RememberedMappings<4> cache;
	int                   a = 0;
	cache.Remember(&a, 7, 0x10000, 0x20000, 0x500000);
	const auto* e = cache.Find(&a, 7, 0x10000, 0x10000);
	Check(e != nullptr, "the whole entry is a hit");
	Check(0x10000 + e->delta == 0x500000, "the backing offset of the entry start");
	e = cache.Find(&a, 7, 0x18000, 8);
	Check(e != nullptr && 0x18000 + e->delta == 0x508000, "an address inside the entry maps linearly");
	Check(cache.Find(&a, 7, 0x1fffc, 4) != nullptr, "the last dword is a hit");
}

void TestMissOutsideOrAcrossTheEntry() {
	RememberedMappings<4> cache;
	int                   a = 0;
	cache.Remember(&a, 1, 0x10000, 0x20000, 0x100);
	Check(cache.Find(&a, 1, 0xfffc, 4) == nullptr, "below the entry");
	Check(cache.Find(&a, 1, 0x1fffd, 4) == nullptr, "a read that crosses the end");
	Check(cache.Find(&a, 1, 0x20000, 4) == nullptr, "at the end");
	Check(cache.Find(&a, 1, 0x30000, 4) == nullptr,
	      "far above the entry (hi - address wraps: it must not be a hit)");
	Check(cache.Find(&a, 1, UINT64_MAX - 3, 4) == nullptr, "the top of the address space");
	Check(cache.Find(&a, 1, 0x10000, 0x10001) == nullptr, "longer than the entry");
}

void TestEpochAndOwner() {
	RememberedMappings<4> cache;
	int                   a = 0, b = 0;
	cache.Remember(&a, 5, 0x10000, 0x20000, 0);
	Check(cache.Find(&a, 6, 0x10000, 4) == nullptr, "a changed map invalidates the entry");
	Check(cache.Find(&b, 5, 0x10000, 4) == nullptr, "another store's entry is not used");
	Check(cache.Find(&a, 5, 0x10000, 4) != nullptr, "the same epoch and store hit");
}

void TestReplacementOrder() {
	RememberedMappings<2> cache;
	int                   a = 0;
	cache.Remember(&a, 1, 0x1000, 0x2000, 0);
	cache.Remember(&a, 1, 0x3000, 0x4000, 0);
	cache.Remember(&a, 1, 0x5000, 0x6000, 0); // replaces the oldest
	Check(cache.Find(&a, 1, 0x1000, 4) == nullptr, "the oldest entry was replaced");
	Check(cache.Find(&a, 1, 0x3000, 4) != nullptr && cache.Find(&a, 1, 0x5000, 4) != nullptr,
	      "the newer entries stay");
}

void TestOffsetBelowTheAddress() {
	// A backing offset smaller than the guest address: delta wraps and the sum is still exact.
	RememberedMappings<2> cache;
	int                   a = 0;
	cache.Remember(&a, 1, 0x500000000ull, 0x500100000ull, 0x2000);
	const auto* e = cache.Find(&a, 1, 0x500000040ull, 16);
	Check(e != nullptr && 0x500000040ull + e->delta == 0x2040, "negative delta");
}

} // namespace

int main() {
	TestHitInsideAnEntry();
	TestMissOutsideOrAcrossTheEntry();
	TestEpochAndOwner();
	TestReplacementOrder();
	TestOffsetBelowTheAddress();
	std::printf("RememberedMappingTests: all passed\n");
	return 0;
}
