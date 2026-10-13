// Item watch of the PM4 lookahead (src/common/speculation.*): the item being scanned is stale once
// the GPU thread has started it; the watch is per thread.

#include "common/speculation.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SpeculationItemWatchTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	using namespace Common::Speculation;
	std::atomic<uint64_t> started {0};

	Check(!ItemStale(), "no watch is never stale");

	SetItemWatch(&started, 10);
	Check(!ItemStale(), "nothing started yet");
	started.store(9);
	Check(!ItemStale(), "an earlier submission started: the item is still ahead of the GPU thread");
	started.store(10);
	Check(ItemStale(), "the GPU thread started the item");
	started.store(500);
	Check(ItemStale(), "and went on: still stale");

	// The sequence is a maximum over all queues: an item the GPU thread may not have reached yet can
	// look started. That direction is the safe one, so it is the documented answer.
	SetItemWatch(&started, 499);
	Check(ItemStale(), "conservative for items below the maximum");

	// A new item resets the verdict.
	SetItemWatch(&started, 501);
	Check(!ItemStale(), "the next item is not started");
	started.store(501);
	Check(ItemStale(), "until the GPU thread gets to it");

	// Item sequence 0 means "unnumbered": never stale.
	SetItemWatch(&started, 0);
	Check(!ItemStale(), "sequence 0 is never stale");

	// The watch belongs to its thread.
	SetItemWatch(&started, 1);
	Check(ItemStale(), "this thread watches item 1");
	bool other_stale = true;
	std::thread other([&] { other_stale = ItemStale(); });
	other.join();
	Check(!other_stale, "another thread has no watch");

	ClearItemWatch();
	Check(!ItemStale(), "cleared");
	std::printf("SpeculationItemWatchTests: ok\n");
	return 0;
}
