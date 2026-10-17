#include "common/adaptiveLock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AdaptiveLockTests: failed: %s\n", message);
		std::abort();
	}
}

// Many threads add to a plain counter under the lock; any lost update, or a deadlock (the test never ends), is a failure.
void Hammer(bool sleeping, int threads, int iterations, int hold_spins) {
	bool field = sleeping;
	Common::g_sleeping_locks_field = &field;
	Common::AdaptiveLock lock;
	uint64_t             counter = 0;
	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t) {
		workers.emplace_back([&] {
			for (int i = 0; i < iterations; ++i) {
				lock.Lock();
				const auto before = counter;
				for (int k = 0; k < hold_spins; ++k) {
					std::atomic_signal_fence(std::memory_order_seq_cst);
				}
				counter = before + 1;
				lock.Unlock();
			}
		});
	}
	for (auto& worker: workers) worker.join();
	Check(counter == static_cast<uint64_t>(threads) * static_cast<uint64_t>(iterations), "no update is lost");
	Common::g_sleeping_locks_field = nullptr;
}

void TestTryLock() {
	Common::AdaptiveLock lock;
	Check(lock.TryLock(), "free lock is taken");
	Check(!lock.TryLock(), "held lock is not taken twice");
	lock.Unlock();
	Check(lock.TryLock(), "released lock is taken again");
	lock.Unlock();
}

// A waiter that sleeps is woken by the unlock (no lost wake-up), also when the holder keeps the lock for a long time.
void TestSleeperWakes() {
	bool field = true;
	Common::g_sleeping_locks_field = &field;
	Common::AdaptiveLock lock;
	lock.Lock();
	std::atomic<bool> got {false};
	std::thread       waiter([&] {
        lock.Lock();
        got.store(true);
        lock.Unlock();
    });
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // far longer than the spin: the waiter sleeps
	Check(!got.load(), "the waiter waits while the lock is held");
	lock.Unlock();
	waiter.join();
	Check(got.load(), "the unlock woke the sleeper");
	Common::g_sleeping_locks_field = nullptr;
}

// A spin-only thread (the GPU thread) and sleeping threads mix on one lock.
void TestMixedWaiters() {
	bool field = true;
	Common::g_sleeping_locks_field = &field;
	Common::AdaptiveLock lock;
	uint64_t             counter = 0;
	std::vector<std::thread> workers;
	for (int t = 0; t < 8; ++t) {
		workers.emplace_back([&, t] {
			Common::t_spin_only_thread = (t % 4 == 0);
			for (int i = 0; i < 20000; ++i) {
				lock.Lock();
				const auto before = counter;
				if (i % 997 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200)); // a long hold
				counter = before + 1;
				lock.Unlock();
			}
		});
	}
	for (auto& worker: workers) worker.join();
	Check(counter == 8u * 20000u, "mixed waiters lose nothing");
	Common::g_sleeping_locks_field = nullptr;
}

} // namespace

int main() {
	TestTryLock();
	Hammer(false, 6, 200000, 0);
	Hammer(true, 6, 200000, 0);
	Hammer(true, 12, 5000, 2000);
	TestSleeperWakes();
	TestMixedWaiters();
	std::printf("AdaptiveLockTests: ok\n");
	return 0;
}
