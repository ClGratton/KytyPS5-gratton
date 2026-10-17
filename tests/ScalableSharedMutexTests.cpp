#include "common/scalableSharedMutex.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ScalableSharedMutexTests: failed: %s\n", message);
		std::abort();
	}
}

// Writers move a pair of values apart and back together while holding the lock; a reader that sees them differ was inside
// with a writer. `flip` toggles the protocol (the runtime flag) while everything runs.
void Hammer(bool enabled, bool flip, int readers, int writers, int write_iterations) {
	bool field = enabled;
	Common::g_scalable_locks_field = &field;
	Common::ScalableSharedMutex mutex;
	uint64_t                    a = 0;
	uint64_t                    b = 0;
	std::atomic<bool>           stop {false};
	std::atomic<uint64_t>       reads {0};
	std::vector<std::thread>    threads;
	for (int r = 0; r < readers; ++r) {
		threads.emplace_back([&] {
			uint64_t local = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				{
					Common::ScalableSharedMutex::SharedGuard guard(mutex);
					const uint64_t x = a;
					std::atomic_signal_fence(std::memory_order_seq_cst);
					const uint64_t y = b;
					Check(x == y, "a reader never sees a writer's half-done update");
					++local;
				}
				std::this_thread::yield(); // std::shared_mutex lets readers starve a writer that never finds a gap
			}
			reads.fetch_add(local);
		});
	}
	std::vector<std::thread> writer_threads;
	for (int w = 0; w < writers; ++w) {
		writer_threads.emplace_back([&] {
			for (int i = 0; i < write_iterations; ++i) {
				Common::ScalableSharedMutex::ExclusiveGuard guard(mutex);
				++a;
				for (int k = 0; k < 50; ++k) {
					std::atomic_signal_fence(std::memory_order_seq_cst);
				}
				++b;
			}
		});
	}
	std::thread flipper;
	std::atomic<bool> flipping {flip};
	if (flip) {
		flipper = std::thread([&] {
			while (flipping.load()) {
				std::atomic_ref<bool>(field).store(!std::atomic_ref<bool>(field).load(std::memory_order_relaxed), std::memory_order_relaxed);
				std::this_thread::sleep_for(std::chrono::microseconds(50));
			}
		});
	}
	for (auto& writer: writer_threads) writer.join();
	stop.store(true);
	for (auto& reader: threads) reader.join();
	flipping.store(false);
	if (flipper.joinable()) flipper.join();
	Check(a == b && a == static_cast<uint64_t>(writers) * static_cast<uint64_t>(write_iterations), "no write is lost");
	Check(reads.load() > 0, "the readers ran");
	Common::g_scalable_locks_field = nullptr;
}

// A reader waits while a writer holds the lock and goes on when it is released; a writer waits for a reader inside.
void TestBlocking(bool enabled) {
	bool field = enabled;
	Common::g_scalable_locks_field = &field;
	Common::ScalableSharedMutex mutex;
	std::atomic<bool>           got {false};
	std::atomic<bool>           wrote {false};
	{
		std::optional<Common::ScalableSharedMutex::ExclusiveGuard> writer;
		writer.emplace(mutex);
		std::thread reader([&] {
			Common::ScalableSharedMutex::SharedGuard guard(mutex);
			got.store(true);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		Check(!got.load(), "a reader waits for the writer");
		writer.reset();
		reader.join();
		Check(got.load(), "the reader got in after the writer left");
	}
	{
		std::optional<Common::ScalableSharedMutex::SharedGuard> reader;
		reader.emplace(mutex);
		std::thread writer([&] {
			Common::ScalableSharedMutex::ExclusiveGuard guard(mutex);
			wrote.store(true);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		Check(!wrote.load(), "a writer waits for the reader inside");
		reader.reset();
		writer.join();
		Check(wrote.load(), "the writer got in after the reader left");
	}
	Common::g_scalable_locks_field = nullptr;
}

} // namespace

int main() {
	TestBlocking(false);
	TestBlocking(true);
	Hammer(false, false, 6, 2, 20000);
	Hammer(true, false, 6, 2, 20000);
	Hammer(true, true, 6, 2, 20000);
	Hammer(true, false, 40, 1, 2000); // more threads than slots share counters
	std::printf("ScalableSharedMutexTests: ok\n");
	return 0;
}
