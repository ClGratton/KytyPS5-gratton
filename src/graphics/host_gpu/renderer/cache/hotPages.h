#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOTPAGES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOTPAGES_H_

// Bookkeeping of --eager-readback: the pages a game thread read-faulted on ("hot" pages) and the
// eager copies in flight for them. Pure data and policy, unit tested in tests/HotPagesTests.cpp;
// BufferCache owns one and touches it only on the GPU thread.
//
// A game thread that reads GPU-written memory faults, and its read is served on the GPU thread
// after a full drain, although in practice the writer has long finished (96-100 % of the reads
// in ogeidexe's and our own traces). A page that was faulted on once is likely to be read again
// after the next GPU write, so its pending GPU writes are copied out in the command buffer
// that is about to be submitted and written back to guest memory when that buffer completes; the
// page is unprotected only after that, and only if no newer GPU write reached it.
//
// Ported from ogeidexe/KytyPS5@76af431df and 663a8d887 ("write back GPU data game threads read
// before they read it", "never block the GPU thread for an eager write-back"; GPL-2.0) and
// BryanKAdams/BryKytyPS5@2fe8f1961 ("read back hot buffer pages eagerly after GPU writes", a
// bounded set with replacement of the page faulted on least recently; GPL-2.0), re-implemented.

#include <cstddef>
#include <cstdint>
#include <map>

namespace Libs::Graphics {

class HotPageSet {
public:
	static constexpr size_t kDefaultCapacity = 64;

	struct Entry {
		uint64_t last_fault    = 0; // logical clock of the latest fault on the page
		uint64_t in_flight_tick = 0; // tick of the command buffer holding its eager copy, 0: none
	};

	explicit HotPageSet(size_t capacity = kDefaultCapacity): m_capacity(capacity) {}

	// A game thread read-faulted on the page (page-aligned address). Marks it hot; when the set is
	// full the page faulted on least recently, among those with no copy in flight, makes room. A
	// full set whose pages are all in flight ignores the new page: it is simply read on demand.
	void NoteFault(uint64_t page) {
		++m_clock;
		const auto found = m_pages.find(page);
		if (found != m_pages.end()) {
			found->second.last_fault = m_clock;
			return;
		}
		if (m_capacity == 0) {
			return;
		}
		if (m_pages.size() >= m_capacity) {
			auto victim = m_pages.end();
			for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
				if (it->second.in_flight_tick != 0) {
					continue;
				}
				if (victim == m_pages.end() || it->second.last_fault < victim->second.last_fault) {
					victim = it;
				}
			}
			if (victim == m_pages.end()) {
				return;
			}
			m_pages.erase(victim);
		}
		m_pages.emplace(page, Entry {m_clock, 0});
	}

	[[nodiscard]] bool   Empty() const { return m_pages.empty(); }
	// True when some page has an eager copy in flight (a scan of at most `capacity` entries).
	[[nodiscard]] bool HasInFlight() const {
		for (const auto& [page, entry]: m_pages) {
			if (entry.in_flight_tick != 0) {
				return true;
			}
		}
		return false;
	}
	[[nodiscard]] size_t Size() const { return m_pages.size(); }
	[[nodiscard]] bool   Contains(uint64_t page) const { return m_pages.find(page) != m_pages.end(); }
	[[nodiscard]] uint64_t InFlight(uint64_t page) const {
		const auto found = m_pages.find(page);
		return found == m_pages.end() ? 0 : found->second.in_flight_tick;
	}
	void SetInFlight(uint64_t page, uint64_t tick) {
		const auto found = m_pages.find(page);
		if (found != m_pages.end()) {
			found->second.in_flight_tick = tick;
		}
	}

	// Calls func(page, entry) for the hot pages in [begin, end), in address order. func may call
	// SetInFlight (it must not add or remove pages).
	template <typename Func>
	void ForEachInRange(uint64_t begin, uint64_t end, Func&& func) {
		for (auto it = m_pages.lower_bound(begin); it != m_pages.end() && it->first < end; ++it) {
			func(it->first, it->second);
		}
	}

private:
	std::map<uint64_t, Entry> m_pages;
	size_t                    m_capacity;
	uint64_t                  m_clock = 0;
};

// What to do with a hot page whose eager copy was recorded for `tick`.
enum class HotRetire : uint8_t {
	Wait,          // the copy has not completed (or its write-back has not run) and the caller may not block
	StayProtected, // completed, but a newer GPU write reached the page: its bytes are not in guest memory
	Unprotect,     // completed and nothing newer: the guest may read the page without a fault
};

// `completed`: the GPU finished the tick and the deferred write-back ran. `can_wait`: the caller
// is a guest read that must see the data (it then waits instead of returning Wait).
[[nodiscard]] constexpr HotRetire DecideRetire(bool completed, bool can_wait, bool newer_gpu_write) {
	if (!completed && !can_wait) {
		return HotRetire::Wait;
	}
	return newer_gpu_write ? HotRetire::StayProtected : HotRetire::Unprotect;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOTPAGES_H_
