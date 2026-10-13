#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

using namespace Libs::Graphics::SrtPlan;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SrtPlanTests: failed: %s\n", message);
		std::abort();
	}
}

// ---- keys and context ---------------------------------------------------------------------

void TestKeyEqualityHashAndOrder() {
	const Key a {7, 3, 0};
	const Key b {7, 3, 1};
	const Key c {7, 4, 0};
	const Key d {8, 3, 0};
	Check(a == Key {7, 3, 0}, "equal keys");
	Check(!(a == b) && !(a == c) && !(a == d), "each field takes part in the equality");
	KeyHash hash;
	Check(hash(a) != hash(b) && hash(a) != hash(c) && hash(a) != hash(d), "fields take part in the hash");
	std::unordered_map<Key, int, KeyHash> map;
	for (uint64_t seq = 1; seq <= 50; ++seq)
		for (uint32_t ordinal = 1; ordinal <= 50; ++ordinal)
			for (uint32_t call = 0; call < 2; ++call) map[{seq, ordinal, call}] = int(seq * 1000 + ordinal * 2 + call);
	Check(map.size() == 5000, "no key collides in the map");
	Check(map.at({13, 29, 1}) == 13 * 1000 + 29 * 2 + 1, "lookup");
	Check(PositionOf(a) == PositionOf(b), "the call index is not part of the position");
	Check(PositionOf(a) < PositionOf(c) && PositionOf(c) < PositionOf(d), "positions order by submission, then draw");
	Check(Position {5, UINT32_MAX} >= PositionOf({5, 99999, 3}), "end of submission covers all its draws");
}

void TestContextCallsAndNesting() {
	Key  key;
	bool nested = false;
	Check(!NextCall(key, nested), "no event, no key");
	{
		EventScope outer(10, 4, 0);
		Check(NextCall(key, nested) && key == Key {10, 4, 0} && !nested, "first call of the event");
		Check(NextCall(key, nested) && key == Key {10, 4, 1}, "second call");
		{
			EventScope inner(10, 4, 1);
			Check(NextCall(key, nested) && key == Key {10, 4, 0} && nested, "nested draw restarts the call index and is marked");
		}
		Check(NextCall(key, nested) && key == Key {10, 4, 2} && !nested, "the outer event continues after the nested one");
	}
	Check(!NextCall(key, nested), "scope end clears the event");
}

// ---- read set -------------------------------------------------------------------------------

struct FakeMemory {
	std::map<uint64_t, uint8_t> bytes; // absent = unmapped
	std::vector<uint64_t> dirty_ranges;  // [address, address + 64)
	int reads = 0;

	void SetByte(uint64_t address, uint8_t value) { bytes[address] = value; }
	void Set(uint64_t address, uint32_t value) {
		for (int i = 0; i < 4; ++i) bytes[address + i] = uint8_t(value >> (8 * i));
	}
	bool Mapped(uint64_t address) const { return bytes.count(address) != 0; }
	uint32_t Get(uint64_t address) const {
		uint32_t value = 0;
		for (int i = 0; i < 4; ++i) {
			const auto it = bytes.find(address + i);
			if (it != bytes.end()) value |= uint32_t(it->second) << (8 * i);
		}
		return value;
	}

	bool Read(ReadKind kind, uint64_t address, std::span<uint32_t> out, bool exact) {
		++reads;
		bool all = true;
		for (size_t i = 0; i < out.size(); ++i) {
			for (int b = 0; b < 4; ++b) all = all && Mapped(address + i * 4 + b);
			out[i] = Get(address + i * 4);
		}
		if (all) return true;
		if (exact) return false;
		if (kind == ReadKind::Scalar) return true; // unmapped memory reads as zeros
		return false;
	}
	bool Dirty(uint64_t address, uint64_t bytes_) const {
		for (const auto base: dirty_ranges)
			if (address < base + 64 && base < address + bytes_) return true;
		return false;
	}
	ReadSet::Result Validate(const ReadSet& set) {
		return set.Validate([&](ReadKind k, uint64_t a, std::span<uint32_t> o, bool e) { return Read(k, a, o, e); },
		                    [&](uint64_t a, uint64_t b) { return Dirty(a, b); });
	}
};

void Put(FakeMemory& memory, uint64_t address, std::initializer_list<uint32_t> values) {
	for (const auto v: values) {
		memory.Set(address, v);
		address += 4;
	}
}

void Record(ReadRecorder& rec, FakeMemory& memory, ReadKind kind, uint64_t address, size_t dwords) {
	std::vector<uint32_t> values(dwords);
	for (size_t i = 0; i < dwords; ++i) values[i] = memory.Get(address + i * 4);
	rec.Record(kind, address, values);
}

void TestRecorderDedupesAndMergesRuns() {
	FakeMemory memory;
	Put(memory, 0x1000, {1, 2, 3, 4});
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x1004, 1); // single dwords out of order, one repeated
	Record(rec, memory, ReadKind::Scalar, 0x1000, 1);
	Record(rec, memory, ReadKind::Scalar, 0x1004, 1);
	Record(rec, memory, ReadKind::Scalar, 0x1008, 2);
	const auto set = rec.Finish();
	Check(set.values.size() == 4, "the repeated address is stored once");
	Check(set.runs.size() == 1 && set.runs[0].dwords == 4, "contiguous dwords form one run");
	Check(set.windows.size() == 1 && set.windows[0].address == 0x1000 && set.windows[0].bytes == 16, "one window");
	Check(rec.Size() == 0, "Finish empties the recorder");
	Check(memory.Validate(set) == ReadSet::Result::Valid && memory.reads == 1, "valid with a single memory read");
}

void TestFirstValueOfARepeatedAddressWins() {
	ReadRecorder rec;
	const uint32_t first[] = {11}, second[] = {22};
	rec.Record(ReadKind::Scalar, 0x40, first);
	rec.Record(ReadKind::Scalar, 0x40, second);
	const auto set = rec.Finish();
	Check(set.values.size() == 1 && set.values[0] == 11, "first value wins");
}

void TestSmallGapsAreBridgedLargeOnesAreNot() {
	FakeMemory memory;
	Put(memory, 0x2000, {1, 2});
	Put(memory, 0x2020, {3});   // gap of 28 bytes: bridged
	Put(memory, 0x2200, {4});   // far away: its own window
	for (uint64_t a = 0x2008; a < 0x2020; a += 4) memory.Set(a, 0xdead0000u + uint32_t(a)); // gap bytes
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x2000, 2);
	Record(rec, memory, ReadKind::Scalar, 0x2020, 1);
	Record(rec, memory, ReadKind::Scalar, 0x2200, 1);
	const auto set = rec.Finish();
	Check(set.windows.size() == 2, "two windows");
	Check(set.windows[0].run_count == 2 && set.windows[0].bytes == 0x24, "the first window spans both nearby runs");
	Check(set.runs.size() == 3, "three runs");
	Check(memory.Validate(set) == ReadSet::Result::Valid && memory.reads == 2, "two memory reads for three runs");
	memory.Set(0x2010, 0x12345678); // a gap dword changes: not part of the plan
	Check(memory.Validate(set) == ReadSet::Result::Valid, "bytes between the runs are not compared");
	memory.Set(0x2020, 99);
	Check(memory.Validate(set) == ReadSet::Result::Changed, "a recorded dword that changed is detected");
}

void TestKindsAreNotMerged() {
	FakeMemory memory;
	Put(memory, 0x3000, {5, 6});
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x3000, 1);
	Record(rec, memory, ReadKind::Clean, 0x3004, 1);
	const auto set = rec.Finish();
	Check(set.windows.size() == 2, "a scalar and a clean read never share a window");
	Check(set.windows[0].kind != set.windows[1].kind, "both kinds present");
}

void TestLongRunsAreSplit() {
	FakeMemory memory;
	ReadRecorder rec;
	std::vector<uint32_t> values(700);
	for (size_t i = 0; i < values.size(); ++i) {
		values[i] = uint32_t(i * 7 + 1);
		memory.Set(0x10000 + i * 4, values[i]);
	}
	rec.Record(ReadKind::Scalar, 0x10000, values);
	const auto set = rec.Finish();
	Check(set.values.size() == 700, "all dwords kept");
	for (const auto& run: set.runs) Check(run.dwords <= ReadSet::kMaxRunDwords, "run within the window limit");
	for (const auto& w: set.windows) Check(w.bytes <= ReadSet::kMaxWindowBytes, "window within the limit");
	Check(memory.Validate(set) == ReadSet::Result::Valid, "valid");
	memory.Set(0x10000 + 650 * 4, memory.Get(0x10000 + 650 * 4) + 1);
	Check(memory.Validate(set) == ReadSet::Result::Changed, "a change in the last chunk is found");
}

void TestUnalignedAndOverlappingReads() {
	FakeMemory memory;
	for (uint64_t a = 0x5001; a < 0x5001 + 24; a += 1) memory.SetByte(a, uint8_t(a * 3));
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x5001, 2);  // [0x5001, 0x5009)
	Record(rec, memory, ReadKind::Scalar, 0x5003, 2);  // overlaps the first, different alignment
	const auto set = rec.Finish();
	Check(set.values.size() == 4, "distinct addresses are all kept");
	Check(memory.Validate(set) == ReadSet::Result::Valid, "unaligned reads validate");
	memory.SetByte(0x5007, uint8_t(memory.bytes[0x5007] + 1));
	Check(memory.Validate(set) == ReadSet::Result::Changed, "a change inside an overlapped dword is found");
}

void TestUnmappedMemory() {
	FakeMemory memory;
	Put(memory, 0x7000, {1, 2});
	// Scalar read of unmapped memory returned zeros when the plan was made.
	ReadRecorder rec;
	const uint32_t zeros[] = {0, 0};
	rec.Record(ReadKind::Scalar, 0x9000, zeros);
	Record(rec, memory, ReadKind::Scalar, 0x7000, 2);
	auto set = rec.Finish();
	Check(memory.Validate(set) == ReadSet::Result::Valid, "zeros from unmapped memory stay valid while it is unmapped");
	Put(memory, 0x9000, {3, 4});
	Check(memory.Validate(set) == ReadSet::Result::Changed, "the memory got mapped and holds data");

	FakeMemory other;
	ReadRecorder rec2;
	const uint32_t one[] = {7};
	rec2.Record(ReadKind::Clean, 0xa000, one);
	Check(other.Validate(rec2.Finish()) == ReadSet::Result::Unreadable, "a clean read of unmapped memory is unreadable");
}

void TestBridgedWindowOverUnmappedGapFallsBackToRuns() {
	FakeMemory memory;
	Put(memory, 0x8000, {1});
	Put(memory, 0x8020, {2}); // the 28 bytes between are unmapped
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x8000, 1);
	Record(rec, memory, ReadKind::Scalar, 0x8020, 1);
	const auto set = rec.Finish();
	Check(set.windows.size() == 1, "bridged");
	Check(memory.Validate(set) == ReadSet::Result::Valid, "the exact bridged read fails, the runs on their own succeed");
	Check(memory.reads == 3, "one failed window read plus one per run");
}

void TestGpuDirtyWindowsInvalidate() {
	FakeMemory memory;
	Put(memory, 0xb000, {1, 2});
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Clean, 0xb000, 2);
	const auto set = rec.Finish();
	Check(memory.Validate(set) == ReadSet::Result::Valid, "clean");
	memory.dirty_ranges.push_back(0xb000);
	Check(memory.Validate(set) == ReadSet::Result::GpuDirty, "GPU-dirty memory invalidates a clean read");
	ReadRecorder rec2;
	Record(rec2, memory, ReadKind::Scalar, 0xb000, 2);
	Check(memory.Validate(rec2.Finish()) == ReadSet::Result::Valid, "scalar reads do not ask about GPU-dirty memory");
}

void TestEmptyReadSet() {
	ReadRecorder rec;
	const auto   set = rec.Finish();
	FakeMemory   memory;
	Check(set.windows.empty() && memory.Validate(set) == ReadSet::Result::Valid && memory.reads == 0, "an empty set is valid");
}

// ---- plan, inputs, diff -----------------------------------------------------------------------

std::unique_ptr<Plan> MakePlan(Key key, uint32_t stage = 1, uint64_t hash = 0xabc) {
	auto plan                 = std::make_unique<Plan>();
	plan->key                 = key;
	plan->stage               = stage;
	plan->shader_hash         = hash;
	plan->shader_base         = 0x1000;
	plan->snapshot.user_data  = {1, 2, 3, 4};
	plan->snapshot.flattened_srt = {9, 8, 7};
	plan->snapshot.buffers.push_back({{1, 2, 3, 4, 0, 0, 0, 0}, 4});
	plan->snapshot.images.push_back({{5, 6, 7, 8, 9, 10, 11, 12}, 8});
	plan->snapshot.samplers.push_back({{1, 1, 1, 1, 0, 0, 0, 0}, 4});
	plan->specialization.buffers.resize(1);
	plan->specialization.images.resize(1);
	plan->workgroup_counts = {2, 3, 4};
	return plan;
}

void TestInputsMatch() {
	const auto plan = MakePlan({1, 1, 0});
	const std::vector<uint32_t> user {1, 2, 3, 4};
	const std::vector<uint32_t> groups {2, 3, 4};
	Check(InputsMatch(*plan, {user, 0x1000, groups}), "same inputs");
	const std::vector<uint32_t> other_user {1, 2, 3, 5};
	Check(!InputsMatch(*plan, {other_user, 0x1000, groups}), "user data differs");
	const std::vector<uint32_t> short_user {1, 2, 3};
	Check(!InputsMatch(*plan, {short_user, 0x1000, groups}), "user data length differs");
	Check(!InputsMatch(*plan, {user, 0x1004, groups}), "shader base differs");
	const std::vector<uint32_t> other_groups {2, 3, 5};
	Check(!InputsMatch(*plan, {user, 0x1000, other_groups}), "workgroup counts differ");
	Check(!InputsMatch(*plan, {user, 0x1000, {}}), "workgroup counts missing");
}

void TestDiffNamesTheDifferingPart() {
	const auto a = MakePlan({1, 1, 0});
	auto       b = MakePlan({1, 1, 0});
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == 0, "identical materializations");
	b->snapshot.flattened_srt[1]++;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffFlattened, "flattened SRT");
	b = MakePlan({1, 1, 0});
	b->snapshot.buffers[0].dwords[2]++;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffBuffers, "buffer descriptor");
	b = MakePlan({1, 1, 0});
	b->snapshot.images[0].dwords[7]++;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffImages, "image descriptor");
	b = MakePlan({1, 1, 0});
	b->snapshot.samplers[0].dwords[0]++;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffSamplers, "sampler");
	b = MakePlan({1, 1, 0});
	b->snapshot.user_data[0]++;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffUserData, "user data");
	b = MakePlan({1, 1, 0});
	b->snapshot.specialization_reads.push_back({0x10, 4});
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffReads, "captured reads");
	b = MakePlan({1, 1, 0});
	b->snapshot.uniform_fill.value = 3;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffFill, "uniform fill");
	b = MakePlan({1, 1, 0});
	b->specialization.buffers[0].packed_stride = 16;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == DiffSpecialization, "specialization");
	b = MakePlan({1, 1, 0});
	b->snapshot.flattened_srt[0]++;
	b->specialization.images[0].mip_count = 3;
	Check(Diff(a->snapshot, a->specialization, b->snapshot, b->specialization) == (DiffFlattened | DiffSpecialization),
	      "several parts");
}

// ---- store ------------------------------------------------------------------------------------

void TestStoreReadyTakeAndClassification() {
	Store store(100, 1u << 30);
	std::unique_ptr<Plan> plan;
	Check(store.Take({5, 1, 0}, plan) == Lookup::NotReady, "before the lookahead got there");
	store.Put(MakePlan({5, 1, 0}));
	store.Put(MakePlan({5, 1, 1}));
	store.NoteProgress({5, 1});
	Check(store.Take({5, 1, 0}, plan) == Lookup::Ready && plan != nullptr && plan->key == Key {5, 1, 0}, "plan found");
	plan.reset();
	Check(store.Take({5, 1, 0}, plan) == Lookup::Missing, "a plan is taken once");
	Check(store.Take({5, 2, 0}, plan) == Lookup::NotReady, "the next draw is not reached yet");
	store.NoteProgress({5, 2});
	Check(store.Take({5, 2, 0}, plan) == Lookup::Missing, "reached without a plan");
	store.NoteProgress({6, UINT32_MAX});
	Check(store.Take({6, 9, 0}, plan) == Lookup::Missing, "the whole submission was looked at");
	Check(store.Take({7, 1, 0}, plan) == Lookup::NotReady, "the next submission is not");
	Check(store.Take({5, 1, 1}, plan) == Lookup::Ready, "the second call of the draw");
}

void TestStoreLateSubmissions() {
	Store store(100, 1u << 30);
	std::unique_ptr<Plan> plan;
	store.NoteLate(9);
	store.NoteProgress({9, UINT32_MAX});
	Check(store.Take({9, 3, 0}, plan) == Lookup::Late, "late submission");
	Check(store.Take({10, 3, 0}, plan) != Lookup::Late, "others are not late");
}

void TestStoreEvictsOldestAndRemembersIt() {
	Store store(3, 1u << 30);
	for (uint32_t i = 1; i <= 5; ++i) store.Put(MakePlan({1, i, 0}));
	store.NoteProgress({1, 5});
	std::unique_ptr<Plan> plan;
	Check(store.Take({1, 1, 0}, plan) == Lookup::Evicted, "oldest evicted");
	Check(store.Take({1, 2, 0}, plan) == Lookup::Evicted, "second oldest evicted");
	Check(store.Take({1, 3, 0}, plan) == Lookup::Ready, "newest three kept");
	Check(store.Take({1, 5, 0}, plan) == Lookup::Ready, "newest kept");
	const auto counters = store.GetCounters();
	Check(counters.put == 5 && counters.evictions == 2, "counters");
}

void TestStoreByteLimitAndReplacement() {
	auto big = MakePlan({1, 1, 0});
	const auto size = big->Bytes();
	Store store(100, size * 2 + size / 2);
	store.Put(std::move(big));
	store.Put(MakePlan({1, 2, 0}));
	store.Put(MakePlan({1, 3, 0}));
	std::unique_ptr<Plan> plan;
	store.NoteProgress({1, 3});
	Check(store.Take({1, 1, 0}, plan) == Lookup::Evicted, "byte limit evicts");
	Check(store.Take({1, 3, 0}, plan) == Lookup::Ready, "newest stays");
	Store again(100, 1u << 30);
	again.Put(MakePlan({2, 1, 0}, 1, 0x111));
	again.Put(MakePlan({2, 1, 0}, 1, 0x222));
	again.NoteProgress({2, 1});
	Check(again.Take({2, 1, 0}, plan) == Lookup::Ready && plan->shader_hash == 0x222, "a second plan replaces the first");
	Check(again.GetCounters().queued == 0 && again.GetCounters().queued_bytes == 0, "byte accounting returns to zero");
}

// ---- statistics ---------------------------------------------------------------------------------

void TestPercentilesAndReservoir() {
	std::vector<double> v;
	for (int i = 1; i <= 101; ++i) v.push_back(i);
	const auto p = ComputePercentiles(v);
	Check(p.n == 101 && p.p10 == 11 && p.p50 == 51 && p.p90 == 91, "percentiles of 1..101");
	Check(ComputePercentiles({}).n == 0, "empty");
	Reservoir res(1000);
	for (int i = 0; i < 100000; ++i) res.Add(double(i % 1000));
	const auto q = res.Get();
	Check(q.n == 1000 && res.Seen() == 100000, "bounded sample");
	Check(q.p50 > 400 && q.p50 < 600 && q.p10 > 50 && q.p10 < 160 && q.p90 > 840 && q.p90 < 950, "uniform sample keeps the shape");
}

Sample Ready(Invalid invalid, bool equal, int64_t lead_ns, uint64_t draws) {
	Sample s;
	s.outcome    = Outcome::Ready;
	s.invalid    = invalid;
	s.equal      = equal;
	s.lead_ns    = lead_ns;
	s.lead_draws = draws;
	s.check_ns   = 500;
	s.materialize_ns = 3000;
	return s;
}

void TestStatsCountOutcomesAndIntervals() {
	Stats stats;
	for (int i = 0; i < 10; ++i) stats.Add(Ready(Invalid::None, true, (i + 1) * 1000, i + 1));
	stats.Add(Ready(Invalid::Changed, false, 2000, 3));
	stats.Add(Ready(Invalid::GpuDirty, true, 2000, 3));
	stats.Add(Ready(Invalid::None, false, 2000, 3)); // valid and unequal: the dangerous case
	Sample missing;
	missing.outcome = Outcome::Missing;
	stats.Add(missing);
	const auto record = stats.EndInterval();
	Check(record.events == 14 && record.ready == 13, "interval counts");
	Check(record.valid == 11 && record.equal == 11, "interval valid/equal");
	Check(record.lead_us.n == 13 && record.lead_us.p50 >= 2 && record.lead_us.p50 <= 6, "interval lead in microseconds");
	const auto window = stats.TakeWindow();
	const auto& t     = window.totals;
	Check(t.events == 14 && t.outcome[size_t(Outcome::Ready)] == 13 && t.outcome[size_t(Outcome::Missing)] == 1, "outcomes");
	Check(t.valid == 11 && t.valid_equal == 10 && t.valid_unequal == 1 && t.invalid_equal == 1, "valid x equal matrix");
	Check(t.invalid[size_t(Invalid::Changed)] == 1 && t.invalid[size_t(Invalid::GpuDirty)] == 1, "invalid reasons");
	Check(t.check_ns == 13 * 500 && t.materialize_ns == 13 * 3000, "costs");
	Check(stats.Total().events == 14, "window moved into the totals");
	Check(stats.TakeWindow().totals.events == 0, "window cleared");
	Check(stats.EndInterval().events == 0, "interval cleared");
}

void TestIntervalMediansNeedEnoughSamples() {
	Stats stats;
	for (int interval = 0; interval < 3; ++interval) {
		for (int i = 0; i < 20; ++i) stats.Add(Ready(Invalid::None, true, (interval + 1) * 10000, 5 * (interval + 1)));
		stats.EndInterval();
	}
	for (int i = 0; i < 3; ++i) stats.Add(Ready(Invalid::None, true, 99999999, 99)); // too short to count
	stats.EndInterval();
	const auto medians = stats.IntervalMedianLeadUs();
	Check(medians.n == 3 && medians.p50 == 20 && medians.p10 == 10 && medians.p90 == 30, "interval medians in us");
	Check(stats.Intervals().size() == 4, "all intervals kept");
}

// ---- adoption (--srt-plans) -----------------------------------------------------------------

// A plan whose read set was recorded from `memory` (a scalar window and a clean one).
std::unique_ptr<Plan> MakeReadPlan(Key key, FakeMemory& memory) {
	auto plan = MakePlan(key);
	Put(memory, 0x8000, {11, 22, 33});
	Put(memory, 0x9000, {44});
	ReadRecorder rec;
	Record(rec, memory, ReadKind::Scalar, 0x8000, 3);
	Record(rec, memory, ReadKind::Clean, 0x9000, 1);
	plan->reads = rec.Finish();
	return plan;
}

struct AdoptFixture {
	Store                       store {100, 1u << 30};
	FakeMemory                  memory;
	std::vector<uint32_t>       user {1, 2, 3, 4};
	std::vector<uint32_t>       groups {2, 3, 4};
	std::unique_ptr<Plan>       plan;

	Decision Decide(Key key, bool nested = false, uint32_t stage = 1, uint64_t hash = 0xabc) {
		plan.reset();
		return ::Libs::Graphics::SrtPlan::Decide(
		    store, key, nested, stage, hash, Inputs {user, 0x1000, groups},
		    [&](ReadKind k, uint64_t a, std::span<uint32_t> o, bool e) { return memory.Read(k, a, o, e); },
		    [&](uint64_t a, uint64_t b) { return memory.Dirty(a, b); }, plan);
	}
	void Add(Key key) {
		store.Put(MakeReadPlan(key, memory));
		store.NoteProgress({key.seq, key.ordinal});
	}
};

void TestDecideAdoptsAValidPlan() {
	AdoptFixture f;
	f.Add({3, 1, 0});
	const auto decision = f.Decide({3, 1, 0});
	Check(decision.Adopted() && decision.outcome == Outcome::Ready && decision.invalid == Invalid::None, "valid plan adopted");
	Check(f.plan != nullptr && f.plan->key == Key {3, 1, 0}, "the plan is handed over");
	Check(f.store.GetCounters().queued == 0, "and no longer in the store");
}

void TestDecideRejectsUnusablePlansAndStillReturnsThem() {
	AdoptFixture f;
	f.Add({3, 1, 0});
	auto decision = f.Decide({3, 1, 0}, false, 2); // another stage
	Check(!decision.Adopted() && decision.outcome == Outcome::Desync && f.plan != nullptr, "stage desync");
	f.Add({3, 2, 0});
	decision = f.Decide({3, 2, 0}, false, 1, 0xdef); // another shader
	Check(decision.outcome == Outcome::Desync && f.plan != nullptr, "shader desync");

	f.Add({3, 3, 0});
	f.user[0] = 99; // inputs differ
	decision = f.Decide({3, 3, 0});
	Check(!decision.Adopted() && decision.outcome == Outcome::Ready && decision.invalid == Invalid::Inputs && f.plan != nullptr,
	      "inputs differ");
	f.user[0] = 1;

	f.Add({3, 4, 0});
	f.memory.Set(0x8004, 1234); // a scalar dword the walk read changed
	decision = f.Decide({3, 4, 0});
	Check(!decision.Adopted() && decision.invalid == Invalid::Changed && f.plan != nullptr, "guest memory changed");
	f.memory.Set(0x8004, 22);

	f.Add({3, 5, 0});
	f.memory.dirty_ranges.push_back(0x9000); // the clean read is now GPU-written
	decision = f.Decide({3, 5, 0});
	Check(!decision.Adopted() && decision.invalid == Invalid::GpuDirty && f.plan != nullptr, "GPU-written clean read");
	f.memory.dirty_ranges.clear();

	f.Add({3, 6, 0});
	f.memory.bytes.erase(0x9000); // the memory a clean read covered is unmapped now
	decision = f.Decide({3, 6, 0});
	Check(!decision.Adopted() && decision.invalid == Invalid::Unreadable && f.plan != nullptr, "unreadable");
	f.Add({3, 7, 0});
	f.memory.bytes.erase(0x8000); // unmapped scalar memory reads as zeros: the changed dword shows
	decision = f.Decide({3, 7, 0});
	Check(!decision.Adopted() && decision.invalid == Invalid::Changed && f.plan != nullptr, "unmapped scalar read is a change");
}

void TestDecideWithoutAPlan() {
	AdoptFixture f;
	auto decision = f.Decide({4, 1, 0});
	Check(decision.outcome == Outcome::NotReady && !decision.Adopted() && f.plan == nullptr, "not made yet");
	f.store.NoteProgress({4, 1});
	decision = f.Decide({4, 1, 0});
	Check(decision.outcome == Outcome::Missing && f.plan == nullptr, "passed without a plan");
	f.store.NoteLate(5);
	Check(f.Decide({5, 1, 0}).outcome == Outcome::Late, "late submission");
	// A nested draw never takes a plan: the one of its packet stays for the draw that owns it.
	f.Add({6, 1, 0});
	decision = f.Decide({6, 1, 0}, true);
	Check(decision.outcome == Outcome::SubDraw && f.plan == nullptr && f.store.GetCounters().queued == 1, "nested draw");
	Check(f.Decide({6, 1, 0}).Adopted(), "the plan is still there for its draw");
}

void TestDecideAfterEviction() {
	AdoptFixture f;
	Store small(2, 1u << 30);
	// Fill a two-plan store: the oldest falls out and is reported as evicted.
	small.Put(MakeReadPlan({7, 1, 0}, f.memory));
	small.Put(MakeReadPlan({7, 2, 0}, f.memory));
	small.Put(MakeReadPlan({7, 3, 0}, f.memory));
	small.NoteProgress({7, 3});
	std::unique_ptr<Plan> plan;
	const Inputs inputs {f.user, 0x1000, f.groups};
	const auto read  = [&](ReadKind k, uint64_t a, std::span<uint32_t> o, bool e) { return f.memory.Read(k, a, o, e); };
	const auto dirty = [&](uint64_t a, uint64_t b) { return f.memory.Dirty(a, b); };
	Check(Decide(small, {7, 1, 0}, false, 1, 0xabc, inputs, read, dirty, plan).outcome == Outcome::Evicted, "evicted plan");
	Check(Decide(small, {7, 3, 0}, false, 1, 0xabc, inputs, read, dirty, plan).Adopted(), "newest plan adopted");
}

void TestAdoptIntoSwapsAndKeepsTheOldBuffers() {
	auto plan = MakePlan({1, 1, 0});
	Libs::Graphics::ShaderRecompiler::IR::ResourceSnapshot       resources;
	Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization specialization;
	resources.flattened_srt = {1, 1, 1, 1, 1};
	resources.user_data     = {5};
	specialization.buffers.resize(3);
	const auto expected_srt = plan->snapshot.flattened_srt;
	const auto expected_user = plan->snapshot.user_data;
	AdoptInto(resources, specialization, *plan);
	Check(resources.flattened_srt == expected_srt && resources.user_data == expected_user, "entry holds the plan's snapshot");
	Check(specialization.buffers.size() == 1 && specialization.images.size() == 1, "and its specialization");
	Check(plan->snapshot.flattened_srt.size() == 5 && plan->specialization.buffers.size() == 3, "the plan holds the old ones");
	Check(resources.buffers.size() == 1 && resources.images.size() == 1 && resources.samplers.size() == 1, "descriptor values moved too");
}

void TestRecycleKeepsTheStoreConsistent() {
	AdoptFixture f;
	std::vector<std::unique_ptr<Plan>> dead;
	for (int i = 0; i < 5; ++i) dead.push_back(MakePlan({9, uint32_t(i + 1), 0}));
	f.store.Recycle(std::move(dead));
	Check(dead.empty() && f.store.GetCounters().queued == 0 && f.store.GetCounters().put == 0, "recycled plans are not stored");
	f.Add({9, 1, 0});
	Check(f.store.GetCounters().queued == 1 && f.store.GetCounters().put == 1, "the next put works");
	Check(f.Decide({9, 1, 0}).Adopted(), "and is adoptable");
}

void TestAdoptTotals() {
	AdoptTotals t;
	t.Note({Outcome::Ready, Invalid::None});
	t.Note({Outcome::Ready, Invalid::Changed});
	t.Note({Outcome::NotReady, Invalid::None});
	t.Note({Outcome::SubDraw, Invalid::None});
	Check(t.events == 4 && t.adopted == 1, "events and adoptions");
	Check(t.outcome[size_t(Outcome::Ready)] == 2 && t.invalid[size_t(Invalid::Changed)] == 1 && t.invalid[size_t(Invalid::None)] == 1,
	      "plans found and why one was not used");
	t.NoteVerified(0);
	t.NoteVerified(DiffFlattened | DiffReads);
	Check(t.verified == 2 && t.mismatched == 1 && t.diff_bits[0] == 1 && t.diff_bits[5] == 1 && t.diff_bits[1] == 0, "verification");
	AdoptTotals sum;
	sum.Add(t);
	sum.Add(t);
	Check(sum.events == 8 && sum.adopted == 2 && sum.mismatched == 2 && sum.diff_bits[0] == 2, "totals add up");
}

} // namespace

int main() {
	TestKeyEqualityHashAndOrder();
	TestContextCallsAndNesting();
	TestRecorderDedupesAndMergesRuns();
	TestFirstValueOfARepeatedAddressWins();
	TestSmallGapsAreBridgedLargeOnesAreNot();
	TestKindsAreNotMerged();
	TestLongRunsAreSplit();
	TestUnalignedAndOverlappingReads();
	TestUnmappedMemory();
	TestBridgedWindowOverUnmappedGapFallsBackToRuns();
	TestGpuDirtyWindowsInvalidate();
	TestEmptyReadSet();
	TestInputsMatch();
	TestDiffNamesTheDifferingPart();
	TestStoreReadyTakeAndClassification();
	TestStoreLateSubmissions();
	TestStoreEvictsOldestAndRemembersIt();
	TestStoreByteLimitAndReplacement();
	TestPercentilesAndReservoir();
	TestStatsCountOutcomesAndIntervals();
	TestIntervalMediansNeedEnoughSamples();
	TestDecideAdoptsAValidPlan();
	TestDecideRejectsUnusablePlansAndStillReturnsThem();
	TestDecideWithoutAPlan();
	TestDecideAfterEviction();
	TestAdoptIntoSwapsAndKeepsTheOldBuffers();
	TestRecycleKeepsTheStoreConsistent();
	TestAdoptTotals();
	std::printf("SrtPlanTests: all passed\n");
	return 0;
}
