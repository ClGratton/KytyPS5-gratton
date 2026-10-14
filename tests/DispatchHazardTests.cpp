#include "graphics/host_gpu/renderer/dispatchHazard.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::DispatchAccess;
using Space = DispatchAccess::Space;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DispatchHazardTests: failed: %s\n", message);
		std::abort();
	}
}

DispatchAccess Make(Space space, uint64_t key, uint64_t begin, uint64_t size, bool write) {
	DispatchAccess a;
	a.Add(space, key, begin, size, write);
	return a;
}

void TestReadsDoNotConflict() {
	const auto a = Make(Space::Guest, 0, 0x1000, 0x100, false);
	const auto b = Make(Space::Guest, 0, 0x1000, 0x100, false);
	Check(!a.ConflictsWith(b), "two readers of the same bytes overlap freely");
}

void TestWriteConflictsWithOverlap() {
	const auto w = Make(Space::Guest, 0, 0x1000, 0x100, true);
	Check(w.ConflictsWith(Make(Space::Guest, 0, 0x1080, 0x10, false)), "read after write");
	Check(Make(Space::Guest, 0, 0x1080, 0x10, false).ConflictsWith(w), "write after read (symmetric)");
	Check(w.ConflictsWith(Make(Space::Guest, 0, 0x1000, 0x100, true)), "write after write");
	Check(!w.ConflictsWith(Make(Space::Guest, 0, 0x1100, 0x100, true)), "adjacent ranges do not conflict");
	Check(!w.ConflictsWith(Make(Space::Guest, 0, 0x0f00, 0x100, true)), "adjacent below does not conflict");
	Check(w.ConflictsWith(Make(Space::Guest, 0, 0x0f00, 0x101, false)), "one byte of overlap does");
}

void TestSpacesAndKeysAreSeparate() {
	const auto w = Make(Space::HostBuffer, 7, 0, 0x100, true);
	Check(!w.ConflictsWith(Make(Space::HostBuffer, 8, 0, 0x100, true)), "different host buffers");
	Check(!w.ConflictsWith(Make(Space::Guest, 7, 0, 0x100, true)), "guest range is not a host buffer");
	Check(w.ConflictsWith(Make(Space::HostBuffer, 7, 0x80, 0x100, false)), "same host buffer overlaps");
	const auto image = Make(Space::HostImage, 5, 0, 1, true);
	Check(image.ConflictsWith(Make(Space::HostImage, 5, 0, 1, false)), "the same image: a write conflicts");
	Check(!image.ConflictsWith(Make(Space::HostImage, 6, 0, 1, true)), "another image");
}

void TestUnknownAccesses() {
	DispatchAccess reads_all;
	reads_all.reads_all = true;
	DispatchAccess writes_all;
	writes_all.writes_all = true;
	const auto reader = Make(Space::Guest, 0, 0x1000, 0x10, false);
	const auto writer = Make(Space::Guest, 0, 0x1000, 0x10, true);
	Check(!reads_all.ConflictsWith(reader), "an address reader and a reader overlap freely");
	Check(reads_all.ConflictsWith(writer), "an address reader waits for any writer");
	Check(writer.ConflictsWith(reads_all), "and a writer waits for an address reader");
	Check(writes_all.ConflictsWith(reader), "an address writer conflicts with any reader");
	Check(writes_all.ConflictsWith(writer), "and any writer");
	DispatchAccess nothing;
	Check(!writes_all.ConflictsWith(nothing), "but not with a dispatch that touches nothing");
	Check(!nothing.ConflictsWith(nothing), "nothing and nothing");
	Check(nothing.Empty() && !reads_all.Empty() && !writes_all.Empty(), "Empty");
}

void TestMerge() {
	auto group = Make(Space::Guest, 0, 0x1000, 0x100, false);
	group.Merge(Make(Space::Guest, 0, 0x1000, 0x100, false));
	Check(group.ranges.size() == 1, "a repeated range is kept once");
	Check(!group.HasWrites(), "still read only");
	group.Merge(Make(Space::Guest, 0, 0x1000, 0x100, true));
	Check(group.ranges.size() == 1 && group.HasWrites(), "a write to the same range wins");
	group.Merge(Make(Space::Guest, 0, 0x5000, 0x10, false));
	Check(group.ranges.size() == 2, "a new range is added");
	DispatchAccess all;
	all.writes_all = true;
	group.Merge(all);
	Check(group.writes_all, "the flags merge");
	Check(group.ConflictsWith(Make(Space::Guest, 0, 0x9000, 0x10, false)), "a group with an address writer conflicts with everything");
}

void TestGroupConflictIsAnyMember() {
	auto group = Make(Space::Guest, 0, 0x1000, 0x100, false);
	group.Merge(Make(Space::Guest, 0, 0x2000, 0x100, true));
	Check(!group.ConflictsWith(Make(Space::Guest, 0, 0x3000, 0x10, true)), "independent of every member");
	Check(group.ConflictsWith(Make(Space::Guest, 0, 0x1010, 0x10, true)), "writes what a member reads");
	Check(group.ConflictsWith(Make(Space::Guest, 0, 0x2010, 0x10, false)), "reads what a member writes");
}

void TestBounds() {
	DispatchAccess a;
	a.Add(Space::Guest, 0, UINT64_MAX - 8, 100, true);
	Check(a.ranges.size() == 1 && a.ranges[0].end == UINT64_MAX, "a range at the top saturates");
	Check(!a.ConflictsWith(Make(Space::Guest, 0, 0x1000, 0x100, true)), "and does not wrap");
	DispatchAccess b;
	b.Add(Space::Guest, 0, 0x1000, 0, true);
	Check(b.Empty(), "a zero-size range is ignored");
}

} // namespace

int main() {
	TestReadsDoNotConflict();
	TestWriteConflictsWithOverlap();
	TestSpacesAndKeysAreSeparate();
	TestUnknownAccesses();
	TestMerge();
	TestGroupConflictIsAnyMember();
	TestBounds();
	std::printf("DispatchHazardTests: all passed\n");
	return 0;
}
