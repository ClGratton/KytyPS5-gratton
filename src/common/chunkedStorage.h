#ifndef EMULATOR_SRC_COMMON_CHUNKEDSTORAGE_H_
#define EMULATOR_SRC_COMMON_CHUNKEDSTORAGE_H_

// Append-only storage with stable element addresses and O(1) indexing by shift and mask.
//
// It replaces std::deque for the cache slot tables (SlotVector, LeastRecentlyUsedCache). A deque indexes with a division by the
// number of elements per node and reads size() from several members; for an element bigger than 512 bytes libstdc++ even makes one
// node per element. The cache lookups index these tables several times per draw, so the index arithmetic showed up as a few percent
// of the GPU thread in a sampling profile. Here the chunk size is a power of two, the chunk table is a plain array of pointers and the
// size is one member.
//
// Elements are default-constructed a chunk at a time and never destroyed before the storage is; growth never moves an element. Like a
// deque, it is not safe to grow while another thread reads it.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Common {

template <typename T>
class ChunkedStorage {
public:
	ChunkedStorage() { m_chunks.reserve(256); }

	ChunkedStorage(const ChunkedStorage&)            = delete;
	ChunkedStorage& operator=(const ChunkedStorage&) = delete;

	[[nodiscard]] T& operator[](size_t index) noexcept { return m_chunks[index >> kShift][index & kMask]; }

	[[nodiscard]] const T& operator[](size_t index) const noexcept { return m_chunks[index >> kShift][index & kMask]; }

	[[nodiscard]] size_t size() const noexcept { return m_size; }

	// Makes one more default-constructed element reachable and returns it.
	T& emplace_back() {
		if ((m_size >> kShift) == m_chunks.size()) {
			m_chunks.push_back(std::unique_ptr<T[]>(new T[kChunkElements])); // default-initialized: no zero fill of the storage
		}
		return (*this)[m_size++];
	}

	[[nodiscard]] T& back() noexcept { return (*this)[m_size - 1]; }

private:
	// About 16 KiB per chunk, at least 4 and at most 1024 elements.
	static constexpr size_t ChunkElementsFor() noexcept {
		size_t elements = 16384 / sizeof(T);
		if (elements < 4) {
			elements = 4;
		}
		if (elements > 1024) {
			elements = 1024;
		}
		return std::bit_floor(elements);
	}

	static constexpr size_t kChunkElements = ChunkElementsFor();
	static constexpr size_t kShift         = std::countr_zero(kChunkElements);
	static constexpr size_t kMask          = kChunkElements - 1;

	std::vector<std::unique_ptr<T[]>> m_chunks;
	size_t                            m_size = 0;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_CHUNKEDSTORAGE_H_
