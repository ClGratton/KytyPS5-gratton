#ifndef KYTY_COMMON_SCALABLE_SHARED_MUTEX_H_
#define KYTY_COMMON_SCALABLE_SHARED_MUTEX_H_

// A reader-writer lock whose readers do not write to a shared cache line (--scalable-read-locks).
//
// std::shared_mutex (pthread_rwlock) keeps its reader count in one word, so every lock_shared and unlock_shared is an atomic
// read-modify-write on a line that every other reading thread also writes. The GPU thread and the four look-ahead workers all look
// up the same pipeline map and program table for every draw; measured with a sampling profile, those lookups were 4.6 % of the GPU
// thread (0.38 us per draw), almost all of it the line moving between cores.
//
// Here a reader increments a counter in its own cache line (a slot chosen once per thread) and then looks at one flag that only
// writers write. A writer sets the flag and waits until every slot is zero. With sequentially consistent operations on both sides,
// either the reader's increment is seen by the writer (it waits for the reader) or the writer's flag is seen by the reader (it backs
// off and waits), so a reader and a writer are never inside together. Readers never take a lock among themselves.
//
// The lock keeps the plain std::shared_mutex protocol as well, chosen per acquisition by the flag, so the flag can flip while the
// emulator runs: a writer takes the plain lock exclusively first and then drains the slots, which excludes readers of either kind;
// a reader of either kind releases through the same guard that took it. The lock is not recursive for readers: a thread that holds
// it shared must not take it shared again (the writer flag would make the second acquisition wait for a writer that waits for it).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace Common {

// The config field of --scalable-read-locks (set by Config::Load; read with a relaxed atomic_ref like every runtime toggle).
inline bool* g_scalable_locks_field = nullptr;
[[nodiscard]] inline bool ScalableLocksEnabled() noexcept {
	return g_scalable_locks_field != nullptr && std::atomic_ref<bool>(*g_scalable_locks_field).load(std::memory_order_relaxed);
}

class ScalableSharedMutex {
public:
	static constexpr size_t kSlots = 32;

	ScalableSharedMutex()                                      = default;
	ScalableSharedMutex(const ScalableSharedMutex&)            = delete;
	ScalableSharedMutex& operator=(const ScalableSharedMutex&) = delete;

	// Shared ownership. `held_slot` is -1 when the plain lock was taken, else the slot that counts this reader.
	[[nodiscard]] int LockShared() {
		if (!ScalableLocksEnabled()) {
			m_plain.lock_shared();
			return -1;
		}
		const size_t slot = ThreadSlot();
		for (;;) {
			m_slots[slot].readers.fetch_add(1, std::memory_order_seq_cst);
			if (m_writer.load(std::memory_order_seq_cst) == 0) {
				return static_cast<int>(slot);
			}
			m_slots[slot].readers.fetch_sub(1, std::memory_order_seq_cst);
			for (uint32_t spins = 0; m_writer.load(std::memory_order_acquire) != 0; ++spins) {
				Wait(spins);
			}
		}
	}

	void UnlockShared(int held_slot) {
		if (held_slot < 0) {
			m_plain.unlock_shared();
		} else {
			m_slots[static_cast<size_t>(held_slot)].readers.fetch_sub(1, std::memory_order_release);
		}
	}

	void Lock() {
		m_plain.lock(); // excludes plain readers and other writers
		m_writer.store(1, std::memory_order_seq_cst);
		for (auto& slot: m_slots) {
			for (uint32_t spins = 0; slot.readers.load(std::memory_order_seq_cst) != 0; ++spins) {
				Wait(spins);
			}
		}
	}

	void Unlock() {
		m_writer.store(0, std::memory_order_release);
		m_plain.unlock();
	}

	class SharedGuard {
	public:
		explicit SharedGuard(ScalableSharedMutex& mutex): m_mutex(&mutex), m_slot(mutex.LockShared()) {}
		~SharedGuard() { m_mutex->UnlockShared(m_slot); }
		SharedGuard(const SharedGuard&)            = delete;
		SharedGuard& operator=(const SharedGuard&) = delete;

	private:
		ScalableSharedMutex* m_mutex;
		int                  m_slot;
	};

	class ExclusiveGuard {
	public:
		explicit ExclusiveGuard(ScalableSharedMutex& mutex): m_mutex(&mutex) { m_mutex->Lock(); }
		~ExclusiveGuard() { m_mutex->Unlock(); }
		ExclusiveGuard(const ExclusiveGuard&)            = delete;
		ExclusiveGuard& operator=(const ExclusiveGuard&) = delete;

	private:
		ScalableSharedMutex* m_mutex;
	};

private:
	struct alignas(64) Slot {
		std::atomic<uint32_t> readers {0};
	};

	static void Pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
		_mm_pause();
#elif defined(_M_ARM64)
		__yield();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	// A short spin, then the CPU is given up, so that a thread that holds the lock is not kept off its core by the threads waiting for it.
	static void Wait(uint32_t spins) noexcept {
		if (spins < 200) {
			Pause();
		} else {
			std::this_thread::yield();
		}
	}

	// One slot per thread, handed out round-robin on first use (threads beyond kSlots share a counter, which stays correct).
	static size_t ThreadSlot() noexcept {
		static std::atomic<size_t> next {0};
		thread_local const size_t  slot = next.fetch_add(1, std::memory_order_relaxed) % kSlots;
		return slot;
	}

	std::shared_mutex     m_plain;
	alignas(64) std::atomic<uint32_t> m_writer {0};
	Slot                  m_slots[kSlots];
};

} // namespace Common

#endif // KYTY_COMMON_SCALABLE_SHARED_MUTEX_H_
