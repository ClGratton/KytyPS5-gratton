#ifndef KYTY_COMMON_ADAPTIVE_LOCK_H_
#define KYTY_COMMON_ADAPTIVE_LOCK_H_

// A test-and-set lock whose waiters can sleep (--tracker-sleeping-locks).
//
// The tracker's locks (RegionManager::lock, the page manager's region lock) are held across the mprotect syscall of a
// protection change, which takes microseconds to tens of microseconds under the process-wide mmap lock. With ten game
// threads faulting in the same few regions (measured: 90 to 110 thousand write faults a second on 16 thousand pages,
// 5 cores' worth of handler time) every waiter spun on _mm_pause for the whole syscall and the machine ran out of cores.
//
// State: 0 free, 1 locked, 2 locked with sleeping waiters. A waiter that may sleep spins a little and then waits on the
// word (std::atomic::wait: futex on Linux, WaitOnAddress on Windows); the holder's unlock wakes one waiter only when the
// state says there is one. A waiter that must not sleep (the GPU thread, which wants the lowest latency) or any waiter
// while the flag is off spins exactly as before and never sets state 2, so the two kinds mix on one lock.

#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace Common {

// The GPU thread (and any thread that must never sleep on a tracker lock) sets this once.
inline thread_local bool t_spin_only_thread = false;

// The config field of --tracker-sleeping-locks (set by Config::Load; read with a relaxed atomic_ref like every runtime toggle).
inline bool* g_sleeping_locks_field = nullptr;
[[nodiscard]] inline bool SleepingLocksEnabled() noexcept {
	return g_sleeping_locks_field != nullptr && std::atomic_ref<bool>(*g_sleeping_locks_field).load(std::memory_order_relaxed);
}

class AdaptiveLock {
public:
	static constexpr int kSpinIterations = 256; // about 5 to 10 us of _mm_pause before a waiter sleeps

	[[nodiscard]] bool TryLock() noexcept {
		uint32_t expected = 0;
		return m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void Lock() noexcept {
		if (!TryLock()) {
			LockSlow();
		}
	}

	void Unlock() noexcept {
		if (m_state.exchange(0, std::memory_order_release) == 2) {
			m_state.notify_one();
		}
	}

private:
	static void Pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
		_mm_pause();
#elif defined(_M_ARM64)
		__yield();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	void LockSlow() noexcept {
		const bool may_sleep = !t_spin_only_thread && SleepingLocksEnabled();
		if (!may_sleep) {
			for (;;) {
				if (m_state.load(std::memory_order_relaxed) == 0 && TryLock()) {
					return;
				}
				Pause();
			}
		}
		for (int i = 0; i < kSpinIterations; ++i) {
			if (m_state.load(std::memory_order_relaxed) == 0 && TryLock()) {
				return;
			}
			Pause();
		}
		// Announce a sleeper; whoever finds state 0 here owns the lock (and leaves 2 behind: its unlock then
		// wakes one more waiter, which is harmless).
		while (m_state.exchange(2, std::memory_order_acquire) != 0) {
			m_state.wait(2, std::memory_order_relaxed);
		}
	}

	std::atomic<uint32_t> m_state {0};
};

} // namespace Common

#endif // KYTY_COMMON_ADAPTIVE_LOCK_H_
