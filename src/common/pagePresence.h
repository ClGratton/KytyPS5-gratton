#ifndef EMULATOR_SRC_COMMON_PAGEPRESENCE_H_
#define EMULATOR_SRC_COMMON_PAGEPRESENCE_H_

// Per-page reference counts of a set of address ranges, readable from any thread without a lock.
//
// The texture cache answers "does any image cover this page" under its lock, by a page-table walk. Most of the
// questions asked (a clean read of shader metadata, a guest write fault on a streaming buffer) have the answer no.
// This counts, per 4 KiB page, the registered images that cover it; `Any` is a few relaxed-acquire loads and no lock.
// The writer (the GPU thread, under the texture cache lock) adds a range before the image becomes findable or
// protected and removes it after it is unprotected, so a reader that sees zero sees a page no image watches (a
// stale non-zero only makes the reader take the lock). Unit tested in tests/PagePresenceTests.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Common {

class PagePresence {
public:
	static constexpr uint32_t kPageBits     = 12;
	static constexpr uint32_t kAddressBits  = 44;
	static constexpr uint32_t kLeafBits     = 16; // pages per leaf: 65,536 (256 MiB of address space)
	static constexpr uint32_t kTopBits      = kAddressBits - kPageBits - kLeafBits;
	static constexpr size_t   kLeafPages    = size_t {1} << kLeafBits;
	static constexpr size_t   kTopEntries   = size_t {1} << kTopBits;
	// A range of more pages than this is not answered (Any says yes): the caller takes the lock anyway.
	static constexpr size_t   kMaxQueryPages = 64;

	PagePresence(): m_top(new std::atomic<Leaf*>[kTopEntries]) {
		for (size_t i = 0; i < kTopEntries; i++) {
			m_top[i].store(nullptr, std::memory_order_relaxed);
		}
	}
	~PagePresence() {
		for (size_t i = 0; i < kTopEntries; i++) {
			delete m_top[i].load(std::memory_order_relaxed);
		}
	}
	PagePresence(const PagePresence&)            = delete;
	PagePresence& operator=(const PagePresence&) = delete;

	// Writer (one thread at a time): every page of [address, address + size) gets one more owner. False when the
	// range is outside the 44-bit space (nothing is counted).
	bool Add(uint64_t address, uint64_t size) { return Change(address, size, +1); }
	// Writer: the matching removal. A page that would go below zero is an accounting bug: it is left at zero.
	bool Remove(uint64_t address, uint64_t size) { return Change(address, size, -1); }

	// Reader, any thread: false only when no page of [address, address + size) has an owner. A range outside the
	// space, or longer than kMaxQueryPages pages, answers true (the caller falls back to its exact query).
	[[nodiscard]] bool Any(uint64_t address, uint64_t size) const {
		if (size == 0) {
			return false;
		}
		const uint64_t last = address + size - 1;
		if (last < address || (last >> kAddressBits) != 0) {
			return true;
		}
		const uint64_t first_page = address >> kPageBits;
		const uint64_t last_page  = last >> kPageBits;
		if (last_page - first_page >= kMaxQueryPages) {
			return true;
		}
		for (uint64_t page = first_page; page <= last_page; page++) {
			const Leaf* leaf = m_top[page >> kLeafBits].load(std::memory_order_acquire);
			if (leaf != nullptr && leaf->count[page & (kLeafPages - 1)].load(std::memory_order_acquire) != 0) {
				return true;
			}
		}
		return false;
	}

	// Reader, any thread: the exact answer where the counts allow one. None: no page of [address, address + size) has an owner.
	// Some: a page has an owner and its count is exact (below saturation), so an owner covers that page right now.
	// Unknown: the range is outside the space or longer than kMaxQueryPages pages, or a page is saturated (its count may be
	// stale non-zero); the caller falls back to its exact query. Use this where "an owner exists" is the whole question (the
	// counts are of images with a property, not of images whose page-granular overlap a later test could still narrow).
	enum class Answer : uint8_t { None, Some, Unknown };
	[[nodiscard]] Answer Query(uint64_t address, uint64_t size) const {
		if (size == 0) {
			return Answer::None;
		}
		const uint64_t last = address + size - 1;
		if (last < address || (last >> kAddressBits) != 0) {
			return Answer::Unknown;
		}
		const uint64_t first_page = address >> kPageBits;
		const uint64_t last_page  = last >> kPageBits;
		if (last_page - first_page >= kMaxQueryPages) {
			return Answer::Unknown;
		}
		for (uint64_t page = first_page; page <= last_page; page++) {
			const Leaf* leaf = m_top[page >> kLeafBits].load(std::memory_order_acquire);
			if (leaf == nullptr) {
				continue;
			}
			const uint16_t count = leaf->count[page & (kLeafPages - 1)].load(std::memory_order_acquire);
			if (count == UINT16_MAX) {
				return Answer::Unknown;
			}
			if (count != 0) {
				return Answer::Some;
			}
		}
		return Answer::None;
	}

	// The count of one page (tests, verification).
	[[nodiscard]] uint32_t Count(uint64_t address) const {
		const uint64_t page = address >> kPageBits;
		if ((address >> kAddressBits) != 0) {
			return 0;
		}
		const Leaf* leaf = m_top[page >> kLeafBits].load(std::memory_order_acquire);
		return leaf == nullptr ? 0 : leaf->count[page & (kLeafPages - 1)].load(std::memory_order_acquire);
	}

private:
	struct Leaf {
		std::atomic<uint16_t> count[kLeafPages];
		Leaf() {
			for (auto& c: count) {
				c.store(0, std::memory_order_relaxed);
			}
		}
	};

	bool Change(uint64_t address, uint64_t size, int delta) {
		if (size == 0) {
			return true;
		}
		const uint64_t last = address + size - 1;
		if (last < address || (last >> kAddressBits) != 0) {
			return false;
		}
		const uint64_t first_page = address >> kPageBits;
		const uint64_t last_page  = last >> kPageBits;
		Leaf*          leaf       = nullptr;
		uint64_t       leaf_index = ~uint64_t {0};
		for (uint64_t page = first_page; page <= last_page; page++) {
			if ((page >> kLeafBits) != leaf_index) {
				leaf_index = page >> kLeafBits;
				leaf       = m_top[leaf_index].load(std::memory_order_acquire);
				if (leaf == nullptr) {
					if (delta < 0) {
						continue;
					}
					auto* created = new Leaf();
					// One writer at a time: no other thread publishes this leaf.
					m_top[leaf_index].store(created, std::memory_order_release);
					leaf = created;
				}
			}
			if (leaf == nullptr) {
				continue;
			}
			auto& c = leaf->count[page & (kLeafPages - 1)];
			if (delta > 0) {
				const auto old = c.load(std::memory_order_relaxed);
				if (old == UINT16_MAX) {
					continue; // saturated: stays non-zero, never reaches zero again (a safe direction)
				}
				c.store(static_cast<uint16_t>(old + 1), std::memory_order_release);
			} else {
				const auto old = c.load(std::memory_order_relaxed);
				if (old != 0 && old != UINT16_MAX) {
					c.store(static_cast<uint16_t>(old - 1), std::memory_order_release);
				}
			}
		}
		return true;
	}

	std::unique_ptr<std::atomic<Leaf*>[]> m_top;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_PAGEPRESENCE_H_
