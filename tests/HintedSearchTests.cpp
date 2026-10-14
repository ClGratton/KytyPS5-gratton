#include "graphics/host_gpu/renderer/pipeline/hintedSearch.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace {

using Libs::Graphics::FindWithHint;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HintedSearchTests: failed: %s\n", message);
		std::abort();
	}
}

uint64_t Next(uint64_t& state) {
	state += 0x9e3779b97f4a7c15ull;
	uint64_t z = state;
	z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

template <typename Items>
auto* PlainScan(Items& items, uint64_t key) {
	for (auto& item: items) {
		if (item == key) return &item;
	}
	decltype(&items[0]) none = nullptr;
	return none;
}

// Unique matches (what Publish guarantees): the hinted search returns the scan's element for every
// key, with the hint on, off, stale, and while the container grows.
void TestUniqueMatchesAgreeWithTheScan() {
	std::deque<uint64_t>  items;
	std::atomic<size_t>   hint {0};
	uint64_t              rng = 3;
	for (int step = 0; step < 20000; step++) {
		if (Next(rng) % 8 == 0 && items.size() < 40) {
			uint64_t key = Next(rng) % 64;
			if (PlainScan(items, key) == nullptr) items.push_back(key);
		}
		const uint64_t key = Next(rng) % 64;
		const bool     use = Next(rng) % 4 != 0;
		auto*          got = FindWithHint(items, hint, use, [&](const uint64_t& v) { return v == key; });
		Check(got == PlainScan(items, key), "hinted search differs from the plain scan on unique keys");
	}
	// A hint beyond the container (stale after nothing was removed cannot happen, but a fresh entry
	// starts at 0) and an empty container.
	std::deque<uint64_t> empty;
	std::atomic<size_t>  big {99};
	Check(FindWithHint(empty, big, true, [](const uint64_t&) { return true; }) == nullptr, "empty container");
}

// Repeated matches are the documented precondition: with a hint on the second equal element the
// hinted search returns that one, the plain scan the first. This is why FindExistingLocked turns the
// hint off once a permutation was published with calls.
void TestRepeatedMatchesAreOutsideThePrecondition() {
	std::deque<uint64_t> items {5, 7, 5};
	std::atomic<size_t>  hint {2};
	auto*                hinted = FindWithHint(items, hint, true, [](const uint64_t& v) { return v == 5; });
	Check(hinted == &items[2] && PlainScan(items, 5) == &items[0], "repeated matches behave as documented");
	auto* off = FindWithHint(items, hint, false, [](const uint64_t& v) { return v == 5; });
	Check(off == &items[0], "hint off returns the first match");
}

} // namespace

int main() {
	TestUniqueMatchesAgreeWithTheScan();
	TestRepeatedMatchesAreOutsideThePrecondition();
	std::puts("HintedSearchTests: ok");
	return 0;
}
