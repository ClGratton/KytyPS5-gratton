#include "graphics/host_gpu/renderer/pendingWrites.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::PendingBufferWrites;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PendingWritesTests: failed: %s\n", message);
		std::abort();
	}
}

void TestEmptyHasNoConflicts() {
	PendingBufferWrites w;
	Check(w.Empty() && w.Size() == 0, "starts empty");
	Check(!w.Conflicts(0x1000, 0x100, false), "nothing pending, nothing conflicts");
	w.Add(0x1000, 0, false);
	Check(w.Empty(), "an empty range is not recorded");
}

void TestOverlapRule() {
	PendingBufferWrites w;
	w.Add(0x1000, 0x100, false);
	Check(w.Conflicts(0x1000, 0x100, false), "same range conflicts");
	Check(w.Conflicts(0x10ff, 1, false), "last byte conflicts");
	Check(!w.Conflicts(0x1100, 0x10, false), "the byte after the end does not");
	Check(!w.Conflicts(0x0f00, 0x100, false), "a range ending at the start does not");
	Check(w.Conflicts(0x0f00, 0x101, false), "one byte into the range does");
	Check(w.Conflicts(0x0, 0x10000, false), "a range containing it does");
	Check(w.Conflicts(0x1040, 0x10, false), "a range inside it does");
	Check(!w.Conflicts(0x1000, 0, false), "an empty access never conflicts");
}

void TestOrderFreeAtomicsDoNotConflict() {
	PendingBufferWrites w;
	w.Add(0x2000, 0x40, true);
	Check(!w.Conflicts(0x2000, 0x40, true), "order-free atomics on both sides need no barrier");
	Check(w.Conflicts(0x2000, 0x40, false), "a plain access after an atomic write waits");
	w.Clear();
	w.Add(0x2000, 0x40, false);
	Check(w.Conflicts(0x2000, 0x40, true), "an atomic after a plain write waits");
	Check(w.Conflicts(0x2000, 0x40, false), "a plain access after a plain write waits");
}

void TestOneNonAtomicWriterTaintsTheRange() {
	PendingBufferWrites w;
	w.Add(0x3000, 0x40, true);
	w.Add(0x3000, 0x40, false);
	Check(w.Size() == 1, "the same range is kept once");
	Check(w.Conflicts(0x3000, 0x40, true), "a plain write to the range makes later atomics wait");
	w.Add(0x3000, 0x40, true);
	Check(w.Conflicts(0x3000, 0x40, true), "and a later atomic write does not clear that");
}

void TestOverlappingDifferentRanges() {
	PendingBufferWrites w;
	w.Add(0x4000, 0x100, true);
	w.Add(0x4080, 0x100, false);
	Check(w.Size() == 2, "different ranges are separate entries");
	Check(w.Conflicts(0x4000, 0x40, true) == false, "atomic on the atomic-only part is free");
	Check(w.Conflicts(0x4090, 0x10, true), "atomic on the plain-written part waits");
	Check(w.Conflicts(0x4040, 0x100, true), "atomic across both waits");
}

void TestBoundsAndSaturation() {
	PendingBufferWrites w;
	w.Add(UINT64_MAX - 0x10, 0x100, false);
	Check(w.Conflicts(UINT64_MAX - 8, 4, false), "a range reaching the top of the address space saturates");
	Check(!w.Conflicts(0x1000, 0x100, false), "and does not wrap around to low addresses");
	PendingBufferWrites v;
	v.Add(0x1000, 0x100, false);
	Check(v.Conflicts(UINT64_MAX - 4, UINT64_MAX, false) == false, "a huge access from a high base is bounded");
}

void TestFullBoundsTheSet() {
	PendingBufferWrites w;
	for (uint64_t i = 0; i < PendingBufferWrites::MaxEntries; i++) {
		w.Add(0x10000 + i * 0x100, 0x10, false);
	}
	Check(!w.Full(), "exactly the maximum is not full");
	w.Add(0x90000, 0x10, false);
	Check(w.Full(), "one more is full: the caller ends the instance and clears");
	w.Clear();
	Check(w.Empty() && !w.Full(), "clear empties it");
}

} // namespace

int main() {
	TestEmptyHasNoConflicts();
	TestOverlapRule();
	TestOrderFreeAtomicsDoNotConflict();
	TestOneNonAtomicWriterTaintsTheRange();
	TestOverlappingDifferentRanges();
	TestBoundsAndSaturation();
	TestFullBoundsTheSet();
	std::printf("PendingWritesTests: all passed\n");
	return 0;
}
