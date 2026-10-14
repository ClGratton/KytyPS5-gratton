// Item watch of the PM4 lookahead (src/common/speculation.*): the item being scanned is stale once
// the guest can have reused its memory (a guest-visible completion covers it); starting the item
// is not enough. The watch is per thread.

#include "common/guestRetirement.h"
#include "common/speculation.h"

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
	Common::GuestRetirement r;

	Check(!ItemStale(), "no watch is never stale");

	SetItemWatch(&r, 0, 10);
	Check(!ItemStale(), "nothing started yet");
	r.NoteStarted(0, 9);
	r.NoteSignal(0);
	Check(!ItemStale(), "an earlier submission retired: the item is still held by the guest");
	r.NoteStarted(0, 10);
	Check(!ItemStale(), "the GPU thread started the item: still not stale (stall mode keeps scanning)");
	r.NoteCompleted(0, 10);
	Check(!ItemStale(), "even finished, until the guest can have seen it");
	r.NoteSignal(0);
	Check(ItemStale(), "a guest-visible completion covers the item");
	r.NoteStarted(0, 500);
	r.NoteSignal(0);
	Check(ItemStale(), "and later ones keep it stale");

	// A new item resets the verdict.
	SetItemWatch(&r, 0, 501);
	Check(!ItemStale(), "the next item is not retired");
	r.NoteStarted(0, 501);
	Check(!ItemStale(), "started is not retired");
	r.NoteSignal(0);
	Check(ItemStale(), "until a signal covers it");

	// Another queue's item is judged by its own queue.
	SetItemWatch(&r, 1, 400);
	Check(!ItemStale(), "compute item 400 started by nobody: a graphics signal does not retire it");
	r.NoteStarted(1, 400);
	Check(!ItemStale(), "started compute item");
	r.NoteCompleted(1, 400);
	Check(ItemStale(), "finished and covered by a later graphics signal");

	// Item sequence 0 means "unnumbered": never stale.
	SetItemWatch(&r, 0, 0);
	Check(!ItemStale(), "sequence 0 is never stale");

	// The watch belongs to its thread.
	SetItemWatch(&r, 0, 1);
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
