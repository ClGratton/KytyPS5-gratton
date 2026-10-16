#include "graphics/host_gpu/renderer/pipeline/driverCacheCheckpoint.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

using Libs::Graphics::DriverCacheCheckpointer;
using namespace std::chrono_literals;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DriverCacheCheckpointTests: failed: %s\n", message);
		std::abort();
	}
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate, std::chrono::milliseconds limit = 5000ms) {
	const auto end = std::chrono::steady_clock::now() + limit;
	while (std::chrono::steady_clock::now() < end) {
		if (predicate()) return true;
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

// Nothing new, nothing written: the thread costs one counter read per period.
void TestNoWriteWithoutNewPipelines() {
	std::atomic<uint64_t> created {0};
	std::atomic<uint32_t> writes {0};
	DriverCacheCheckpointer checkpoint(5ms, [&] { return created.load(); }, [&] {
		writes.fetch_add(1);
		return true;
	});
	std::this_thread::sleep_for(60ms);
	Check(writes.load() == 0 && checkpoint.Writes() == 0, "wrote a cache that had no new pipeline");
}

// Pipelines created before the thread started belong to the file the cache was loaded from.
void TestExistingPipelinesAreNotNew() {
	std::atomic<uint64_t> created {7};
	std::atomic<uint32_t> writes {0};
	DriverCacheCheckpointer checkpoint(5ms, [&] { return created.load(); }, [&] {
		writes.fetch_add(1);
		return true;
	});
	std::this_thread::sleep_for(60ms);
	Check(writes.load() == 0, "wrote for pipelines that existed when the thread started");
}

// One write per change, however many periods pass between changes.
void TestWritesOncePerChange() {
	std::atomic<uint64_t> created {0};
	std::atomic<uint32_t> writes {0};
	DriverCacheCheckpointer checkpoint(5ms, [&] { return created.load(); }, [&] {
		writes.fetch_add(1);
		return true;
	});
	created.fetch_add(3);
	Check(WaitFor([&] { return writes.load() == 1; }), "no write after pipelines were created");
	std::this_thread::sleep_for(60ms);
	Check(writes.load() == 1, "wrote again with no new pipeline");
	created.fetch_add(1);
	Check(WaitFor([&] { return writes.load() == 2; }), "no second write after another pipeline");
	Check(checkpoint.Writes() == 2 && checkpoint.Failures() == 0, "write count is wrong");
}

// The reason for the thread: pipeline creation goes on while a (slow) write runs. The write waits
// until the creating thread has made progress during it, which it could not if creation had to wait
// for the write (as when the GPU thread wrote the file itself).
void TestCreationContinuesDuringAWrite() {
	std::atomic<uint64_t> created {0};
	std::atomic<bool>     creating {true};
	std::atomic<bool>     progress_during_write {false};
	std::thread           creator([&] {
		while (creating.load()) {
			created.fetch_add(1);
			std::this_thread::sleep_for(1ms);
		}
	});
	DriverCacheCheckpointer checkpoint(5ms, [&] { return created.load(); }, [&] {
		const auto at_start = created.load();
		WaitFor([&] { return created.load() >= at_start + 10 || !creating.load(); });
		if (created.load() >= at_start + 10) progress_during_write.store(true);
		return true;
	});
	Check(WaitFor([&] { return progress_during_write.load(); }),
	      "pipeline creation stalled while the cache was written");
	creating.store(false);
	creator.join();
	Check(checkpoint.Writes() >= 1 || progress_during_write.load(), "no write while pipelines were created");
}

// A failed write is not a checkpoint: the next period tries again, and counts the failure.
void TestFailedWriteIsRetried() {
	std::atomic<uint64_t> created {0};
	std::atomic<uint32_t> attempts {0};
	DriverCacheCheckpointer checkpoint(5ms, [&] { return created.load(); }, [&] {
		return attempts.fetch_add(1) >= 2;
	});
	created.fetch_add(1);
	Check(WaitFor([&] { return checkpoint.Writes() == 1; }), "a failed write was not retried");
	Check(checkpoint.Failures() == 2 && attempts.load() == 3, "failures were not counted");
	std::this_thread::sleep_for(40ms);
	Check(attempts.load() == 3, "kept writing after the write succeeded");
}

// Stop() ends a long wait at once and nothing writes after it returns.
void TestStopIsPromptAndFinal() {
	std::atomic<uint64_t> created {0};
	std::atomic<uint32_t> writes {0};
	DriverCacheCheckpointer idle(std::chrono::hours {1}, [&] { return created.load(); }, [&] {
		writes.fetch_add(1);
		return true;
	});
	const auto begin = std::chrono::steady_clock::now();
	idle.Stop();
	Check(std::chrono::steady_clock::now() - begin < 2s, "Stop() waited for the period to pass");
	created.fetch_add(1);
	std::this_thread::sleep_for(20ms);
	Check(writes.load() == 0, "wrote after Stop()");

	DriverCacheCheckpointer busy(2ms, [&] { return created.load(); }, [&] {
		writes.fetch_add(1);
		return true;
	});
	created.fetch_add(1);
	Check(WaitFor([&] { return writes.load() == 1; }), "no write before Stop()");
	busy.Stop();
	const auto after_stop = writes.load();
	created.fetch_add(1);
	std::this_thread::sleep_for(30ms);
	Check(writes.load() == after_stop, "wrote after Stop() returned");
	busy.Stop(); // idempotent
}

} // namespace

int main() {
	TestNoWriteWithoutNewPipelines();
	TestExistingPipelinesAreNotNew();
	TestWritesOncePerChange();
	TestCreationContinuesDuringAWrite();
	TestFailedWriteIsRetried();
	TestStopIsPromptAndFinal();
	std::puts("DriverCacheCheckpointTests: all cases passed");
	return 0;
}
