#ifndef COMMON_GUEST_RETIREMENT_H
#define COMMON_GUEST_RETIREMENT_H

// When can the guest reuse the memory a submission points at?
//
// A submitted command buffer references guest memory the CPU owns: indirect buffers, descriptor and
// SRT tables, constant memory, shader code. The guest reuses that memory once it believes the GPU
// is done with it, and it can only believe that after it has OBSERVED a completion: an end-of-pipe
// label or a release-mem write the GPU thread performed, an EOP interrupt, a flip, a suspend point.
// Until then the memory is guaranteed to hold what the submission's packets expect, no matter how
// far the GPU thread has got through the submission, or whether it started it at all. After it,
// the memory can hold anything.
//
// The look-ahead (--pipeline-lookahead) reads that memory speculatively, on another thread, while
// the GPU thread executes. It is only allowed to trust it while the submission is not retired in
// this sense. "The GPU thread has started the submission" is NOT a retirement: in --async-pipelines
// stall mode the GPU thread starts a submission long before it finishes it, the guest still holds
// the memory, and the look-ahead's scan is exactly what produces useful plans.
//
// The GPU thread publishes three facts per guest queue (submission sequences are global and
// increasing, assigned in submission order; a queue's submissions run in order):
//   NoteStarted(queue, seq)   the GPU thread began submission `seq`
//   NoteCompleted(queue, seq) the GPU thread finished it
//   NoteSignal(queue)         the GPU thread is about to make a completion visible to the guest
//                             (label write, interrupt, flip, ...). Everything the queue has started
//                             so far counts as retired from this call on: a signal in the middle of
//                             a submission is treated as covering all of it (conservative).
// NoteSignal must be called BEFORE the visible effect (it is a full barrier), so a reader that sees
// the guest recycle memory also sees the retirement.
//
// IsRetired(queue, seq): the submission's memory may have been recycled when
//   - a signal was published for its queue at or after it (seq <= signal[queue]), or
//   - the GPU thread finished it and a signal of ANY queue covers a sequence >= seq: another
//     queue's label can legitimately tell the guest that this queue's work is done (a label that
//     waits for it), and a finished submission cannot be told apart from one that was waited for.
// A started but unfinished submission with no signal in its queue is never retired.

#include <atomic>
#include <cstdint>

namespace Common {

class GuestRetirement {
public:
	static constexpr uint32_t kMaxQueues = 64;

	void NoteStarted(uint32_t queue, uint64_t seq) noexcept {
		if (seq == 0) {
			return;
		}
		RaiseMax(m_started[Clamp(queue)], seq, std::memory_order_release);
	}

	void NoteCompleted(uint32_t queue, uint64_t seq) noexcept {
		if (seq == 0) {
			return;
		}
		RaiseMax(m_completed[Clamp(queue)], seq, std::memory_order_release);
	}

	// Everything this queue has started is retired from here on. Call before the guest-visible effect.
	void NoteSignal(uint32_t queue) noexcept {
		const auto q       = Clamp(queue);
		const auto started = m_started[q].load(std::memory_order_acquire);
		m_signals.fetch_add(1, std::memory_order_relaxed);
		if (started == 0) {
			return;
		}
		RaiseMax(m_signal[q], started, std::memory_order_seq_cst);
		RaiseMax(m_signal_any, started, std::memory_order_seq_cst);
	}

	[[nodiscard]] bool IsRetired(uint32_t queue, uint64_t seq) const noexcept {
		if (seq == 0) {
			return false;
		}
		const auto q = Clamp(queue);
		if (seq <= m_signal[q].load(std::memory_order_acquire)) {
			return true;
		}
		return seq <= m_completed[q].load(std::memory_order_acquire) &&
		       seq <= m_signal_any.load(std::memory_order_acquire);
	}

	[[nodiscard]] uint64_t Started(uint32_t queue) const noexcept {
		return m_started[Clamp(queue)].load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t Completed(uint32_t queue) const noexcept {
		return m_completed[Clamp(queue)].load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t Signalled(uint32_t queue) const noexcept {
		return m_signal[Clamp(queue)].load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t SignalCalls() const noexcept {
		return m_signals.load(std::memory_order_relaxed);
	}

	// The process-wide instance (one GPU thread publishes, the look-ahead thread reads).
	[[nodiscard]] static GuestRetirement& Global() noexcept {
		static GuestRetirement instance;
		return instance;
	}

private:
	// Queue ids past the table share the last slot: more retirement, never less.
	[[nodiscard]] static constexpr uint32_t Clamp(uint32_t queue) noexcept {
		return queue < kMaxQueues ? queue : kMaxQueues - 1;
	}

	static void RaiseMax(std::atomic<uint64_t>& slot, uint64_t value, std::memory_order order) noexcept {
		auto current = slot.load(std::memory_order_relaxed);
		while (current < value) {
			if (slot.compare_exchange_weak(current, value, order, std::memory_order_relaxed)) {
				return;
			}
		}
		if (order == std::memory_order_seq_cst) {
			std::atomic_thread_fence(std::memory_order_seq_cst);
		}
	}

	std::atomic<uint64_t> m_started[kMaxQueues]   = {};
	std::atomic<uint64_t> m_completed[kMaxQueues] = {};
	std::atomic<uint64_t> m_signal[kMaxQueues]    = {};
	std::atomic<uint64_t> m_signal_any {0};
	std::atomic<uint64_t> m_signals {0};
};

} // namespace Common

#endif // COMMON_GUEST_RETIREMENT_H
