#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/adaptiveLock.h"
#include "common/assert.h"
#include "common/atomicBitMirror.h"
#include "graphics/host_gpu/bdaSyncGate.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <mutex>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

// --write-fault-around: write faults that took pages ahead with them, and those pages (RegionManager::FaultRunAhead).
inline std::atomic<uint64_t> g_fault_run_events {0};
inline std::atomic<uint64_t> g_fault_run_pages {0};

class TrackingSpinLock final {
public:
	void lock() noexcept {
		if (!m_lock.TryLock()) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == CurrentThread());
			m_lock.Lock();
		}
		m_owner.store(CurrentThread(), std::memory_order_relaxed);
	}
	[[nodiscard]] bool HeldByCurrentThread() const noexcept {
		return m_owner.load(std::memory_order_relaxed) == CurrentThread();
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.Unlock();
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		static thread_local const uint32_t tid =
		    static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
		return tid;
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	Common::AdaptiveLock m_lock;
	std::atomic_uint32_t m_owner {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
		m_cpu_mirror.Reset(m_cpu_dirty);
		m_gpu_mirror.Reset(m_gpu_dirty);
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	// Moves (under the region lock, after the bits changed) whenever any page's CPU-dirty or GPU-dirty
	// bit may have changed in this region. A reader that saw both queries answer "clean" while it read
	// epoch E and finds E unchanged later knows no page of the region changed state in between: a clean
	// page is write-protected, so a guest write to it would have faulted into ChangeState, and a GPU
	// write marks it through ChangeState as well. Readable without the lock.
	[[nodiscard]] uint32_t Epoch() const { return m_epoch.load(std::memory_order_acquire); }
	// Write-fault run-ahead (--write-fault-around). A guest thread that writes a buffer sequentially faults
	// once per page: the handler unprotects the page, the next upload protects it again, and the write
	// pointer moves on to the next protected page. A fault on the page where the SAME THREAD's previous
	// fault run ended continues a sequential stream (measured: 54 % of the faults are +1 page from the same
	// thread's previous fault, 7 % from the previous fault in the region, because several threads write one
	// region): the pages after it that are still clean on both sides are marked CPU-dirty with it (one change,
	// one unprotect), doubling the run each time up to kMaxFaultRun. A page marked dirty without a write only
	// costs one redundant upload; a page an image also watches stays protected (the page manager counts
	// watchers), so the image still sees the real write. Returns the number of pages after `page` to add.
	// The lock is held; `stream` is the calling thread's.
	static constexpr size_t kMaxFaultRun = 64;
	struct FaultStream {
		uint64_t next_address = 0; // where the previous fault's run ended
		size_t   run          = 0;
	};
	[[nodiscard]] size_t FaultRunAhead(size_t page, FaultStream& stream) {
		const uint64_t address    = m_cpu_addr + page * TRACKER_PAGE_SIZE;
		const bool     sequential = stream.next_address != 0 && address == stream.next_address;
		stream.run = sequential ? (stream.run == 0 ? 2 : std::min(stream.run * 2, kMaxFaultRun)) : 0;
		size_t extra = 0;
		if (stream.run != 0) {
			const size_t limit = std::min(page + 1 + stream.run, TRACKER_REGION_PAGES);
			const auto   cpu   = m_cpu_dirty.FirstRangeFrom(page + 1).first;
			const auto   gpu   = m_gpu_dirty.FirstRangeFrom(page + 1).first;
			const size_t stop  = std::min({limit, static_cast<size_t>(cpu), static_cast<size_t>(gpu)});
			extra              = stop > page + 1 ? stop - (page + 1) : 0;
		}
		stream.next_address = address + (1 + extra) * TRACKER_PAGE_SIZE;
		return extra;
	}

	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	// Lock-free twin of IsModified (--tracker-lockfree, see AtomicBitMirror): the mirror of the
	// dirty bits is published under the lock by every change, before the pages' protection moves.
	template <DirtySource source>
	[[nodiscard]] bool IsModifiedLockFree(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetMirror<source>().Any(start, end);
	}

	// Verifier: both mirrors equal their bits. Caller holds `lock`.
	[[nodiscard]] bool MirrorsMatch() const {
		return m_cpu_mirror.Matches(m_cpu_dirty) && m_gpu_mirror.Matches(m_gpu_dirty);
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		GetMirror<source>().Publish(bits, start, end);
		if constexpr (source == DirtySource::Cpu && enable) {
			// After the bits are set (K15): a BDA sync that read the generation earlier either
			// uploads these pages or sees the new value on its next call.
			BdaSync::NoteCpuModified(m_cpu_addr + start * TRACKER_PAGE_SIZE,
			                         (end - start) * TRACKER_PAGE_SIZE);
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateProtection<!enable, false>();
		} else {
			UpdateProtection<enable, true>();
		}
		m_epoch.fetch_add(1, std::memory_order_release);
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			GetMirror<source>().Publish(bits, start, end);
			if constexpr (source == DirtySource::Cpu) {
				UpdateProtection<true, false>();
			} else {
				UpdateProtection<false, true>();
			}
			m_epoch.fetch_add(1, std::memory_order_release);
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	TrackingSpinLock lock;

private:
	template <bool track, bool is_read>
	void UpdateProtection() {
		const auto protection = is_read ? ~m_gpu_dirty : m_cpu_dirty;
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	template <DirtySource source>
	Common::AtomicBitMirror<TRACKER_REGION_PAGES>& GetMirror() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_mirror;
		} else {
			return m_gpu_mirror;
		}
	}

	template <DirtySource source>
	const Common::AtomicBitMirror<TRACKER_REGION_PAGES>& GetMirror() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_mirror;
		} else {
			return m_gpu_mirror;
		}
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	std::atomic<uint32_t> m_epoch {0};
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	// Lock-free copies of m_cpu_dirty / m_gpu_dirty, written under `lock` (see IsModifiedLockFree).
	Common::AtomicBitMirror<TRACKER_REGION_PAGES> m_cpu_mirror;
	Common::AtomicBitMirror<TRACKER_REGION_PAGES> m_gpu_mirror;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
