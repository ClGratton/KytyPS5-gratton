#include "common/finishedPrefix.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FinishedPrefixTests: failed: %s\n", message);
		std::abort();
	}
}

void TestOrder() {
	Common::FinishedPrefix<16> prefix;
	Check(prefix.Prefix() == 0, "starts empty");
	prefix.Mark(1);
	Check(prefix.Prefix() == 1, "first job");
	prefix.Mark(3);
	Check(prefix.Prefix() == 1, "a gap holds the prefix");
	prefix.Mark(4);
	Check(prefix.Prefix() == 1, "still a gap");
	prefix.Mark(2);
	Check(prefix.Prefix() == 4, "the last job of the gap releases the run behind it");
	for (uint64_t i = 5; i <= 100; ++i) prefix.Mark(i); // laps the ring several times
	Check(prefix.Prefix() == 100, "the ring wraps");
}

// Jobs 1..N are finished by several threads in a random order; the prefix must reach N (no mark may be lost to the
// store-then-look race between neighbours) and must never pass a job that is not finished.
void TestThreads(int threads, uint64_t jobs) {
	Common::FinishedPrefix<256>  prefix;
	std::vector<std::atomic<bool>> finished(jobs + 1);
	for (auto& f: finished) f.store(false);
	std::atomic<uint64_t> next {1};
	std::atomic<bool>     bad {false};
	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t) {
		workers.emplace_back([&, t] {
			std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
			for (;;) {
				const uint64_t job = next.fetch_add(1);
				if (job > jobs) return;
				// the producer keeps fewer than 256 jobs ahead of the prefix
				while (job - prefix.Prefix() >= 255) std::this_thread::yield();
				if (rng() % 4 == 0) std::this_thread::yield();
				finished[job].store(true, std::memory_order_seq_cst);
				prefix.Mark(job);
				const auto p = prefix.Prefix();
				for (uint64_t i = p > 3 ? p - 3 : 1; i <= p; ++i) {
					if (!finished[i].load(std::memory_order_seq_cst)) bad.store(true);
				}
			}
		});
	}
	for (auto& worker: workers) worker.join();
	Check(!bad.load(), "the prefix never passes an unfinished job");
	Check(prefix.Prefix() == jobs, "every job is in the prefix at the end");
}

} // namespace

int main() {
	TestOrder();
	for (int round = 0; round < 20; ++round) {
		TestThreads(2, 20000);
		TestThreads(7, 20000);
	}
	std::printf("FinishedPrefixTests: ok\n");
	return 0;
}
