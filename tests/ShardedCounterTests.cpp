#include "common/shardedCounter.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ShardedCounterTests: failed: %s\n", message);
		std::abort();
	}
}

void TestSingleThread() {
	Common::ShardedCounter counter;
	Check(counter.load() == 0, "starts at zero");
	counter.fetch_add(1);
	counter.fetch_add(41, std::memory_order_relaxed);
	Check(counter.load(std::memory_order_relaxed) == 42, "sum of adds");
}

void TestIndependentCounters() {
	Common::ShardedCounter a;
	Common::ShardedCounter b;
	a.fetch_add(5);
	b.fetch_add(7);
	Check(a.load() == 5 && b.load() == 7, "counters do not share a cell");
}

// Many threads add; the total is exact after they join, including the counts of threads that have already exited.
void TestThreadsExact() {
	Common::ShardedCounter counter;
	constexpr int          kThreads = 16;
	constexpr int          kPerThread = 100000;
	std::vector<std::thread> workers;
	for (int t = 0; t < kThreads; ++t) {
		workers.emplace_back([&] {
			for (int i = 0; i < kPerThread; ++i) counter.fetch_add(1);
		});
	}
	for (auto& worker: workers) worker.join();
	Check(counter.load() == static_cast<uint64_t>(kThreads) * kPerThread, "no add is lost, exited threads' counts are kept");
	// A new round of threads (new blocks) adds on top.
	std::vector<std::thread> second;
	for (int t = 0; t < 4; ++t) second.emplace_back([&] { counter.fetch_add(10); });
	for (auto& worker: second) worker.join();
	Check(counter.load() == static_cast<uint64_t>(kThreads) * kPerThread + 40, "later threads add to the same counter");
}

// A reader running while the writers add only ever sees a value that does not go down and ends at the exact total.
void TestConcurrentReader() {
	Common::ShardedCounter counter;
	std::atomic<bool>      stop {false};
	std::atomic<bool>      monotonic {true};
	std::thread            reader([&] {
        uint64_t last = 0;
        while (!stop.load()) {
            const uint64_t now = counter.load();
            if (now < last) monotonic.store(false);
            last = now;
        }
    });
	std::vector<std::thread> workers;
	for (int t = 0; t < 4; ++t) {
		workers.emplace_back([&] {
			for (int i = 0; i < 200000; ++i) counter.fetch_add(1);
		});
	}
	for (auto& worker: workers) worker.join();
	stop.store(true);
	reader.join();
	Check(monotonic.load(), "the sum never goes down");
	Check(counter.load() == 800000, "exact total after the writers stop");
}

// More counters than block cells: the extra ones use a shared atomic and stay exact.
void TestOverflowCounters() {
	std::vector<std::unique_ptr<Common::ShardedCounter>> counters;
	for (int i = 0; i < 300; ++i) counters.push_back(std::make_unique<Common::ShardedCounter>());
	std::vector<std::thread> workers;
	for (int t = 0; t < 4; ++t) {
		workers.emplace_back([&] {
			for (auto& c: counters) {
				for (int i = 0; i < 100; ++i) c->fetch_add(1);
			}
		});
	}
	for (auto& worker: workers) worker.join();
	for (size_t i = 0; i < counters.size(); ++i) {
		Check(counters[i]->load() == 400, "every counter, with or without a cell, adds up");
	}
}

} // namespace

int main() {
	TestSingleThread();
	TestIndependentCounters();
	TestThreadsExact();
	TestConcurrentReader();
	TestOverflowCounters();
	std::puts("ShardedCounterTests: OK");
	return 0;
}
