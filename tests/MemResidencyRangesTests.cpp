#include "graphics/host_gpu/memResidencyRanges.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::MemResidency;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "MemResidencyRangesTests: failed: %s\n", text);
		std::abort();
	}
}

void TestMerge() {
	// Rounded outwards to the granule, sorted, touching and overlapping ranges joined.
	const auto merged = Merge({{0x5100, 0x100}, {0x1000, 0x1000}, {0x1800, 0x2000}, {0x9000, 0}}, 0x1000);
	Check(merged.size() == 2, "merge produced the wrong number of ranges");
	Check(merged[0] == Range {0x1000, 0x3000}, "overlapping ranges were not joined");
	Check(merged[1] == Range {0x5000, 0x1000}, "range was not rounded outwards");
	Check(TotalBytes(merged) == 0x4000, "total bytes are wrong");
	Check(Merge({}, 0x1000).empty(), "empty input is not empty");
}

void TestSubtract() {
	const RangeList a {{0x1000, 0x4000}, {0x8000, 0x1000}};
	const RangeList b {{0x2000, 0x1000}, {0x4000, 0x6000}};
	const auto      out = Subtract(a, b);
	Check(out.size() == 2, "subtract produced the wrong number of ranges");
	Check(out[0] == Range {0x1000, 0x1000}, "subtract kept the wrong head");
	Check(out[1] == Range {0x3000, 0x1000}, "subtract kept the wrong middle");
	Check(Subtract(a, {}) == a, "subtracting nothing changed the list");
	Check(Subtract({}, b).empty(), "subtracting from nothing is not empty");
	Check(Subtract(a, a).empty(), "subtracting a list from itself is not empty");
}

void TestIntersectUnion() {
	const RangeList a {{0x1000, 0x3000}, {0x8000, 0x1000}};
	const RangeList b {{0x2000, 0x4000}, {0x8800, 0x1000}};
	const auto      both = Intersect(a, b);
	Check(both.size() == 2 && both[0] == Range {0x2000, 0x2000} && both[1] == Range {0x8800, 0x800},
	      "intersect is wrong");
	const auto all = Union(a, b);
	Check(all.size() == 2 && all[0] == Range {0x1000, 0x5000} && all[1] == Range {0x8000, 0x1800},
	      "union is wrong");
	Check(TotalBytes(all) == TotalBytes(a) + TotalBytes(b) - TotalBytes(both),
	      "inclusion-exclusion does not hold");
}

} // namespace

int main() {
	TestMerge();
	TestSubtract();
	TestIntersectUnion();
	std::puts("MemResidencyRangesTests: all cases passed");
	return 0;
}
