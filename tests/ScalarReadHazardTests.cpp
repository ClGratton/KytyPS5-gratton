#include "graphics/host_gpu/renderer/pipeline/scalarReadHazard.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

using namespace Libs::Graphics;
using Range = std::pair<uint64_t, uint64_t>;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ScalarReadHazardTests: failed: %s\n", message);
		std::abort();
	}
}

// The check as CommitBindings first wrote it: every read against every written range, images first,
// each range tested with ImageRangeOverlaps (which aborts on a zero-sized or wrapping range).
ScalarReadHazard Reference(const std::vector<ScalarRead>& reads, const std::vector<Range>& images,
                           const std::vector<Range>& buffers) {
	auto overlaps = [](const ScalarRead& read, const Range& written, bool& invalid) {
		if (read.second == 0 || written.second == 0 || read.first > UINT64_MAX - read.second ||
		    written.first > UINT64_MAX - written.second) {
			invalid = true;
			return false;
		}
		return read.first < written.first + written.second &&
		       written.first < read.first + read.second;
	};
	for (size_t i = 0; i < reads.size(); ++i) {
		for (const auto& written: images) {
			if (written.second == 0) continue;
			bool invalid = false;
			const bool hit = overlaps(reads[i], written, invalid);
			if (invalid) return {ScalarReadHazardKind::InvalidRange, i};
			if (hit) return {ScalarReadHazardKind::ImageWrite, i};
		}
		for (const auto& written: buffers) {
			if (written.second == 0) continue;
			bool invalid = false;
			const bool hit = overlaps(reads[i], written, invalid);
			if (invalid) return {ScalarReadHazardKind::InvalidRange, i};
			if (hit) return {ScalarReadHazardKind::BufferWrite, i};
		}
	}
	return {};
}

ScalarReadHazard Indexed(const std::vector<ScalarRead>& reads, const std::vector<Range>& images,
                         const std::vector<Range>& buffers) {
	WrittenRangeIndex image_index;
	WrittenRangeIndex buffer_index;
	for (const auto& range: images) image_index.Add(range.first, range.second);
	for (const auto& range: buffers) buffer_index.Add(range.first, range.second);
	image_index.Build();
	buffer_index.Build();
	return FindScalarReadHazard(reads, image_index, buffer_index);
}

void CheckSame(const std::vector<ScalarRead>& reads, const std::vector<Range>& images,
               const std::vector<Range>& buffers, const char* message) {
	const auto expected = Reference(reads, images, buffers);
	const auto actual   = Indexed(reads, images, buffers);
	Check(expected.kind == actual.kind && expected.read_index == actual.read_index, message);
}

void TestEmpty() {
	CheckSame({}, {}, {}, "no reads, no writes");
	CheckSame({{0x1000, 4}}, {}, {}, "reads without writes never fire");
	CheckSame({}, {{0x1000, 4}}, {{0x2000, 4}}, "writes without reads never fire");
	// With nothing written nothing is validated, even a zero-sized read.
	CheckSame({{0x1000, 0}, {UINT64_MAX, 8}}, {}, {}, "invalid reads against no writes are ignored");
	CheckSame({{0x1000, 4}}, {{0x1000, 0}}, {{0x1000, 0}}, "zero-sized writes are ignored");
}

void TestTouchingRanges() {
	// A read ending where a write starts, or starting where it ends, does not overlap.
	CheckSame({{0x1000, 0x10}}, {{0x1010, 0x10}}, {}, "read ends at the write");
	CheckSame({{0x1010, 0x10}}, {{0x1000, 0x10}}, {}, "read starts at the write's end");
	CheckSame({{0x1000, 0x10}}, {}, {{0x1010, 0x10}}, "buffer: read ends at the write");
	CheckSame({{0x1010, 0x10}}, {}, {{0x1000, 0x10}}, "buffer: read starts at the write's end");
	// One byte of overlap on each side does.
	CheckSame({{0x1000, 0x11}}, {{0x1010, 0x10}}, {}, "one byte into the write");
	CheckSame({{0x100f, 0x11}}, {{0x1000, 0x10}}, {}, "one byte from the write's end");
	Check(Indexed({{0x1000, 0x11}}, {{0x1010, 0x10}}, {}).kind == ScalarReadHazardKind::ImageWrite,
	      "one byte of overlap is an image hazard");
	Check(Indexed({{0x1000, 0x10}}, {{0x1010, 0x10}}, {}).kind == ScalarReadHazardKind::None,
	      "touching is no hazard");
}

void TestContainmentAndDuplicates() {
	// A long early range must still be seen after shorter ranges that sort after it.
	const std::vector<Range> writes = {{0x100, 0x1000}, {0x200, 4}, {0x300, 4}, {0x400, 4}};
	CheckSame({{0x700, 4}}, writes, {}, "read inside a long early range");
	CheckSame({{0x1100, 4}}, writes, {}, "read just after the long range");
	CheckSame({{0x10, 0x10}}, writes, {}, "read before everything");
	CheckSame({{0x500, 4}}, {{0x500, 4}, {0x500, 4}, {0x500, 4}}, {{0x500, 4}}, "duplicates");
	CheckSame({{0x500, 4}, {0x500, 4}}, {}, {{0x500, 4}, {0x500, 4}}, "duplicate reads and writes");
}

void TestOrderAndKind() {
	// The first read in capture order wins, and for one read images are reported before buffers.
	CheckSame({{0x10, 4}, {0x2000, 4}, {0x3000, 4}}, {{0x3000, 4}}, {{0x2000, 4}}, "first read wins");
	CheckSame({{0x2000, 4}}, {{0x2000, 4}}, {{0x2000, 4}}, "images before buffers");
	Check(Indexed({{0x2000, 4}}, {{0x2000, 4}}, {{0x2000, 4}}).kind == ScalarReadHazardKind::ImageWrite,
	      "an image hit is reported as an image");
	Check(Indexed({{0x2000, 4}}, {}, {{0x2000, 4}}).kind == ScalarReadHazardKind::BufferWrite,
	      "a buffer hit is reported as a buffer");
	Check(Indexed({{0x10, 4}, {0x2000, 4}}, {}, {{0x2000, 4}}).read_index == 1, "the read index");
}

void TestInvalidRanges() {
	CheckSame({{0x1000, 0}}, {{0x2000, 4}}, {}, "zero-sized read against an image write");
	CheckSame({{0x1000, 0}}, {}, {{0x2000, 4}}, "zero-sized read against a buffer write");
	CheckSame({{UINT64_MAX - 3, 8}}, {{0x2000, 4}}, {}, "wrapping read");
	CheckSame({{0x10, 4}, {UINT64_MAX, 1}, {0x2000, 4}}, {}, {{0x5000, 4}}, "wrap after valid reads");
	// Without anything to compare against nothing is invalid; with a hit earlier in capture order the
	// earlier hit is reported.
	CheckSame({{0x2000, 4}, {0x1000, 0}}, {{0x2000, 4}}, {}, "hit before an invalid read");
	// A wrapping written range aborts (the exact kind may differ from the nested loops when another
	// range of the same set overlaps first in order; both abort).
	const auto wrapped = Indexed({{0x10, 4}}, {{UINT64_MAX - 1, 16}}, {});
	Check(wrapped.kind == ScalarReadHazardKind::InvalidRange && wrapped.read_index == 0,
	      "wrapping written range");
	WrittenRangeIndex only_invalid;
	only_invalid.Add(UINT64_MAX, 2);
	only_invalid.Build();
	Check(!only_invalid.Empty() && only_invalid.HasInvalid() && only_invalid.Count() == 0,
	      "a set holding only a wrapping range is not empty");
}

void TestRebuildKeepsNothing() {
	WrittenRangeIndex index;
	index.Add(0x100, 0x10);
	index.Build();
	Check(index.Overlaps(0x100, 1), "first build");
	index.Clear();
	index.Add(0x900, 0x10);
	index.Build();
	Check(!index.Overlaps(0x100, 1) && index.Overlaps(0x908, 1), "the second build forgot the first");
	index.Clear();
	index.Build();
	Check(index.Empty() && !index.Overlaps(0, 1), "cleared index");
}

std::vector<Range> RandomRanges(std::mt19937_64& rng, size_t count, uint64_t space, bool allow_bad) {
	std::vector<Range> result;
	for (size_t i = 0; i < count; ++i) {
		uint64_t address = rng() % space;
		uint64_t size    = rng() % 9; // zero-sized ones included
		if (allow_bad && rng() % 40 == 0) {
			address = UINT64_MAX - rng() % 6;
			size    = 1 + rng() % 12; // may wrap
		}
		result.push_back({address, size});
	}
	return result;
}

void TestRandomAgainstNestedLoops() {
	std::mt19937_64 rng(0x5eed);
	size_t          hits[4] = {};
	// Tiny address spaces make touching ranges, duplicates and containment common.
	for (const uint64_t space: {uint64_t {24}, uint64_t {64}, uint64_t {4096}, uint64_t {1} << 40}) {
		for (int trial = 0; trial < 30000; ++trial) {
			const bool bad    = trial % 7 == 0;
			const auto reads  = RandomRanges(rng, rng() % 12, space, bad);
			const auto images = RandomRanges(rng, rng() % 6, space, bad);
			const auto buffers = RandomRanges(rng, rng() % 6, space, bad);
			const auto expected = Reference(reads, images, buffers);
			const auto actual   = Indexed(reads, images, buffers);
			bool wrapped_write  = false;
			for (const auto* set: {&images, &buffers}) {
				for (const auto& range: *set) {
					wrapped_write |= range.second != 0 && range.first > UINT64_MAX - range.second;
				}
			}
			if (!wrapped_write) {
				Check(expected.kind == actual.kind && expected.read_index == actual.read_index,
				      "random set equals the nested loops");
			} else {
				// A wrapping written range: both abort, on the same read unless the nested loops
				// found a hit on an earlier read first, which the index also reports (it checks reads
				// in order) as a hit or as the invalid range.
				Check((expected.kind != ScalarReadHazardKind::None) ==
				          (actual.kind != ScalarReadHazardKind::None),
				      "random set with a wrapping write: same verdict");
			}
			++hits[static_cast<int>(expected.kind)];
		}
	}
	// The generator must exercise every outcome, or the comparison proves nothing.
	for (int kind = 0; kind < 4; ++kind) {
		Check(hits[kind] > 1000, "every outcome occurs in the random sets");
	}
}

void TestRandomIndexAgainstBruteForce() {
	std::mt19937_64 rng(77);
	for (int trial = 0; trial < 3000; ++trial) {
		const auto      writes = RandomRanges(rng, rng() % 40, 512, false);
		WrittenRangeIndex index;
		for (const auto& range: writes) index.Add(range.first, range.second);
		index.Build();
		for (int query = 0; query < 60; ++query) {
			const uint64_t address = rng() % 540;
			const uint64_t size    = 1 + rng() % 12;
			bool           brute   = false;
			for (const auto& range: writes) {
				if (range.second != 0 && address < range.first + range.second &&
				    range.first < address + size) {
					brute = true;
				}
			}
			Check(index.Overlaps(address, size) == brute, "Overlaps equals brute force");
		}
	}
}

// The read hull: when it proves "no hazard", the full check must say None (soundness, on every
// random set); when the sets are valid and the full check says None, the proof must exist
// (tightness: otherwise the hull would never be used).
void TestReadHullAgainstFullCheck() {
	std::mt19937_64 rng(0xa11ce);
	size_t proofs = 0, refusals = 0, none_without_proof = 0, empty_reads = 0;
	for (const uint64_t space: {uint64_t {24}, uint64_t {64}, uint64_t {4096}, uint64_t {1} << 40}) {
		for (int trial = 0; trial < 30000; ++trial) {
			const bool bad     = trial % 9 == 0;
			const auto reads   = RandomRanges(rng, rng() % 12, space, bad);
			const auto images  = RandomRanges(rng, rng() % 4, space, bad);
			const auto buffers = RandomRanges(rng, rng() % 4, space, bad);
			WrittenRangeIndex image_index;
			WrittenRangeIndex buffer_index;
			for (const auto& range: images) image_index.Add(range.first, range.second);
			for (const auto& range: buffers) buffer_index.Add(range.first, range.second);
			image_index.Build();
			buffer_index.Build();
			Libs::Graphics::ShaderRecompiler::IR::ReadHull hull;
			BuildReadHull(reads, hull);
			Check(hull.valid, "a built hull is valid");
			const bool proof = HullProvesNoHazard(hull, image_index, buffer_index);
			const auto full  = FindScalarReadHazard(reads, image_index, buffer_index);
			if (proof) {
				Check(full.kind == ScalarReadHazardKind::None, "a hull proof means the full check finds nothing");
				++proofs;
			} else {
				++refusals;
			}
			if (full.kind == ScalarReadHazardKind::None && !hull.has_invalid && !image_index.HasInvalid() &&
			    !buffer_index.HasInvalid()) {
				Check(proof, "no hazard and nothing invalid: the hull proves it");
			} else if (full.kind == ScalarReadHazardKind::None && !proof) {
				++none_without_proof; // an invalid range met an empty set: the caller runs the full check
			}
			empty_reads += reads.empty();
		}
	}
	Check(proofs > 10000 && refusals > 10000 && empty_reads > 100, "the generator exercises proofs and refusals");
	// An unbuilt hull never proves anything.
	WrittenRangeIndex none;
	none.Build();
	Libs::Graphics::ShaderRecompiler::IR::ReadHull unbuilt;
	Check(!HullProvesNoHazard(unbuilt, none, none), "an unbuilt hull has no proof");
}

// The order the index is built in does not matter to the read hull (it walks the entries) and Overlaps builds it on
// demand: a hull proof and the full check on an index nobody called Build() on agree with the built one
// (--desc-commit-diet leaves the Build out).
void TestLazyBuild() {
	std::mt19937_64 rng(0x1a2b3c);
	size_t          proofs = 0, overlaps = 0;
	for (const uint64_t space: {uint64_t {64}, uint64_t {4096}, uint64_t {1} << 40}) {
		for (int trial = 0; trial < 20000; ++trial) {
			const auto reads   = RandomRanges(rng, rng() % 10, space, trial % 9 == 0);
			const auto images  = RandomRanges(rng, rng() % 4, space, trial % 9 == 0);
			const auto buffers = RandomRanges(rng, rng() % 4, space, trial % 9 == 0);
			WrittenRangeIndex built_images, built_buffers, lazy_images, lazy_buffers;
			for (const auto& range: images) {
				built_images.Add(range.first, range.second);
				lazy_images.Add(range.first, range.second);
			}
			for (const auto& range: buffers) {
				built_buffers.Add(range.first, range.second);
				lazy_buffers.Add(range.first, range.second);
			}
			built_images.Build();
			built_buffers.Build();
			Libs::Graphics::ShaderRecompiler::IR::ReadHull hull;
			BuildReadHull(reads, hull);
			Check(HullProvesNoHazard(hull, built_images, built_buffers) ==
			          HullProvesNoHazard(hull, lazy_images, lazy_buffers),
			      "the hull proof does not need the sort");
			proofs += HullProvesNoHazard(hull, lazy_images, lazy_buffers);
			const auto expected = FindScalarReadHazard(reads, built_images, built_buffers);
			const auto actual   = FindScalarReadHazard(reads, lazy_images, lazy_buffers);
			Check(expected.kind == actual.kind && expected.read_index == actual.read_index,
			      "the full check builds the index when it first needs it");
			overlaps += expected.kind != ScalarReadHazardKind::None;
			// Add after a query invalidates the sorted order again.
			lazy_images.Add(0x40, 8);
			built_images.Add(0x40, 8);
			built_images.Build();
			for (const auto& read: reads) {
				if (read.second == 0 || read.first > UINT64_MAX - read.second) continue;
				Check(built_images.Overlaps(read.first, read.second) == lazy_images.Overlaps(read.first, read.second),
				      "Add after a query is seen by the next query");
			}
		}
	}
	Check(proofs > 1000 && overlaps > 1000, "the generator exercises proofs and hazards");
}

} // namespace

int main() {
	TestLazyBuild();
	TestEmpty();
	TestTouchingRanges();
	TestContainmentAndDuplicates();
	TestOrderAndKind();
	TestInvalidRanges();
	TestRebuildKeepsNothing();
	TestRandomAgainstNestedLoops();
	TestRandomIndexAgainstBruteForce();
	TestReadHullAgainstFullCheck();
	std::puts("ScalarReadHazardTests: all passed");
	return 0;
}
