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
	          totals.dependent[2] == 0 && totals.dependent[3] == 0,
	      "a visual read marks both events visual-dependent only");
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 2); // third event, no reader since
	totals = t.GetTotals();
	Check(totals.skip_events == 3 && totals.dependent[0] == 2, "a later event without a reader stays free");
	t.NoteRead({0x1000, 4}, ReadKind::ScalarWalk, 9, hits);
	totals = t.GetTotals();
	Check(totals.dependent[0] == 3 && totals.dependent[1] == 3 && totals.dependent[2] == 3 &&
	          totals.dependent[3] == 3,
	      "a table read marks every event at all four levels");
	const auto done = t.NoteReady(kKey, 100);
	Check(done.size() == 1 && done[0].skips == 3 && done[0].dependent[3] == 3, "resolve reports the stats");
	Check(!t.Active(), "no poison left");
	totals = t.GetTotals();
	Check(totals.resolved == 1 && totals.dependent[3] == 3 && totals.skip_events == 3,
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
	Check(totals.dependent[3] == 1 && totals.dependent[0] == 1, "the root has a table-class dependent through the chain");
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
	          SinkOf(ReadKind::IndexOrVertex) == Sink::Data,
	      "buffer, DMA and geometry reads are data");
	Check(SinkOf(ReadKind::CpuRead) == Sink::Cpu, "a CPU readback steers game logic");
	Check(SinkOf(ReadKind::ScalarWalk) == Sink::Table && SinkOf(ReadKind::IndirectArgs) == Sink::Table,
	      "descriptor walks and draw arguments are tables");
}

// ---- --skip-guard -----------------------------------------------------------------------------

// The unsafe set merges, answers overlap queries by binary search, and saturates fail-closed.
void TestRangeSet() {
	RangeSet set;
	Check(!set.Add({0x1000, 0}), "an empty range adds nothing");
	Check(set.Add({0x1000, 0x100}) && set.Size() == 1, "first range");
	Check(!set.Add({0x1020, 0x10}), "a covered range adds nothing");
	Check(set.Add({0x1100, 0x100}) && set.Size() == 1, "an adjacent range merges");
	Check(set.Add({0x3000, 0x100}) && set.Size() == 2, "a disjoint range is a second entry");
	Check(set.Add({0x1100, 0x2000}) && set.Size() == 1, "a bridging range merges everything it touches");
	Check(set.Overlaps({0x1000, 1}) && set.Overlaps({0x30ff, 1}) && set.Overlaps({0x0, 0x1001}),
	      "overlap inside and at the edges");
	Check(!set.Overlaps({0xfff, 1}) && !set.Overlaps({0x3100, 0x10}) && !set.Overlaps({0x1000, 0}),
	      "no overlap just outside or for an empty query");
	RangeSet full;
	for (uint64_t i = 0; i <= RangeSet::kMaxRanges; i++) full.Add({0x100000 + i * 0x1000, 0x100});
	Check(full.Saturated(), "too many disjoint ranges saturate the set");
	Check(full.Overlaps({0x7, 1}), "a saturated set answers yes to everything (everything waits)");
}

// A hard-class reader of a skipped draw's output makes that output unsafe; sampled images do not.
void TestGuardLearnsFromHardReads() {
	Tracker          t;
	std::vector<Hit> hits;
	const Range      color[] = {{0x1000, 0x400}, {0x5000, 0x40}};
	const Range      other   = {0x9000, 0x400};
	t.SetGuard(true, true);
	const auto id = t.NoteSkip(kKey, 1, 2, color, 0);
	t.NoteSkip(kKey, 1, 2, color, 1);
	const Range write[] = {{0x1200, 0x10}};
	Check(!t.MustWait(write), "nothing learned yet");

	t.NoteRead({0x1000, 16}, ReadKind::SampledImage, 7, hits);
	t.NoteRead({0x1000, 16}, ReadKind::BufferRead, 8, hits);
	Check(!t.MustWait(write) && t.GetGuardCounts().misses == 0, "image and buffer reads are not hard");

	hits.clear();
	Check(t.NoteRead({0x1100, 8}, ReadKind::ScalarWalk, 9, hits), "the SRT walk hits the target");
	Check(hits.size() == 1 && hits[0].hard && hits[0].newly_unsafe && hits[0].unsafe_added == 1 &&
	          hits[0].new_misses == 2 && hits[0].unsafe_first == color[0],
	      "first hard hit: the touched range is unsafe, both skip events are misses");
	Check(t.MustWait(write), "a draw writing the range now waits");
	const Range elsewhere[] = {{0x5000, 0x40}};
	Check(!t.MustWait(elsewhere), "the root's other range was not read, it stays skippable");
	Check(!t.MustWait(std::span<const Range>(&other, 1)), "unrelated ranges stay skippable");
	auto counts = t.GetGuardCounts();
	Check(counts.misses == 2 && counts.hard_ranges == 1 && counts.hard_roots == 1 &&
	          counts.hard_hits[static_cast<size_t>(ReadKind::ScalarWalk)] == 1,
	      "counters");

	hits.clear();
	t.NoteRead({0x1100, 8}, ReadKind::IndirectArgs, 10, hits);
	Check(hits.size() == 1 && hits[0].hard && !hits[0].newly_unsafe && hits[0].new_misses == 0,
	      "a second hard read of an unsafe range is not news");
	Check(t.GetGuardCounts().misses == 2, "misses are counted once per skip event");

	// A later skip of the same root (not vetoed by a caller) is a new event with its own miss.
	t.NoteSkip(kKey, 1, 2, color, 2);
	hits.clear();
	t.NoteRead({0x1100, 8}, ReadKind::Dma, 11, hits);
	Check(hits[0].new_misses == 1 && !hits[0].newly_unsafe, "a new skip event read later is a new miss");
	Check(t.GetGuardCounts().misses == 3, "three misses");

	t.NoteReady(kKey, 50);
	Check(t.MustWait(write), "the learned range outlives the root");
	(void)id;
}

// Without the guard nothing is learned and nothing waits.
void TestGuardOffLearnsNothing() {
	Tracker          t;
	std::vector<Hit> hits;
	const Range      rt = {0x1000, 0x400};
	t.NoteSkip(kKey, 1, 2, std::span(&rt, 1), 0);
	t.NoteRead({0x1000, 8}, ReadKind::ScalarWalk, 9, hits);
	Check(!hits[0].hard && !hits[0].newly_unsafe, "no learning with the guard off");
	Check(!t.MustWait(std::span(&rt, 1)) && t.GetGuardCounts().misses == 0, "and nothing waits");
}

// A hard read through a taint (a pass built the table from a skipped draw's output) marks every
// range of the root and counts the miss as derived.
void TestGuardDerivedRead() {
	Tracker          t;
	std::vector<Hit> sources;
	const Range      rt[]  = {{0x1000, 0x400}, {0x6000, 0x100}};
	const Range      table = {0x9000, 0x100};
	t.SetGuard(true, true);
	t.NoteSkip(kKey, 1, 2, rt, 0);
	t.NoteRead({0x1000, 64}, ReadKind::SampledImage, 11, sources);
	t.NoteTaint(sources, std::span(&table, 1));
	std::vector<Hit> walk;
	t.NoteRead({0x9010, 8}, ReadKind::ScalarWalk, 12, walk);
	Check(walk.size() == 1 && walk[0].derived && walk[0].hard && walk[0].unsafe_added == 2 &&
	          walk[0].new_misses == 1,
	      "a derived hard read marks both ranges of the root");
	const Range second[] = {{0x6040, 8}};
	Check(t.MustWait(second), "including the one nobody read directly");
	const auto counts = t.GetGuardCounts();
	Check(counts.misses == 1 && counts.misses_derived == 1, "a derived miss");
}

// The blind class: a shader with 64-bit loads that reads poison makes the roots it read unsafe, but
// that is not a known miss.
void TestGuardBdaSuspect() {
	for (const bool bda: {true, false}) {
		Tracker          t;
		std::vector<Hit> hits;
		const Range      rt[] = {{0x1000, 0x400}, {0x2000, 0x40}};
		t.SetGuard(true, bda);
		t.NoteSkip(kKey, 1, 2, rt, 0);
		t.NoteRead({0x1000, 8}, ReadKind::SampledImage, 5, hits);
		const auto added = t.NoteBdaSuspect(hits);
		if (bda) {
			Check(added.size() == 2 && t.MustWait(std::span(&rt[1], 1)), "all ranges of the root are unsafe");
			Check(t.GetGuardCounts().misses == 0 && t.GetGuardCounts().bda_ranges == 2,
			      "a suspicion is not a miss");
			Check(t.NoteBdaSuspect(hits).empty(), "and is news once");
		} else {
			Check(added.empty() && !t.MustWait(std::span(&rt[0], 1)), "the rule can be off");
		}
	}
}

// Hard classes: what steers later work, not what is merely shown.
void TestHardKinds() {
	Check(!IsHardRead(ReadKind::SampledImage) && !IsHardRead(ReadKind::BufferRead), "soft");
	Check(IsHardRead(ReadKind::ScalarWalk) && IsHardRead(ReadKind::IndirectArgs) &&
	          IsHardRead(ReadKind::IndexOrVertex) && IsHardRead(ReadKind::Dma) &&
	          IsHardRead(ReadKind::CpuRead),
	      "hard");
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
	TestRangeSet();
	TestGuardLearnsFromHardReads();
	TestGuardOffLearnsNothing();
	TestGuardDerivedRead();
	TestGuardBdaSuspect();
	TestHardKinds();
	std::printf("SkipDependenciesTests: ok\n");
	return 0;
}
