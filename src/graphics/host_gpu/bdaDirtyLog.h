#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYLOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYLOG_H_

// A bounded log of the guest ranges that became CPU-modified (or got a new buffer) since the GPU thread last looked.
//
// A shader that addresses memory through raw pointers needs every buffer current, so before each such draw
// RenderContext::PrepareBda used to walk every buffer of every mapped range (about 30 us, hundreds of draws per
// level frame) and upload the CPU-dirty pages it found. The pass finds something only in the buffers whose pages
// were dirtied since the last pass, and every event that dirties a page already goes through one place (the region
// manager's CPU-dirty transition). The log records the transitions; the pass then visits only the buffers that
// overlap a logged range. Unit tested in tests/BdaDirtyLogTests.cpp.
//
// Exactness: a page's dirty bit is set BEFORE its range is noted (release), and the consumer reads the bit AFTER it
// popped the note (acquire), so a consumer that pops a note sees the bit. A producer that finds the log full sets the
// overflow flag instead (it never blocks, it may run in a fault handler): the consumer then treats everything as
// dirty (a full pass). The flag is taken at the start of a drain, so a note made after the flag was taken is either
// still in the log or flags the next drain.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::BdaSync {

class DirtyLog {
public:
	static constexpr size_t kSlots = 1u << 13; // power of two

	DirtyLog() noexcept {
		for (size_t i = 0; i < kSlots; i++) {
			m_slots[i].sequence.store(i, std::memory_order_relaxed);
		}
	}

	// Any thread, lock-free, never blocks. Call after the dirty bits are set.
	void Note(uint64_t address, uint64_t size) noexcept {
		if (size == 0) {
			return;
		}
		auto pos = m_enqueue.load(std::memory_order_relaxed);
		for (;;) {
			Slot&      slot = m_slots[pos & (kSlots - 1)];
			const auto seq  = slot.sequence.load(std::memory_order_acquire);
			const auto diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
			if (diff == 0) {
				if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
					slot.address = address;
					slot.size    = size;
					slot.sequence.store(pos + 1, std::memory_order_release);
					return;
				}
			} else if (diff < 0) { // full
				m_overflow.store(true, std::memory_order_release);
				return;
			} else {
				pos = m_enqueue.load(std::memory_order_relaxed);
			}
		}
	}

	// The single consumer. Calls f(address, size) for every range noted before the call and returns true. Returns
	// false, and calls f for nothing, when the log overflowed since the last drain: everything must then be treated
	// as dirty (the notes that are in the log are dropped; the caller's full pass covers them).
	template <typename F>
	[[nodiscard]] bool Drain(F&& f) {
		const bool overflowed = m_overflow.exchange(false, std::memory_order_acq_rel);
		for (;;) {
			Slot&      slot = m_slots[m_dequeue & (kSlots - 1)];
			const auto seq  = slot.sequence.load(std::memory_order_acquire);
			if (static_cast<int64_t>(seq) - static_cast<int64_t>(m_dequeue + 1) < 0) {
				break; // empty
			}
			const auto address = slot.address;
			const auto size    = slot.size;
			slot.sequence.store(m_dequeue + kSlots, std::memory_order_release);
			m_dequeue++;
			if (!overflowed) {
				f(address, size);
			}
		}
		return !overflowed;
	}

	// The consumer discards the notes made so far (a full pass that starts now covers them).
	void Reset() {
		(void)Drain([](uint64_t, uint64_t) {});
	}

	// The consumer: the number of notes in the log (producers may be adding).
	[[nodiscard]] size_t Pending() const noexcept {
		return static_cast<size_t>(m_enqueue.load(std::memory_order_acquire) - m_dequeue);
	}

private:
	struct Slot {
		std::atomic<uint64_t> sequence {0};
		uint64_t              address = 0;
		uint64_t              size    = 0;
	};

	Slot                  m_slots[kSlots];
	std::atomic<uint64_t> m_enqueue {0};
	uint64_t              m_dequeue = 0; // consumer only
	std::atomic<bool>     m_overflow {false};
};

} // namespace Libs::Graphics::BdaSync

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYLOG_H_
