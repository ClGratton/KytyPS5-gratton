#ifndef KYTY_COMMON_PUBLISHED_INDEX_H_
#define KYTY_COMMON_PUBLISHED_INDEX_H_

// A read index over objects that live elsewhere, readable without a lock and without any atomic read-modify-write
// (--lockfree-pipeline-reads).
//
// The pipeline map and the program table are read for every draw by the GPU thread and by the look-ahead workers, and written only when
// a new pipeline or program appears. Their reader lock (a shared mutex, then a per-thread counter) was a locked instruction on the GPU
// thread's draw path: in a sampling profile the instruction after it held about 5 % of all GPU-thread samples, the same with either
// lock. A locked instruction waits for every earlier store of its thread to complete, and the stores of the vertex and index copies
// that precede the pipeline lookup are exactly that kind (cold lines of the stream ring).
//
// The index holds pointers to elements of a node-based container that never erases (std::unordered_map node references stay valid
// when the table rehashes). The owner inserts under its own exclusive lock, AFTER the element is completely filled in; the insertion
// is a release store, a lookup an acquire load, so a reader that finds the element sees it complete. Insert-only open addressing
// with linear probing; the table doubles at half load and the old tables are kept until the index is destroyed, so a reader that
// still holds an old table reads valid memory. A reader can miss an element inserted after it loaded the table (or while it probes):
// the answer "not found" is then only "not found yet", and the caller repeats the lookup under its lock. A found element is always
// the one the container holds for that key, so a hit is exact.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Common {

// `KeyOf(const Node&)` returns a const reference to the node's key; `Hash` and `KeyEq` are the container's own functors.
template <class Node, class Key, class KeyOf, class Hash, class KeyEq>
class PublishedIndex {
public:
	explicit PublishedIndex(KeyOf key_of = {}, Hash hash = {}, KeyEq eq = {}): m_key_of(key_of), m_hash(hash), m_eq(eq) {
		Publish(NewTable(kInitialSlots));
	}
	PublishedIndex(const PublishedIndex&)            = delete;
	PublishedIndex& operator=(const PublishedIndex&) = delete;

	// Any thread, any time. nullptr: not (yet) in the index.
	[[nodiscard]] const Node* Find(const Key& key) const noexcept {
		const Table* const table = m_current.load(std::memory_order_acquire);
		return FindIn(*table, key, m_hash(key));
	}

	[[nodiscard]] const Node* Find(const Key& key, size_t hash) const noexcept {
		return FindIn(*m_current.load(std::memory_order_acquire), key, hash);
	}

	// One caller at a time (the owner's exclusive lock). The node must be complete; inserting the same key twice is a bug.
	void Insert(const Node* node) {
		Table* table = m_tables.back().get();
		if ((m_count + 1) * 2 > table->capacity) {
			table = Grow(*table);
		}
		Place(*table, node);
		++m_count;
	}

	[[nodiscard]] size_t Size() const noexcept { return m_count; }

private:
	static constexpr size_t kInitialSlots = 1024;

	struct Table {
		size_t                                      capacity = 0; // power of two
		unsigned                                    shift    = 0; // 64 - log2(capacity)
		std::unique_ptr<std::atomic<const Node*>[]> slots;
	};

	// The container's hash may have weak low bits (a mix of small integers): the slot comes from the top bits of a multiplicative scramble.
	static size_t Slot(size_t hash, const Table& table) noexcept {
		return static_cast<size_t>((static_cast<uint64_t>(hash) * 0x9e3779b97f4a7c15ull) >> table.shift);
	}

	std::unique_ptr<Table> NewTable(size_t capacity) {
		auto table      = std::make_unique<Table>();
		table->capacity = capacity;
		table->shift    = 64u - static_cast<unsigned>(__builtin_ctzll(capacity));
		table->slots    = std::make_unique<std::atomic<const Node*>[]>(capacity);
		for (size_t i = 0; i < capacity; ++i) {
			table->slots[i].store(nullptr, std::memory_order_relaxed);
		}
		return table;
	}

	void Publish(std::unique_ptr<Table> table) {
		Table* const raw = table.get();
		m_tables.push_back(std::move(table));
		m_current.store(raw, std::memory_order_release);
	}

	Table* Grow(const Table& old) {
		auto bigger = NewTable(old.capacity * 2);
		for (size_t i = 0; i < old.capacity; ++i) {
			if (const Node* node = old.slots[i].load(std::memory_order_relaxed)) {
				Place(*bigger, node);
			}
		}
		Table* const raw = bigger.get();
		Publish(std::move(bigger));
		return raw;
	}

	void Place(Table& table, const Node* node) {
		const size_t mask = table.capacity - 1;
		size_t       i    = Slot(m_hash(m_key_of(*node)), table);
		while (table.slots[i].load(std::memory_order_relaxed) != nullptr) {
			i = (i + 1) & mask;
		}
		table.slots[i].store(node, std::memory_order_release);
	}

	const Node* FindIn(const Table& table, const Key& key, size_t hash) const noexcept {
		const size_t mask = table.capacity - 1;
		for (size_t i = Slot(hash, table);; i = (i + 1) & mask) {
			const Node* const node = table.slots[i].load(std::memory_order_acquire);
			if (node == nullptr) {
				return nullptr;
			}
			if (m_eq(m_key_of(*node), key)) {
				return node;
			}
		}
	}

	KeyOf  m_key_of;
	Hash   m_hash;
	KeyEq  m_eq;
	size_t m_count = 0;
	// Writer side: every table ever published (the last one is the current one). Readers use only m_current.
	std::vector<std::unique_ptr<Table>> m_tables;
	std::atomic<const Table*>           m_current {nullptr};
};

} // namespace Common

#endif // KYTY_COMMON_PUBLISHED_INDEX_H_
