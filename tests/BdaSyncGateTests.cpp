#include "graphics/host_gpu/bdaSyncGate.h"

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace Libs::Graphics::BdaSync;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "BdaSyncGateTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFirstPassAlwaysRuns() {
	SkipGate gate;
	Check(!gate.CanSkip(true, {}), "no full pass yet: cannot skip");
	Check(!gate.CanSkip(true, {5, 5, 5}), "no full pass yet, any generations");
}

void TestSkipsWhenNothingChanged() {
	SkipGate         gate;
	const Generations g {3, 7, 2};
	gate.RecordFullPass(g);
	Check(gate.CanSkip(true, g), "same generations: skip");
	Check(gate.CanSkip(true, {3, 7, 2}), "equal value, not identity");
}

void TestAnyGenerationChangeForcesAPass() {
	SkipGate gate;
	gate.RecordFullPass({3, 7, 2});
	Check(!gate.CanSkip(true, {4, 7, 2}), "pages became CPU-modified");
	Check(!gate.CanSkip(true, {3, 8, 2}), "a buffer was registered or unregistered");
	Check(!gate.CanSkip(true, {3, 7, 3}), "the mapped ranges changed");
}

void TestDisabledNeverSkips() {
	SkipGate gate;
	gate.RecordFullPass({1, 1, 1});
	Check(!gate.CanSkip(false, {1, 1, 1}), "flag off: stock behaviour, always synchronize");
}

void TestChangeDuringPassIsNotLost() {
	// The generations are read before the pass; a change that lands during the pass makes the
	// next call see different generations and synchronize again.
	SkipGate         gate;
	const Generations before {10, 4, 1};
	gate.RecordFullPass(before);
	const Generations after_concurrent_write {11, 4, 1};
	Check(!gate.CanSkip(true, after_concurrent_write), "write during the pass is picked up");
	gate.RecordFullPass(after_concurrent_write);
	Check(gate.CanSkip(true, after_concurrent_write), "and then settles");
}

void TestInvalidate() {
	SkipGate gate;
	gate.RecordFullPass({1, 2, 3});
	gate.Invalidate();
	Check(!gate.CanSkip(true, {1, 2, 3}), "invalidated gate forces a pass");
}

void TestCpuDirtyCounterIsMonotonicAcrossThreads() {
	const auto start = g_cpu_dirty_generation.load();
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([] {
			for (int i = 0; i < 1000; ++i) {
				NoteCpuModified();
			}
		});
	}
	for (auto& thread: threads) {
		thread.join();
	}
	Check(g_cpu_dirty_generation.load() == start + 4000, "no bump lost");
}

void TestLogDrainsInOrder() {
	auto log = std::make_unique<DirtyLog>();
	std::vector<std::pair<uint64_t, uint64_t>> got;
	Check(log->Drain([&](uint64_t a, uint64_t n) { got.emplace_back(a, n); }) && got.empty(), "empty log drains clean");
	log->Note(0x1000, 0x2000);
	log->Note(0x9000, 0x1000);
	log->Note(0x5000, 0); // an empty note is dropped
	Check(log->Pending() == 2, "two notes pending");
	Check(log->Drain([&](uint64_t a, uint64_t n) { got.emplace_back(a, n); }), "no overflow");
	Check(got.size() == 2 && got[0] == std::make_pair(uint64_t {0x1000}, uint64_t {0x2000}) &&
	          got[1] == std::make_pair(uint64_t {0x9000}, uint64_t {0x1000}),
	      "notes come back in order");
	got.clear();
	Check(log->Drain([&](uint64_t a, uint64_t n) { got.emplace_back(a, n); }) && got.empty(), "drained notes are gone");
}

void TestLogPeekDoesNotConsume() {
	auto log = std::make_unique<DirtyLog>();
	log->Note(0x1000, 0x1000);
	log->Note(0x3000, 0x2000);
	std::vector<uint64_t> seen;
	Check(log->Peek([&](uint64_t a, uint64_t) { seen.push_back(a); }) && seen.size() == 2, "peek sees both notes");
	Check(log->Pending() == 2, "peek leaves them in the log");
	std::vector<uint64_t> drained;
	Check(log->Drain([&](uint64_t a, uint64_t) { drained.push_back(a); }) && drained == seen,
	      "the drain after a peek still delivers them");
	Check(log->Peek([&](uint64_t, uint64_t) { Check(false, "nothing left to peek"); }), "empty peek");
}

void TestLogOverflowForcesAFullPassAndRecovers() {
	auto log = std::make_unique<DirtyLog>();
	for (size_t i = 0; i < DirtyLog::kSlots + 10; i++) {
		log->Note(0x1000 * (i + 1), 0x1000);
	}
	size_t calls = 0;
	Check(!log->Drain([&](uint64_t, uint64_t) { calls++; }), "a full log reports the overflow");
	Check(calls == 0, "and hands out nothing (the caller walks everything)");
	Check(log->Pending() == 0, "the notes it held are dropped");
	log->Note(0x42000, 0x1000);
	std::vector<uint64_t> got;
	Check(log->Drain([&](uint64_t a, uint64_t) { got.push_back(a); }) && got.size() == 1 && got[0] == 0x42000,
	      "after the overflow the log works again");
}

void TestLogResetDropsEarlierNotes() {
	auto log = std::make_unique<DirtyLog>();
	log->Note(0x1000, 0x1000);
	log->Reset();
	size_t calls = 0;
	Check(log->Drain([&](uint64_t, uint64_t) { calls++; }) && calls == 0, "reset drops what a full pass covers");
}

void TestNoteCpuModifiedLogsOnlyWhileActive() {
	g_dirty_log_active.store(false);
	g_dirty_log.Reset();
	NoteCpuModified(0x2000, 0x1000);
	Check(g_dirty_log.Pending() == 0, "inactive: nothing logged");
	g_dirty_log_active.store(true);
	NoteCpuModified(0x2000, 0x1000);
	NoteCpuModified(); // range unknown: everything
	std::vector<std::pair<uint64_t, uint64_t>> got;
	Check(g_dirty_log.Drain([&](uint64_t a, uint64_t n) { got.emplace_back(a, n); }) && got.size() == 2 &&
	          got[0].first == 0x2000 && got[1].first == 0 && got[1].second == ~uint64_t {0},
	      "active: the range, and everything for an unknown range");
	g_dirty_log_active.store(false);
}

void TestLogManyProducersOneConsumer() {
	// Every note is delivered once, or the consumer is told to walk everything.
	auto                  log = std::make_unique<DirtyLog>();
	constexpr int         kProducers = 4, kNotes = 20000;
	std::atomic<bool>     done {false};
	std::atomic<uint64_t> delivered {0};
	std::atomic<uint64_t> overflows {0};
	std::thread           consumer([&] {
        for (;;) {
            const bool finished = done.load();
            if (!log->Drain([&](uint64_t, uint64_t) { delivered.fetch_add(1); })) {
                overflows.fetch_add(1);
            }
            if (finished) {
                break;
            }
        }
    });
	std::vector<std::thread> producers;
	for (int t = 0; t < kProducers; ++t) {
		producers.emplace_back([&] {
			for (int i = 0; i < kNotes; ++i) {
				log->Note(0x1000 + uint64_t(i) * 0x1000, 0x1000);
			}
		});
	}
	for (auto& p: producers) {
		p.join();
	}
	done.store(true);
	consumer.join();
	Check(overflows.load() > 0 || delivered.load() == uint64_t {kProducers} * kNotes,
	      "all notes arrive unless an overflow told the consumer to walk everything");
	Check(delivered.load() <= uint64_t {kProducers} * kNotes, "no note is delivered twice");
}

} // namespace

int main() {
	TestLogDrainsInOrder();
	TestLogPeekDoesNotConsume();
	TestLogOverflowForcesAFullPassAndRecovers();
	TestLogResetDropsEarlierNotes();
	TestNoteCpuModifiedLogsOnlyWhileActive();
	TestLogManyProducersOneConsumer();
	TestFirstPassAlwaysRuns();
	TestSkipsWhenNothingChanged();
	TestAnyGenerationChangeForcesAPass();
	TestDisabledNeverSkips();
	TestChangeDuringPassIsNotLost();
	TestInvalidate();
	TestCpuDirtyCounterIsMonotonicAcrossThreads();
	std::printf("BdaSyncGateTests: all passed\n");
	return 0;
}
