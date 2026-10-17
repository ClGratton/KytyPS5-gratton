#ifndef KYTY_COMMON_SHARDED_COUNTER_H_
#define KYTY_COMMON_SHARDED_COUNTER_H_

// A statistics counter that costs no locked instruction.
//
// std::atomic::fetch_add is a `lock add`. A locked instruction waits until every store the core still has in flight has drained, so
// on the GPU thread (which writes a lot of cache-missing memory per draw) each one stalls for up to a few hundred nanoseconds, and the
// draw path executed several of them only to feed a line of the periodic report.
//
// Here every thread adds to its own cache-line aligned block with a plain load and store (only the owning thread ever writes it), and
// load() sums the blocks of all threads that ever counted. The result is exact once the writers have stopped, and a reader running
// concurrently sees each block's latest stored value (like a relaxed atomic read of one counter). Blocks are never freed, so the counts
// of a thread that has exited are kept.
//
// Interface-compatible with the parts of std::atomic<uint64_t> the report code used (fetch_add, load). Not for counters that decide
// anything (use std::atomic): the sum is not a single atomic snapshot.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Common {

namespace sharded_counter_detail {

inline constexpr size_t kCapacity = 128; // counters with a block slot; later ones share one atomic (still exact, just locked)

struct alignas(64) Block {
	std::atomic<uint64_t> value[kCapacity];
	Block*                next = nullptr;
};

inline std::atomic<Block*>   g_blocks {nullptr};
inline std::atomic<uint32_t> g_next_id {0};
inline thread_local Block*   t_block = nullptr;

[[nodiscard]] inline Block* Register() {
	auto* block = new Block(); // value-initialized: every counter starts at zero
	block->next = g_blocks.load(std::memory_order_relaxed);
	while (!g_blocks.compare_exchange_weak(block->next, block, std::memory_order_release, std::memory_order_relaxed)) {
	}
	return block;
}

} // namespace sharded_counter_detail

class ShardedCounter {
public:
	ShardedCounter(): m_id(sharded_counter_detail::g_next_id.fetch_add(1, std::memory_order_relaxed)) {}

	ShardedCounter(const ShardedCounter&)            = delete;
	ShardedCounter& operator=(const ShardedCounter&) = delete;

	// The order argument exists for source compatibility with std::atomic; the add is always relaxed.
	void fetch_add(uint64_t n, std::memory_order = std::memory_order_relaxed) noexcept {
		if (m_id >= sharded_counter_detail::kCapacity) {
			m_overflow.fetch_add(n, std::memory_order_relaxed);
			return;
		}
		auto* block = sharded_counter_detail::t_block;
		if (block == nullptr) [[unlikely]] {
			block = sharded_counter_detail::t_block = sharded_counter_detail::Register();
		}
		auto& cell = block->value[m_id];
		cell.store(cell.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
	}

	[[nodiscard]] uint64_t load(std::memory_order = std::memory_order_relaxed) const noexcept {
		if (m_id >= sharded_counter_detail::kCapacity) {
			return m_overflow.load(std::memory_order_relaxed);
		}
		uint64_t sum = 0;
		for (const auto* block = sharded_counter_detail::g_blocks.load(std::memory_order_acquire); block != nullptr; block = block->next) {
			sum += block->value[m_id].load(std::memory_order_relaxed);
		}
		return sum;
	}

private:
	uint32_t              m_id;
	std::atomic<uint64_t> m_overflow {0};
};

} // namespace Common

#endif // KYTY_COMMON_SHARDED_COUNTER_H_
