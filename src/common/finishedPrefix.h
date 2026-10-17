#ifndef KYTY_COMMON_FINISHED_PREFIX_H_
#define KYTY_COMMON_FINISHED_PREFIX_H_

// The longest prefix 1..n of a numbered job queue whose jobs are all finished, when several threads finish jobs in any order.
//
// Mark(seq) says that job `seq` (1, 2, 3...) is finished; Prefix() is the largest n with 1..n all marked. Whichever thread marks the
// last job of a gap advances the prefix over the whole run behind it, so no thread waits for another. The marks live in a ring of
// kRing words: the producer must keep fewer than kRing jobs between Prefix() and the newest queued number.
//
// The marks and the prefix are sequentially consistent: two threads that finish neighbours each store their mark and then look at the
// other's, and with weaker ordering both could miss it and leave the prefix behind until some later job finishes.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Common {

template <size_t kRing>
class FinishedPrefix {
public:
	void Mark(uint64_t seq) noexcept {
		m_marks[seq % kRing].store(seq, std::memory_order_seq_cst);
		auto prefix = m_prefix.load(std::memory_order_seq_cst);
		while (m_marks[(prefix + 1) % kRing].load(std::memory_order_seq_cst) == prefix + 1) {
			if (m_prefix.compare_exchange_weak(prefix, prefix + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
				++prefix;
			}
		}
	}

	[[nodiscard]] uint64_t Prefix() const noexcept { return m_prefix.load(std::memory_order_acquire); }

private:
	std::atomic<uint64_t> m_prefix {0};
	std::atomic<uint64_t> m_marks[kRing] = {};
};

} // namespace Common

#endif // KYTY_COMMON_FINISHED_PREFIX_H_
