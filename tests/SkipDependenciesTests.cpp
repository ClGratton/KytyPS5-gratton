#include "graphics/host_gpu/renderer/skipDependencies.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Libs::Graphics::SkipDeps;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SkipDependenciesTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t kKey = 0x1234;

// Range arithmetic: adjacent ranges do not overlap, empty and wrapping ranges are handled.
void TestRanges() {
	Check(Overlaps({100, 10}, {105, 10}), "partial overlap");
	Check(!Overlaps({100, 10}, {110, 10}), "adjacent ranges do not overlap");
	Check(!Overlaps({100, 10}, {90, 10}), "adjacent below");
	Check(!Overlaps({100, 0}, {100, 10}), "empty never overlaps");
	Check(Overlaps({UINT64_MAX - 4, 100}, {UINT64_MAX - 2, 1}), "wrapping range is clamped");
	Check(Covers({100, 100}, {120, 10}) && !Covers({100, 100}, {190, 20}) && !Covers({100, 100}, {120, 0}),
	      "covers");
}

// A reader of a skipped draw's output is reported, a reader elsewhere is not.
void TestDirectReader() {
	Tracker          t;
	std::vector<Hit> hits;
	const Range      rt {0x1000, 0x400};
	Check(!t.Active(), "nothing skipped yet");
	const auto id = t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 10);
	Check(id != 0 && t.Active(), "skip registers poison");
	Check(!t.NoteRead({0x2000, 16}, ReadKind::SampledImage, 7, hits) && hits.empty(), "elsewhere");
	Check(!t.NoteRead({0x1400, 16}, ReadKind::SampledImage, 7, hits), "just past the end");
	Check(t.NoteRead({0x13f0, 32}, ReadKind::SampledImage, 7, hits), "straddling the end hits");
	Check(hits.size() == 1 && hits[0].entry == id && !hits[0].derived && hits[0].first, "first hit");
	Check(hits[0].root_vs == 1 && hits[0].root_ps == 2, "root shader pair is reported");
	hits.clear();
	Check(t.NoteRead({0x1000, 4}, ReadKind::SampledImage, 7, hits) && !hits[0].first,
	      "the same reader hitting again is not a new report");
	hits.clear();
	Check(t.NoteRead({0x1000, 4}, ReadKind::SampledImage, 8, hits) && hits[0].first,
	      "another reader shader is a new report");
}

// Dependent counting: every skip event so far counts once per sink class, later events need their own read.
void TestDependentCounting() {
	Tracker          t;
	std::vector<Hit> hits;
	const Range      rt {0x1000, 0x400};
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 0);
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 1); // same key and ranges: merged, 2 events
	Check(t.EntryCount() == 1, "same pipeline and ranges share one entry");
	t.NoteRead({0x1000, 4}, ReadKind::SampledImage, 7, hits);
	auto totals = t.GetTotals();
	Check(totals.skip_events == 2 && totals.dependent[0] == 2 && totals.dependent[1] == 0 &&
	          totals.dependent[2] == 0,
	      "a visual read marks both events visual-dependent only");
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 2); // third event, no reader since
	totals = t.GetTotals();
	Check(totals.skip_events == 3 && totals.dependent[0] == 2, "a later event without a reader stays free");
	t.NoteRead({0x1000, 4}, ReadKind::ScalarWalk, 9, hits);
	totals = t.GetTotals();
	Check(totals.dependent[0] == 3 && totals.dependent[1] == 3 && totals.dependent[2] == 3,
	      "a table read marks every event at all three levels");
	const auto done = t.NoteReady(kKey, 100);
	Check(done.size() == 1 && done[0].skips == 3 && done[0].dependent[2] == 3, "resolve reports the stats");
	Check(!t.Active(), "no poison left");
	totals = t.GetTotals();
	Check(totals.resolved == 1 && totals.dependent[2] == 3 && totals.skip_events == 3,
	      "totals survive the resolve");
}

// Ready of another pipeline leaves the poison alone.
void TestReadyOnlyForTheKey() {
	Tracker     t;
	const Range rt {0x1000, 0x400};
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 0);
	Check(t.NoteReady(kKey + 1, 5).empty() && t.Active(), "another pipeline does not resolve");
	Check(t.NoteReady(kKey, 5).size() == 1 && !t.Active(), "its own pipeline does");
}

// A compute pass that reads poison and writes a buffer passes the poison on; reading that buffer as an
// SRT table is attributed to the skipped draw.
void TestTaintChain() {
	Tracker          t;
	std::vector<Hit> sources;
	const Range      rt {0x1000, 0x400};
	const Range      table {0x9000, 0x100};
	const auto       id = t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 0);
	Check(t.NoteRead({0x1000, 64}, ReadKind::SampledImage, 11, sources), "the compute pass reads the target");
	t.NoteTaint(sources, std::span(&table, 1));
	std::vector<Hit> walk;
	Check(t.NoteRead({0x9010, 8}, ReadKind::ScalarWalk, 12, walk), "the SRT walk reads the built table");
	Check(walk.size() == 1 && walk[0].derived && walk[0].depth == 1 && walk[0].root_count == 1 &&
	          walk[0].roots[0] == id,
	      "attributed to the skipped draw at depth 1");
	const auto totals = t.GetTotals();
	Check(totals.dependent[2] == 1, "the root has a table-class dependent through the chain");
	// A second pass reading the table and writing another buffer: depth 2.
	const Range second {0xa000, 0x40};
	t.NoteTaint(walk, std::span(&second, 1));
	std::vector<Hit> deeper;
	Check(t.NoteRead({0xa000, 4}, ReadKind::BufferRead, 13, deeper) && deeper[0].depth == 2, "depth 2");
	// Resolving the root removes the taints with it.
	Check(t.NoteReady(kKey, 9).size() == 1 && !t.Active() && t.EntryCount() == 0, "taint dies with its root");
}

// A taint with two roots keeps living while one root does.
void TestTaintSurvivesOneRoot() {
	Tracker          t;
	std::vector<Hit> sources;
	const Range      a {0x1000, 0x100};
	const Range      b {0x2000, 0x100};
	const Range      out {0x9000, 0x40};
	t.NoteSkip(1, 1, 1, std::span(&a, 1), 0);
	t.NoteSkip(2, 2, 2, std::span(&b, 1), 0);
	t.NoteRead({0x1000, 4}, ReadKind::BufferRead, 1, sources);
	t.NoteRead({0x2000, 4}, ReadKind::BufferRead, 1, sources);
	t.NoteTaint(sources, std::span(&out, 1));
	Check(t.EntryCount() == 3, "two roots and one taint");
	t.NoteReady(1, 5);
	std::vector<Hit> hits;
	Check(t.NoteRead({0x9000, 4}, ReadKind::ScalarWalk, 2, hits) && hits[0].root_count == 2,
	      "the taint still names both roots");
	t.NoteReady(2, 6);
	Check(t.EntryCount() == 0, "gone with the last root");
}

// A complete overwrite by the CPU or a DMA fill drops taints, never roots.
void TestOverwrite() {
	Tracker          t;
	std::vector<Hit> sources;
	const Range      a {0x1000, 0x100};
	const Range      out {0x9000, 0x40};
	t.NoteSkip(1, 1, 1, std::span(&a, 1), 0);
	t.NoteRead({0x1000, 4}, ReadKind::BufferRead, 1, sources);
	t.NoteTaint(sources, std::span(&out, 1));
	t.NoteOverwrite({0x9010, 0x10});
	Check(t.EntryCount() == 2, "a partial overwrite keeps the taint");
	t.NoteOverwrite({0x9000, 0x40});
	Check(t.EntryCount() == 1, "a full overwrite drops the taint");
	t.NoteOverwrite({0x0, 0x10000});
	Check(t.EntryCount() == 1, "a root stays until its pipeline is ready");
}

// The table is bounded; overflow is counted, not tracked.
void TestCapacity() {
	Tracker t;
	for (uint64_t i = 0; i < kMaxEntries + 10; i++) {
		const Range r {0x100000 + i * 0x1000, 0x100};
		t.NoteSkip(i, i, i, std::span(&r, 1), 0);
	}
	Check(t.EntryCount() == kMaxEntries, "bounded");
	Check(t.GetTotals().dropped_entries == 10, "overflow is counted");
}

// The sink classes.
void TestSinks() {
	Check(SinkOf(ReadKind::SampledImage) == Sink::Visual, "image reads are visual");
	Check(SinkOf(ReadKind::BufferRead) == Sink::Data && SinkOf(ReadKind::Dma) == Sink::Data &&
	          SinkOf(ReadKind::CpuRead) == Sink::Data && SinkOf(ReadKind::IndexOrVertex) == Sink::Data,
	      "buffer, DMA, CPU and geometry reads are data");
	Check(SinkOf(ReadKind::ScalarWalk) == Sink::Table && SinkOf(ReadKind::IndirectArgs) == Sink::Table,
	      "descriptor walks and draw arguments are tables");
}

} // namespace

int main() {
	TestRanges();
	TestDirectReader();
	TestDependentCounting();
	TestReadyOnlyForTheKey();
	TestTaintChain();
	TestTaintSurvivesOneRoot();
	TestOverwrite();
	TestCapacity();
	TestSinks();
	std::printf("SkipDependenciesTests: ok\n");
	return 0;
}
