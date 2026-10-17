#ifndef EMULATOR_SRC_COMMON_SLOTVECTOR_H_
#define EMULATOR_SRC_COMMON_SLOTVECTOR_H_

#include "common/assert.h"
#include "common/abi.h"
#include "common/chunkedStorage.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Common {

struct SlotId {
	static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

	constexpr SlotId() noexcept = default;
	constexpr SlotId(uint32_t value) noexcept: index(value), generation(1) {}
	constexpr SlotId(uint32_t value, uint32_t slot_generation) noexcept
	    : index(value), generation(slot_generation) {}

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return index != INVALID_INDEX; }
	constexpr auto operator<=>(const SlotId&) const noexcept = default;

	uint32_t index = INVALID_INDEX;
	uint32_t generation = 0;
};

// Stable-address slot storage for cache resources that are intentionally non-movable.
template <typename T>
class SlotVector {
public:
	SlotVector() = default;
	KYTY_CLASS_NO_COPY(SlotVector);

	[[nodiscard]] T& operator[](SlotId id) noexcept {
		EXIT_IF(!is_allocated(id));
		return *m_values[id.index].ptr();
	}

	[[nodiscard]] const T& operator[](SlotId id) const noexcept {
		EXIT_IF(!is_allocated(id));
		return *m_values[id.index].ptr();
	}

	[[nodiscard]] T* try_get(SlotId id) noexcept {
		return is_allocated(id) ? m_values[id.index].ptr() : nullptr;
	}

	[[nodiscard]] const T* try_get(SlotId id) const noexcept {
		return is_allocated(id) ? m_values[id.index].ptr() : nullptr;
	}

	// One slot lookup, one cache line: the generation and the live flag sit in front of the value, in the line its first members
	// are in (they used to sit behind it, in the line of its last members).
	[[nodiscard]] bool is_allocated(SlotId id) const noexcept {
		if (!id || id.index >= m_values.size()) {
			return false;
		}
		const Slot& slot = m_values[id.index];
		return slot.generation == id.generation && slot.alive;
	}

	template <typename... Args>
	[[nodiscard]] SlotId insert(Args&&... args) {
		uint32_t index = 0;
		if (m_free_list.empty()) {
			index = static_cast<uint32_t>(m_values.size());
			m_values.emplace_back().emplace(std::forward<Args>(args)...);
		} else {
			index = m_free_list.back();
			m_free_list.pop_back();
			EXIT_IF(m_values[index].alive);
			m_values[index].emplace(std::forward<Args>(args)...);
		}
		++m_size;
		return SlotId {index, m_values[index].generation};
	}

	void erase(SlotId id) noexcept {
		EXIT_IF(!is_allocated(id));
		auto& slot = m_values[id.index];
		slot.reset();
		if (++slot.generation == 0) {
			slot.generation = 1;
		}
		m_free_list.push_back(id.index);
		--m_size;
	}

	[[nodiscard]] size_t size() const noexcept { return m_size; }
	[[nodiscard]] size_t capacity() const noexcept { return m_values.size(); }

	template <typename F>
	void ForEach(F&& fn) {
		for (uint32_t index = 0; index < m_values.size(); ++index) {
			if (m_values[index].alive) {
				fn(SlotId {index, m_values[index].generation}, *m_values[index].ptr());
			}
		}
	}

	template <typename F>
	void ForEach(F&& fn) const {
		for (uint32_t index = 0; index < m_values.size(); ++index) {
			if (m_values[index].alive) {
				fn(SlotId {index, m_values[index].generation}, *m_values[index].ptr());
			}
		}
	}

private:
	struct Slot {
		uint32_t generation = 1;
		bool     alive      = false;
		alignas(T) unsigned char storage[sizeof(T)];

		Slot() noexcept = default;
		~Slot() { reset(); }
		Slot(const Slot&)            = delete;
		Slot& operator=(const Slot&) = delete;

		[[nodiscard]] T*       ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
		[[nodiscard]] const T* ptr() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }

		template <typename... Args>
		void emplace(Args&&... args) {
			::new (static_cast<void*>(storage)) T(std::forward<Args>(args)...);
			alive = true;
		}
		void reset() noexcept {
			if (alive) {
				ptr()->~T();
				alive = false;
			}
		}
	};

	ChunkedStorage<Slot>         m_values;
	std::vector<uint32_t>        m_free_list;
	size_t                       m_size = 0;
};

} // namespace Common

template <>
struct std::hash<Common::SlotId> {
	[[nodiscard]] size_t operator()(Common::SlotId id) const noexcept {
		return std::hash<uint64_t> {}((static_cast<uint64_t>(id.generation) << 32u) | id.index);
	}
};

#endif // EMULATOR_SRC_COMMON_SLOTVECTOR_H_
