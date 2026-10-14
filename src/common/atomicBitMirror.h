#ifndef EMULATOR_SRC_COMMON_ATOMICBITMIRROR_H_
#define EMULATOR_SRC_COMMON_ATOMICBITMIRROR_H_

// A lock-free, read-only view of a BitArray that one writer (serialised by the caller's lock) keeps
// up to date. The writer calls Publish(bits, start, end) after every change of bits [start, end);
// readers call Any(start, end) without any lock and get the answer BitArray::FirstRangeFrom would
// give, except that the words of a range are read one by one, not as one snapshot.
//
// Why that is enough for the memory tracker (--tracker-lockfree): the writer publishes in
// ascending word order, and a reader that finds every word of its range clear read the first word
// before the writer's change reached it, hence before the change reached any later word: the
// answer is that of a point in time before the change (as if the reader had taken the lock first).
// A reader that sees a set bit answers "modified", which a locked reader could also have seen
// (the change is already in effect). Unit test: tests/AtomicBitMirrorTests.cpp.

#include "common/bitArray.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Common {

template <size_t N>
class AtomicBitMirror final {
	using Bits = BitArray<N>;
	static constexpr size_t BITS_PER_WORD = 64;

public:
	// Not thread safe: construction time only.
	void Reset(const Bits& bits) {
		for (size_t word = 0; word < Bits::kWordCount; word++) {
			m_words[word].store(bits.Word(word), std::memory_order_relaxed);
		}
	}

	// Writer side (caller holds the lock that serialises changes of `bits`): copy the words that
	// hold bits [start, end) of `bits`. Ascending word order.
	void Publish(const Bits& bits, size_t start, size_t end) {
		if (start >= end || end > N) {
			return;
		}
		const auto last = (end - 1) / BITS_PER_WORD;
		for (auto word = start / BITS_PER_WORD; word <= last; word++) {
			m_words[word].store(bits.Word(word), std::memory_order_release);
		}
	}

	// Reader side, lock free: true when a bit of [start, end) is set.
	[[nodiscard]] bool Any(size_t start, size_t end) const {
		if (start >= end || end > N) {
			return false;
		}
		const auto first_word = start / BITS_PER_WORD;
		const auto last_word  = (end - 1) / BITS_PER_WORD;
		const auto start_bit  = start % BITS_PER_WORD;
		const auto end_bit    = (end - 1) % BITS_PER_WORD;
		const auto start_mask = ~uint64_t {0} << start_bit;
		const auto end_mask =
		    end_bit == BITS_PER_WORD - 1 ? ~uint64_t {0} : (uint64_t {1} << (end_bit + 1)) - 1;
		if (first_word == last_word) {
			return (m_words[first_word].load(std::memory_order_acquire) & start_mask & end_mask) != 0;
		}
		if ((m_words[first_word].load(std::memory_order_acquire) & start_mask) != 0) {
			return true;
		}
		for (auto word = first_word + 1; word < last_word; word++) {
			if (m_words[word].load(std::memory_order_acquire) != 0) {
				return true;
			}
		}
		return (m_words[last_word].load(std::memory_order_acquire) & end_mask) != 0;
	}

	// Verifier (caller holds the writer lock): the mirror equals `bits` word for word.
	[[nodiscard]] bool Matches(const Bits& bits) const {
		for (size_t word = 0; word < Bits::kWordCount; word++) {
			if (m_words[word].load(std::memory_order_relaxed) != bits.Word(word)) {
				return false;
			}
		}
		return true;
	}

private:
	std::atomic<uint64_t> m_words[Bits::kWordCount] {};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace Common

#endif // EMULATOR_SRC_COMMON_ATOMICBITMIRROR_H_
