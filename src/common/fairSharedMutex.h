#ifndef KYTY_COMMON_FAIR_SHARED_MUTEX_H_
#define KYTY_COMMON_FAIR_SHARED_MUTEX_H_

// A shared mutex whose writers are not starved by a steady stream of readers.
//
// glibc's pthread rwlock (behind std::shared_mutex) prefers readers: while readers keep overlapping,
// a writer waits. With several threads that take a program entry's lock shared for a few
// microseconds each (the prefetch workers, kyGpu), the writers (a permutation being published, a
// call source refreshed) would wait for ever. Here a reader that sees a writer waiting stands aside
// until the writer has had its turn. Meets SharedMutex plus try_lock, so std::unique_lock and
// std::shared_lock work with it. Raw() exposes the underlying mutex for the speculation guard,
// which unlocks it directly when a fatal check parks a thread.

#include <atomic>
#include <shared_mutex>
#include <thread>

namespace Common {

class FairSharedMutex {
public:
	void lock() {
		m_writers.fetch_add(1, std::memory_order_acq_rel);
		m_mutex.lock();
		m_writers.fetch_sub(1, std::memory_order_acq_rel);
	}
	bool try_lock() { return m_mutex.try_lock(); }
	void unlock() { m_mutex.unlock(); }

	void lock_shared() {
		for (uint32_t spins = 0; m_writers.load(std::memory_order_acquire) != 0; ++spins) {
			if (spins > 16) std::this_thread::yield();
		}
		m_mutex.lock_shared();
	}
	bool try_lock_shared() { return m_writers.load(std::memory_order_acquire) == 0 && m_mutex.try_lock_shared(); }
	void unlock_shared() { m_mutex.unlock_shared(); }

	[[nodiscard]] std::shared_mutex* Raw() { return &m_mutex; }

private:
	std::shared_mutex     m_mutex;
	std::atomic<uint32_t> m_writers {0};
};

} // namespace Common

#endif // KYTY_COMMON_FAIR_SHARED_MUTEX_H_
