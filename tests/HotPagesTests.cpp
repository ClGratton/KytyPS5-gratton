#include "graphics/host_gpu/renderer/cache/hotPages.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Libs::Graphics::DecideRetire;
using Libs::Graphics::HotPageSet;
using Libs::Graphics::HotRetire;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HotPagesTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t Page(uint64_t index) { return 0x100000000ull + index * 4096; }

void TestFaultMarksHot() {
	HotPageSet set(4);
	Check(set.Empty() && !set.Contains(Page(1)), "starts empty");
	set.NoteFault(Page(1));
	set.NoteFault(Page(1));
	Check(set.Size() == 1 && set.Contains(Page(1)), "a repeated fault keeps one entry");
	Check(set.InFlight(Page(1)) == 0 && set.InFlight(Page(9)) == 0, "no copy in flight yet");
}

void TestLeastRecentlyFaultedIsReplaced() {
	HotPageSet set(3);
	set.NoteFault(Page(1));
	set.NoteFault(Page(2));
	set.NoteFault(Page(3));
	set.NoteFault(Page(1)); // page 2 is now the least recently faulted
	set.NoteFault(Page(4));
	Check(set.Size() == 3, "the set stays at its capacity");
	Check(!set.Contains(Page(2)) && set.Contains(Page(1)) && set.Contains(Page(3)) &&
	          set.Contains(Page(4)),
	      "the page faulted on least recently makes room");
}

void TestInFlightPagesAreNotReplaced() {
	HotPageSet set(2);
	set.NoteFault(Page(1));
	set.NoteFault(Page(2));
	set.SetInFlight(Page(1), 70);
	set.NoteFault(Page(3));
	Check(set.Contains(Page(1)) && set.InFlight(Page(1)) == 70, "a page with a copy in flight stays");
	Check(!set.Contains(Page(2)) && set.Contains(Page(3)), "the other page is replaced");
	set.SetInFlight(Page(3), 71);
	set.NoteFault(Page(4));
	Check(!set.Contains(Page(4)) && set.Size() == 2, "all in flight: the new page is ignored");
	set.SetInFlight(Page(1), 0);
	set.NoteFault(Page(4));
	Check(set.Contains(Page(4)) && !set.Contains(Page(1)), "a retired page can be replaced again");
}

void TestRangeIterationIsOrderedAndBounded() {
	HotPageSet set(8);
	for (uint64_t p: {5, 1, 3, 7, 2}) set.NoteFault(Page(p));
	std::vector<uint64_t> seen;
	set.ForEachInRange(Page(2), Page(6), [&](uint64_t page, HotPageSet::Entry& entry) {
		seen.push_back(page);
		entry.in_flight_tick = 9;
	});
	Check(seen == std::vector<uint64_t> {Page(2), Page(3), Page(5)}, "pages in [begin, end), by address");
	Check(set.InFlight(Page(3)) == 9 && set.InFlight(Page(1)) == 0, "the callback may mark in flight");
	set.ForEachInRange(Page(8), Page(9), [&](uint64_t, HotPageSet::Entry&) { Check(false, "empty range"); });
}

void TestZeroCapacityTracksNothing() {
	HotPageSet set(0);
	set.NoteFault(Page(1));
	Check(set.Empty(), "capacity 0: eager write-back is off");
}

void TestRetireDecision() {
	Check(DecideRetire(false, false, false) == HotRetire::Wait, "background poll: not done, do not block");
	Check(DecideRetire(false, true, false) == HotRetire::Unprotect,
	      "a guest read that waited: unprotect when nothing newer was written");
	Check(DecideRetire(true, false, false) == HotRetire::Unprotect, "done and nothing newer: unprotect");
	Check(DecideRetire(true, false, true) == HotRetire::StayProtected,
	      "a newer GPU write: stay protected (its bytes are not in guest memory)");
	Check(DecideRetire(false, true, true) == HotRetire::StayProtected, "a newer write after a wait: stay protected");
}

} // namespace

int main() {
	TestFaultMarksHot();
	TestLeastRecentlyFaultedIsReplaced();
	TestInFlightPagesAreNotReplaced();
	TestRangeIterationIsOrderedAndBounded();
	TestZeroCapacityTracksNothing();
	TestRetireDecision();
	std::printf("HotPagesTests: ok\n");
	return 0;
}
