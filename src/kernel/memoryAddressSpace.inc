// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

// The shader resource walk answers reads below this address with zeros (common/guestAddressSpace.h).
static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN  = Common::kGuestAddressSpaceStart;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX  = 0x07ffffbfffull;
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fbfffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x7000000000ull;
#else
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fffffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x1000000000ull;
#endif
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;

static bool GuestFreeRangeContains(uint64_t base, uint64_t range_size, uint64_t candidate,
                                   uint64_t size) {
	if (UINT64_MAX - base < range_size || candidate < base) {
		return false;
	}
	const auto range_end = base + range_size;
	return candidate <= range_end && size <= range_end - candidate;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool CoalescePlaceholders(uint64_t vaddr, uint64_t size) {
	return size != 0 && VirtualFree(reinterpret_cast<void*>(vaddr), size,
	                                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
#endif

// Ranges whose guest mapping is briefly absent while the emulator replaces it: a guest mmap with
// MAP_FIXED over an existing mapping (unmapped, then mapped again), and, on Windows, the parts of a
// file view that stay mapped when part of it is unmapped (Windows splits a view only by unmapping
// all of it). On the console these replacements are atomic, so guest threads keep using the
// memory: GTA V's streamer decompresses into buffers whose 64 KiB pages are mapped again or
// neighbour an unmap. A fault inside such a range waits until the mapping is back and is retried
// instead of being reported as an access violation.
struct TransientUnmapWindow {
	std::atomic<uint64_t> id {0};
	std::atomic<uint64_t> start {0};
	std::atomic<uint64_t> end {0};
	std::atomic<int64_t>  closed_ns {0}; // 0 while the range is being remapped
};

class TransientUnmapWindows {
public:
	using Window = TransientUnmapWindow;

	class Scope {
	public:
		Scope(uint64_t start, uint64_t end) noexcept: m_window(Open(start, end)) {}
		~Scope() { m_window->closed_ns.store(Now(), std::memory_order_release); }
		KYTY_CLASS_NO_COPY(Scope);

	private:
		Window* m_window;
	};

	// The newest range being remapped recently that holds vaddr (0: none). Waits while it still
	// is being remapped.
	static uint64_t Wait(uint64_t vaddr) noexcept {
		constexpr int64_t Second  = 1000000000;
		uint64_t          matched = 0;
		for (auto& window: s_windows) {
			if (vaddr < window.start.load(std::memory_order_acquire) ||
			    vaddr >= window.end.load(std::memory_order_acquire)) {
				continue;
			}
			const auto begin = Now();
			for (uint32_t spins = 0; window.closed_ns.load(std::memory_order_acquire) == 0; spins++) {
				if (Now() - begin > Second * 10) {
					return 0; // never closed: report the fault
				}
				if (spins < 4096) {
#if defined(_M_X64) || defined(__x86_64__)
					_mm_pause();
#endif
				} else {
					std::this_thread::yield();
				}
			}
			// The fault may be handled after the mapping is already back.
			if (Now() - window.closed_ns.load(std::memory_order_acquire) < Second) {
				matched = std::max(matched, window.id.load(std::memory_order_acquire));
			}
		}
		return matched;
	}

private:
	static Window* Open(uint64_t start, uint64_t end) noexcept {
		auto& window = s_windows[s_next.fetch_add(1, std::memory_order_relaxed) % s_windows.size()];
		window.closed_ns.store(0, std::memory_order_relaxed);
		window.id.store(s_opened.fetch_add(1, std::memory_order_relaxed) + 1,
		                std::memory_order_relaxed);
		window.start.store(start, std::memory_order_relaxed);
		window.end.store(end, std::memory_order_seq_cst);
		return &window;
	}

	static int64_t Now() noexcept {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
		           std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	}

	inline static std::array<Window, 16> s_windows {};
	inline static std::atomic<uint32_t>  s_next {0};
	inline static std::atomic<uint64_t>  s_opened {0};
};

class GuestBackingStore {
public:
	enum class FailureReason {
		None,
		BackingUnavailable,
		PhysicalRangeOutOfBounds,
		FixedMapViewFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		WrongHostAddress,
		AddressRangeUnavailable,
	};

	explicit GuestBackingStore(uint64_t size): m_size(size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_handle = CreateFileMappingA(
		    INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
		    static_cast<DWORD>(m_size >> 32u), static_cast<DWORD>(m_size & 0xffffffffu), nullptr);
		if (m_handle == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: CreateFileMapping failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			return;
		}
		m_backing_base = static_cast<uint8_t*>(
		    MapViewOfFile(m_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, m_size));
		if (m_backing_base == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			CloseHandle(m_handle);
			m_handle = nullptr;
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#if defined(__APPLE__)
		{
			char shm_name[64];
			snprintf(shm_name, sizeof(shm_name), "/kyty-dm-%d-%llx", static_cast<int>(getpid()),
			         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
			m_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (m_fd >= 0) {
				shm_unlink(shm_name);
			}
		}
#else
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
#endif
		if (m_fd < 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: shared fd creation failed: %d\n",
			           errno);
			return;
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: ftruncate failed: %d\n", errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: writable alias failed: %d\n",
			           errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		m_backing_base = static_cast<uint8_t*>(backing);
		if (g_mem_stats_on || g_mem_punch_on) {
			InitTrackingUnlocked();
		}
#endif
	}

	~GuestBackingStore() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [_, entry]: m_maps) {
			UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size);
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			UnmapViewOfFile(m_backing_base);
		}
		if (m_handle != nullptr) {
			CloseHandle(m_handle);
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		for (const auto& [_, entry]: m_maps) {
			EXIT_IF(
			    !UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size));
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			munmap(m_backing_base, m_size);
		}
		if (m_fd >= 0) {
			close(m_fd);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestBackingStore);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::BackingUnavailable: return "backing-unavailable";
			case FailureReason::PhysicalRangeOutOfBounds: return "physical-range-out-of-bounds";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::WrongHostAddress: return "wrong-host-address";
			case FailureReason::AddressRangeUnavailable: return "address-range-unavailable";
			default: return "unknown";
		}
	}

	[[nodiscard]] bool IsAvailable() const {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return m_handle != nullptr && m_backing_base != nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		return m_fd >= 0 && m_backing_base != nullptr;
#else
		return false;
#endif
	}

	[[nodiscard]] uint64_t GetBackingBase() const {
		return reinterpret_cast<uint64_t>(m_backing_base);
	}

	[[nodiscard]] uint64_t GetBackingSize() const { return m_size; }

	bool ZeroRange(uint64_t backing_offset, uint64_t size) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		std::memset(m_backing_base + backing_offset, 0, size);
		return true;
	}

	// The guest freed [backing_offset, backing_offset + size) of the backing object (direct memory
	// released, pool range decommitted, flexible memory unmapped). Whole host pages inside the range
	// that no view maps any more are counted and, with --mem-release-punch, returned to the host.
	// Pages a view still maps are remembered and handled when the last view goes away.
	void NoteReleased(uint64_t backing_offset, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		if (!m_tracking || size == 0 || !RangeOk(backing_offset, size)) {
			return;
		}
		std::lock_guard lock(m_mutex);
		TrackReleaseUnlocked(backing_offset, size);
#else
		(void)backing_offset;
		(void)size;
#endif
	}

	struct ReleaseGauges {
		uint64_t outstanding_bytes          = 0; // released, unmapped, not punched
		uint64_t outstanding_resident_bytes = 0; // ... of which resident in RAM right now
		uint64_t pending_bytes              = 0; // released but a view still maps them
	};

	[[nodiscard]] ReleaseGauges GetReleaseGauges() {
		ReleaseGauges gauges;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		if (!m_tracking) {
			return gauges;
		}
		std::shared_lock lock(m_mutex);
		const uint64_t   pages = m_pstate.size();
		for (uint64_t page = 0; page < pages;) {
			const uint8_t state = m_pstate[page];
			if (state == PAGE_PENDING) {
				gauges.pending_bytes += m_host_page;
				page++;
				continue;
			}
			if (state != PAGE_CANDIDATE) {
				page++;
				continue;
			}
			uint64_t end = page;
			while (end < pages && m_pstate[end] == PAGE_CANDIDATE) {
				end++;
			}
			gauges.outstanding_bytes += (end - page) * m_host_page;
			gauges.outstanding_resident_bytes += ResidentBytes(page, end);
			page = end;
		}
#endif
		return gauges;
	}

	// Bytes of the guest range [vaddr, vaddr + size) on resident backing pages (mincore through the
	// writable alias; swapped-out pages do not count). Whole host pages, rounded outwards.
	[[nodiscard]] uint64_t ResidentGuestBytes(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		if (!IsAvailable() || size == 0 || UINT64_MAX - vaddr < size) {
			return 0;
		}
		std::shared_lock lock(m_mutex);
		const auto       end     = vaddr + size;
		auto             mapping = m_maps.upper_bound(vaddr);
		if (mapping != m_maps.begin()) {
			--mapping;
		}
		uint64_t resident = 0;
		for (; mapping != m_maps.end() && mapping->second.vaddr < end; ++mapping) {
			const auto& entry = mapping->second;
			const auto  lo    = std::max(vaddr, entry.vaddr);
			const auto  hi    = std::min(end, entry.vaddr + entry.size);
			if (lo >= hi) {
				continue;
			}
			const auto offset = entry.backing_offset + (lo - entry.vaddr);
			resident += ResidentBytesInFile(offset, offset + (hi - lo));
		}
		return resident;
#else
		(void)vaddr;
		(void)size;
		return 0;
#endif
	}

	[[nodiscard]] uint64_t ResidentTotalBytes() {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		return IsAvailable() ? ResidentBytesInFile(0, m_size) : 0;
#else
		return 0;
#endif
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		const auto      end   = vaddr + size;
		auto            first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

		const auto* source = static_cast<const uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			std::memcpy(m_backing_base + offset, source + (current - vaddr), bytes);
			current += bytes;
		}
		return true;
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		// --backing-read-cache: a read inside a mapping entry this thread has seen, while no mapping
		// changed since (the epoch), is the entry's backing bytes: no map lock, no tree walk.
		struct Remembered {
			const GuestBackingStore* store = nullptr;
			uint64_t                 epoch = 0, lo = 0, hi = 0, delta = 0;
		};
		constexpr size_t                           kRemembered = 8;
		thread_local std::array<Remembered, kRemembered> remembered {};
		thread_local size_t                        next_remembered = 0;
		const bool cache_on = Config::BackingReadCacheEnabled();
		if (cache_on) {
			const auto epoch = m_maps_epoch.load(std::memory_order_acquire);
			for (const auto& r: remembered) {
				if (r.store == this && r.epoch == epoch && vaddr >= r.lo && size <= r.hi - vaddr) {
					const auto offset = vaddr + r.delta;
					if (RangeOk(offset, size)) {
						if (Config::ResolvedDrawVerify()) {
							VerifyRememberedRead(vaddr, size, offset, epoch);
						}
						g_backing_read_cache_stats.hits++;
						CopyFromBacking(static_cast<uint8_t*>(data), offset, size);
						return true;
					}
				}
			}
			g_backing_read_cache_stats.misses++;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end   = vaddr + size;
		auto       first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

		auto* destination = static_cast<uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			CopyFromBacking(destination + (current - vaddr), offset, bytes);
			current += bytes;
		}
		if (cache_on && first->second.vaddr <= vaddr && end <= first->second.vaddr + first->second.size) {
			// Remember the entry (the epoch cannot move while the shared lock is held).
			const auto& entry = first->second;
			remembered[next_remembered] = {this, m_maps_epoch.load(std::memory_order_relaxed), entry.vaddr,
			                               entry.vaddr + entry.size, entry.backing_offset - entry.vaddr};
			next_remembered = (next_remembered + 1) % kRemembered;
		}
		return true;
	}

	// --resolved-draw-verify: the entry a remembered read used is the live one (checked under the lock; skipped when
	// a mapping changed in between, which is a legal order of the two events).
	void VerifyRememberedRead(uint64_t vaddr, uint64_t size, uint64_t offset, uint64_t epoch) {
		std::shared_lock lock(m_mutex);
		if (m_maps_epoch.load(std::memory_order_relaxed) != epoch) {
			return;
		}
		auto it = m_maps.upper_bound(vaddr);
		bool ok = it != m_maps.begin();
		if (ok) {
			--it;
			const auto& entry = it->second;
			ok = vaddr >= entry.vaddr && vaddr + size <= entry.vaddr + entry.size &&
			     entry.backing_offset + (vaddr - entry.vaddr) == offset;
		}
		if (!ok) {
			EXIT("GuestBackingStore: a remembered read of 0x%016" PRIx64 "+0x%" PRIx64
			     " differs from the live mapping\n", vaddr, size);
		}
	}

	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end     = vaddr + size;
		auto       mapping = m_maps.lower_bound(vaddr);
		if (mapping != m_maps.begin()) {
			auto previous = std::prev(mapping);
			if (previous->second.vaddr + previous->second.size > vaddr) {
				mapping = previous;
			}
		}

		auto* destination = static_cast<uint8_t*>(data);
		auto  cursor      = vaddr;
		for (; mapping != m_maps.end() && mapping->second.vaddr < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  start  = std::max(vaddr, entry.vaddr);
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - start;
			const auto  offset = entry.backing_offset + start - entry.vaddr;
			if (cursor < start) {
				std::memset(destination + (cursor - vaddr), 0, start - cursor);
			}
			CopyFromBacking(destination + (start - vaddr), offset, bytes);
			cursor = start + bytes;
		}
		if (cursor < end) {
			std::memset(destination + (cursor - vaddr), 0, end - cursor);
		}
		return true;
	}

	// True when every byte of [vaddr, vaddr + size) is mapped to the backing store and none of it was
	// ever written (no data and no swapped page in the backing file): it reads as zeros, and
	// checking it reads nothing and makes no page resident. False when in doubt (unmapped parts,
	// a page that holds data, hosts without SEEK_DATA on the backing object).
	bool RangeIsNeverWritten(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		if (!IsAvailable() || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		const auto       end   = vaddr + size;
		auto             first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			const auto  lo    = entry.backing_offset + current - entry.vaddr;
			if (!RangeOk(lo, bytes) || !FileRangeIsHole(lo, lo + bytes)) {
				return false;
			}
			current += bytes;
		}
		return true;
#else
		(void)vaddr;
		(void)size;
		return false;
#endif
	}

	// Debug check for RangeIsNeverWritten: reads the range with pread (no page becomes resident) and
	// returns whether every byte is zero. False when it cannot read the range.
	bool RangeReadsAsZero(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		if (!IsAvailable() || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		const auto       end   = vaddr + size;
		auto             first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		std::vector<uint8_t> scratch(1u << 20);
		auto                 current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			const auto  lo    = entry.backing_offset + current - entry.vaddr;
			if (!RangeOk(lo, bytes)) {
				return false;
			}
			for (uint64_t done = 0; done < bytes;) {
				const auto n = std::min<uint64_t>(scratch.size(), bytes - done);
				if (!PreadFromBacking(scratch.data(), lo + done, n)) {
					return false;
				}
				if (std::any_of(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n),
				                [](uint8_t b) { return b != 0; })) {
					return false;
				}
				done += n;
			}
			current += bytes;
		}
		return true;
#else
		(void)vaddr;
		(void)size;
		return false;
#endif
	}

	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	              VirtualMemory::Mode mode, FailureReason* failure_reason = nullptr) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		void* mapped = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			if (mapped != nullptr) {
				UnmapViewPreservePlaceholder(mapped, size);
				SetFailure(failure_reason, FailureReason::WrongHostAddress);
			}
			return false;
		}

		{
			std::lock_guard lock(m_mutex);
			EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
			m_maps_epoch.fetch_add(1, std::memory_order_release);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#else
		std::lock_guard lock(m_mutex);
		void* mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			return false;
		}
		// Count the new view before dropping the ones it replaced, so pages that stay mapped are
		// never seen as unmapped in between.
		TrackMapUnlocked(backing_offset, size);
		RemoveMappingsUnlocked(vaddr, size);
		EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		m_maps_epoch.fetch_add(1, std::memory_order_release);
		SetFailure(failure_reason, FailureReason::None);
		return true;
#endif
	}

	bool Unmap(uint64_t vaddr, uint64_t size, bool* placeholder_preserved = nullptr) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = false;
		}

		const auto            end = vaddr + size;
		std::vector<MapEntry> targets;
		{
			std::lock_guard lock(m_mutex);
			auto            current = vaddr;
			while (current < end) {
				MapEntry entry {};
				if (!FindContainingUnlocked(current, 1, &entry)) {
					return false;
				}
				const auto part_size = std::min(end, entry.vaddr + entry.size) - current;
				targets.push_back(
				    {current, part_size, entry.backing_offset + current - entry.vaddr, entry.mode});
				current += part_size;
			}
		}

		std::vector<MapEntry> removed;
		removed.reserve(targets.size());
		for (const auto& entry: targets) {
			bool part_placeholder_preserved = false;
			if (!UnmapOne(entry.vaddr, entry.size, &part_placeholder_preserved)) {
				for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
					EXIT_IF(!MapFixed(it->vaddr, it->size, it->backing_offset, it->mode));
				}
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = false;
				}
				return false;
			}
			EXIT_IF(!part_placeholder_preserved);
			removed.push_back(entry);
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		if (removed.size() > 1 && !CoalescePlaceholders(vaddr, size)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				EXIT_IF(!MapFixed(it->vaddr, it->size, it->backing_offset, it->mode));
			}
			return false;
		}
#endif

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = true;
		}
		return true;
	}

	bool UnmapOne(uint64_t vaddr, uint64_t size, bool* placeholder_preserved = nullptr) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_store_unmaps_before_failure == 0) {
			g_test_backing_store_unmaps_before_failure = UINT32_MAX;
			return false;
		}
		if (g_test_backing_store_unmaps_before_failure != UINT32_MAX) {
			g_test_backing_store_unmaps_before_failure--;
		}
#endif

		MapEntry old {};
		{
			std::lock_guard lock(m_mutex);
			auto            next = m_maps.upper_bound(vaddr);
			if (next != m_maps.begin()) {
				auto entry = std::prev(next);
				if (vaddr + size <= entry->second.vaddr + entry->second.size) {
					old = entry->second;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					m_maps.erase(entry);
					m_maps_epoch.fetch_add(1, std::memory_order_release);
#endif
				}
			}
		}

		if (old.size == 0) {
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t right_size = old.vaddr + old.size - right_addr;
		const bool exact_unmap = (vaddr == old.vaddr && size == old.size);
		if (exact_unmap) {
			const bool unmapped =
			    UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size);
			if (!unmapped) {
				std::lock_guard lock(m_mutex);
				m_maps.emplace(old.vaddr, old);
				m_maps_epoch.fetch_add(1, std::memory_order_release);
				return false;
			}
			if (placeholder_preserved != nullptr) {
				*placeholder_preserved = true;
			}
			return true;
		}

		// The parts that stay mapped vanish until they are mapped again (see
		// TransientUnmapWindows), and come back with the view's protection: give them back their
		// own (guest mprotect, GPU write and read tracking) before faults there are retried.
		const auto                   kept_protections = SnapshotProtections(old, vaddr, size);
		const uint64_t               old_end          = old.vaddr + old.size;
		TransientUnmapWindows::Scope transient(old.vaddr, old_end);
		if (UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)) {
			bool ok = true;
			if (left_size != 0 && left_size != old.size) {
				ok = SplitPlaceholder(old.vaddr, left_size) && ok;
			}
			if (right_size != 0) {
				ok = SplitPlaceholder(vaddr, size) && ok;
			}
			if (left_size != 0) {
				ok = MapFixed(old.vaddr, left_size, old.backing_offset, old.mode) && ok;
			}
			if (right_size != 0) {
				ok = MapFixed(right_addr, right_size,
				              old.backing_offset + (right_addr - old.vaddr), old.mode) &&
				     ok;
			}
			if (ok) {
				RestoreProtections(kept_protections);
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = true;
				}
				return true;
			}

			if (left_size != 0 && Contains(old.vaddr, left_size)) {
				EXIT_IF(!UnmapOne(old.vaddr, left_size));
			}
			if (right_size != 0 && Contains(right_addr, right_size)) {
				EXIT_IF(!UnmapOne(right_addr, right_size));
			}
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(old.vaddr), old.size,
			                    MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) == 0);
			EXIT_IF(!MapFixed(old.vaddr, old.size, old.backing_offset, old.mode));
			RestoreProtections(kept_protections);
			return false;
		}
		std::lock_guard lock(m_mutex);
		m_maps.emplace(old.vaddr, old);
		m_maps_epoch.fetch_add(1, std::memory_order_release);
		return false;
#else
		// Replace only the requested range so concurrent accesses and GPU watch
		// protections on the surviving mappings remain intact.
		if (!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(vaddr), size)) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		RemoveMappingsUnlocked(vaddr, size);

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = true;
		}
		return true;
#endif
	}

	bool Contains(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
		const auto      end     = vaddr + size;
		auto            current = vaddr;

		while (current < end) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) {
				return false;
			}
			const auto entry_end = entry.vaddr + entry.size;
			if (entry_end <= current) {
				return false;
			}
			current = std::min(end, entry_end);
		}

		return true;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	static uint64_t HostPageSize() {
		static const uint64_t page = static_cast<uint64_t>(std::max<long>(sysconf(_SC_PAGESIZE), 4096));
		return page;
	}

	// Resident bytes of the backing file in [lo, hi) (page rounded outwards), by mincore.
	[[nodiscard]] uint64_t ResidentBytesInFile(uint64_t lo, uint64_t hi) const {
		const uint64_t page     = HostPageSize();
		const uint64_t first    = lo / page;
		const uint64_t last     = std::min((hi + page - 1) / page, m_size / page);
		constexpr uint64_t CHUNK = 1u << 16; // pages per mincore call
		uint64_t           pages = 0;
		std::vector<unsigned char> vec(std::min<uint64_t>(CHUNK, last > first ? last - first : 0));
		for (uint64_t at = first; at < last; at += CHUNK) {
			const uint64_t count = std::min(CHUNK, last - at);
			if (mincore(m_backing_base + at * page, count * page, vec.data()) != 0) {
				continue;
			}
			for (uint64_t i = 0; i < count; i++) {
				pages += vec[i] & 1u;
			}
		}
		return pages * page;
	}

	// Bytes of [lo, hi) that were never written (no data, not swapped) according to the backing
	// file. Swapped-out shmem pages count as data for SEEK_DATA.
	[[nodiscard]] uint64_t HoleBytesInFile(uint64_t lo, uint64_t hi) const {
		uint64_t hole = 0;
		uint64_t pos  = lo;
		while (pos < hi) {
			const off_t data = lseek(m_fd, static_cast<off_t>(pos), SEEK_DATA);
			if (data < 0 || static_cast<uint64_t>(data) >= hi) {
				hole += hi - pos;
				break;
			}
			if (static_cast<uint64_t>(data) > pos) {
				hole += static_cast<uint64_t>(data) - pos;
			}
			const off_t next_hole = lseek(m_fd, data, SEEK_HOLE);
			if (next_hole < 0) {
				break;
			}
			pos = static_cast<uint64_t>(next_hole);
		}
		return hole;
	}

	// True when no byte of the backing file in [lo, hi) holds data (SEEK_DATA finds nothing before hi).
	[[nodiscard]] bool FileRangeIsHole(uint64_t lo, uint64_t hi) const {
		const off_t data = lseek(m_fd, static_cast<off_t>(lo), SEEK_DATA);
		if (data < 0) {
			return errno == ENXIO; // no data at or after lo
		}
		return static_cast<uint64_t>(data) >= hi;
	}

	static constexpr uint64_t SCAN_READ_MIN = 16 * 1024;

	// Copies a guest backing range into `destination`. With --mem-residency, large reads are
	// classified by page residency. With --mem-hole-reads, pages that are not resident are read with
	// pread: a read of a never-written page of a shared mapping allocates a zero page, pread of the
	// same page returns zeros and allocates nothing.
	void CopyFromBacking(uint8_t* destination, uint64_t offset, uint64_t bytes) const {
		if ((g_mem_residency_on || g_mem_hole_reads_on) && bytes >= SCAN_READ_MIN) {
			ScanCopyFromBacking(destination, offset, bytes);
			return;
		}
		std::memcpy(destination, m_backing_base + offset, bytes);
	}

	// pread of [offset, offset + bytes) into `destination`; false when it could not read it all.
	[[nodiscard]] bool PreadFromBacking(uint8_t* destination, uint64_t offset, uint64_t bytes) const {
		uint64_t done = 0;
		while (done < bytes) {
			const ssize_t n =
			    pread(m_fd, destination + done, bytes - done, static_cast<off_t>(offset + done));
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			done += static_cast<uint64_t>(n);
		}
		return true;
	}

	void ScanCopyFromBacking(uint8_t* destination, uint64_t offset, uint64_t bytes) const {
		auto&          counts = g_backing_read_tag == BackingReadTag::Image ? g_backing_reads.image
		                                                                    : g_backing_reads.other;
		const uint64_t page   = HostPageSize();
		const uint64_t end    = offset + bytes;
		counts.reads.fetch_add(1, std::memory_order_relaxed);
		counts.bytes.fetch_add(bytes, std::memory_order_relaxed);
		constexpr uint64_t         CHUNK = 1u << 16;
		std::vector<unsigned char> vec;
		uint64_t                   copied      = offset; // bytes before this offset are in `destination`
		uint64_t                   nonresident = 0;
		uint64_t                   hole        = 0;
		uint64_t                   preads      = 0;
		const auto copy_run = [&](uint64_t lo, uint64_t hi, bool resident) {
			if (hi <= lo) {
				return;
			}
			uint8_t* to = destination + (lo - offset);
			if (!resident && g_mem_hole_reads_on) {
				if (PreadFromBacking(to, lo, hi - lo)) {
					preads += hi - lo;
					return;
				}
				g_backing_reads.pread_failures.fetch_add(1, std::memory_order_relaxed);
			}
			std::memcpy(to, m_backing_base + lo, hi - lo);
		};
		for (uint64_t at = offset / page; at * page < end;) {
			const uint64_t last = std::min<uint64_t>(at + CHUNK, (end + page - 1) / page);
			vec.resize(last - at);
			if (mincore(m_backing_base + at * page, (last - at) * page, vec.data()) != 0) {
				break; // copy the rest unclassified
			}
			for (uint64_t i = 0; i < last - at;) {
				const bool resident = (vec[i] & 1u) != 0;
				uint64_t   j        = i + 1;
				while (j < last - at && ((vec[j] & 1u) != 0) == resident) {
					j++;
				}
				const uint64_t lo = std::max(offset, (at + i) * page);
				const uint64_t hi = std::min(end, (at + j) * page);
				if (!resident) {
					nonresident += hi - lo;
					if (g_mem_residency_on) {
						hole += HoleBytesInFile(lo, hi);
					}
				}
				copy_run(lo, hi, resident);
				copied = hi;
				i      = j;
			}
			at = last;
		}
		if (copied < end) {
			std::memcpy(destination + (copied - offset), m_backing_base + copied, end - copied);
		}
		if (g_mem_residency_on) {
			counts.nonresident.fetch_add(nonresident, std::memory_order_relaxed);
			counts.hole.fetch_add(hole, std::memory_order_relaxed);
		}
		counts.pread_bytes.fetch_add(preads, std::memory_order_relaxed);
	}
#else
	void CopyFromBacking(uint8_t* destination, uint64_t offset, uint64_t bytes) const {
		std::memcpy(destination, m_backing_base + offset, bytes);
	}
#endif

private:
	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
	};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// Per host page of the backing object: how many views map it and whether the guest freed it.
	static constexpr uint8_t PAGE_LIVE      = 0; // in use, or never released
	static constexpr uint8_t PAGE_PENDING   = 1; // freed by the guest, a view still maps it
	static constexpr uint8_t PAGE_CANDIDATE = 2; // freed and unmapped: safe to punch
	static constexpr uint8_t PAGE_PUNCHED   = 3; // freed, unmapped and returned to the host

	void InitTrackingUnlocked() {
		const long host_page = sysconf(_SC_PAGESIZE);
		if (host_page <= 0 || m_size % static_cast<uint64_t>(host_page) != 0) {
			return;
		}
		m_host_page = static_cast<uint64_t>(host_page);
		m_refs.assign(m_size / m_host_page, 0);
		m_pstate.assign(m_size / m_host_page, PAGE_LIVE);
		m_tracking = true;
	}

	// Pages touched by a view: every page the byte range overlaps.
	void OverlapPages(uint64_t offset, uint64_t size, uint64_t* first, uint64_t* last) const {
		*first = offset / m_host_page;
		*last  = (offset + size + m_host_page - 1) / m_host_page;
		if (*last > m_pstate.size()) {
			*last = m_pstate.size();
		}
	}

	void TrackMapUnlocked(uint64_t offset, uint64_t size) {
		if (!m_tracking) {
			return;
		}
		uint64_t first = 0;
		uint64_t last  = 0;
		OverlapPages(offset, size, &first, &last);
		uint64_t reused = 0;
		uint64_t reused_punched = 0;
		for (uint64_t page = first; page < last; page++) {
			if (m_refs[page] != UINT16_MAX) {
				m_refs[page]++;
			}
			switch (m_pstate[page]) {
				case PAGE_CANDIDATE:
					reused += m_host_page;
					break;
				case PAGE_PUNCHED: reused_punched += m_host_page; break;
				default: break;
			}
			m_pstate[page] = PAGE_LIVE;
		}
		if (reused != 0) {
			g_mem_stats.realloc_bytes.fetch_add(reused, std::memory_order_relaxed);
		}
		if (reused_punched != 0) {
			g_mem_stats.realloc_after_punch_bytes.fetch_add(reused_punched,
			                                                std::memory_order_relaxed);
		}
	}

	void TrackUnmapUnlocked(uint64_t offset, uint64_t size) {
		if (!m_tracking) {
			return;
		}
		uint64_t first = 0;
		uint64_t last  = 0;
		OverlapPages(offset, size, &first, &last);
		for (uint64_t page = first; page < last; page++) {
			if (m_refs[page] != 0 && m_refs[page] != UINT16_MAX) {
				m_refs[page]--;
			}
		}
		FinalizeUnlocked(first, last);
	}

	void TrackReleaseUnlocked(uint64_t offset, uint64_t size) {
		// Only whole host pages are ours to give back; the partial edges stay as they are.
		const uint64_t first = (offset + m_host_page - 1) / m_host_page;
		const uint64_t last  = std::min<uint64_t>((offset + size) / m_host_page, m_pstate.size());
		if (first >= last) {
			return;
		}
		for (uint64_t page = first; page < last; page++) {
			if (m_pstate[page] == PAGE_LIVE) {
				m_pstate[page] = PAGE_PENDING;
			}
		}
		FinalizeUnlocked(first, last);
	}

	// Pending pages that no view maps any more become candidates; each run is counted and punched.
	void FinalizeUnlocked(uint64_t first, uint64_t last) {
		for (uint64_t page = first; page < last;) {
			if (m_pstate[page] != PAGE_PENDING || m_refs[page] != 0) {
				page++;
				continue;
			}
			uint64_t end = page;
			while (end < last && m_pstate[end] == PAGE_PENDING && m_refs[end] == 0) {
				end++;
			}
			HandleCandidateRunUnlocked(page, end);
			page = end;
		}
	}

	uint64_t ResidentBytes(uint64_t first, uint64_t last) const {
		uint64_t resident = 0;
		// Bounded chunks keep the vector small for very large runs.
		constexpr uint64_t CHUNK = 1u << 18;
		std::vector<unsigned char> vec;
		for (uint64_t page = first; page < last; page += CHUNK) {
			const uint64_t count = std::min(CHUNK, last - page);
			vec.resize(count);
#if defined(__APPLE__)
			continue;
#else
			if (mincore(m_backing_base + page * m_host_page, count * m_host_page, vec.data()) !=
			    0) {
				continue;
			}
#endif
			for (uint64_t i = 0; i < count; i++) {
				resident += (vec[i] & 1u) != 0 ? m_host_page : 0;
			}
		}
		return resident;
	}

	void HandleCandidateRunUnlocked(uint64_t first, uint64_t last) {
		const uint64_t bytes = (last - first) * m_host_page;
		for (uint64_t page = first; page < last; page++) {
			m_pstate[page] = PAGE_CANDIDATE;
		}
		uint64_t resident = 0;
		if (g_mem_stats_on) {
			resident = ResidentBytes(first, last);
			g_mem_stats.released_unmapped_bytes.fetch_add(bytes, std::memory_order_relaxed);
			g_mem_stats.released_unmapped_resident_bytes.fetch_add(resident,
			                                                       std::memory_order_relaxed);
		}
		if (!g_mem_punch_on) {
			return;
		}
		const uint64_t offset = first * m_host_page;
#if defined(__APPLE__)
		constexpr bool punched = false; // no fallocate(PUNCH_HOLE) here
#else
		const bool punched = fallocate(m_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		                               static_cast<off_t>(offset), static_cast<off_t>(bytes)) == 0;
#endif
		if (!punched) {
			g_mem_stats.punch_failures.fetch_add(1, std::memory_order_relaxed);
			if (g_mem_stats_on) {
				Log::WriteToConsoleAndLog(
				    "[mem-punch] FAILED off=0x" + ToHex(offset) + " len=" + std::to_string(bytes) +
				    " errno=" + std::to_string(errno) + "\n");
			}
			return;
		}
		for (uint64_t page = first; page < last; page++) {
			m_pstate[page] = PAGE_PUNCHED;
		}
		if (g_mem_stats_on) {
			g_mem_stats.punch.Add(bytes);
			g_mem_stats.punch_resident_bytes.fetch_add(resident, std::memory_order_relaxed);
			Log::WriteToConsoleAndLog("[mem-punch] off=0x" + ToHex(offset) + " len=" +
			                          std::to_string(bytes) + " resident=" + std::to_string(resident) +
			                          "\n");
		}
	}

	static std::string ToHex(uint64_t value) {
		char buffer[24];
		snprintf(buffer, sizeof(buffer), "%" PRIx64, value);
		return buffer;
	}

	void RemoveMappingsUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end = vaddr + size;
		auto       it = m_maps.upper_bound(vaddr);
		if (it != m_maps.begin()) {
			--it;
		}
		while (it != m_maps.end() && it->first < end) {
			const auto old     = it->second;
			const auto old_end = old.vaddr + old.size;
			if (old_end <= vaddr) {
				++it;
				continue;
			}
			TrackUnmapUnlocked(old.backing_offset + std::max(vaddr, old.vaddr) - old.vaddr,
			                   std::min(end, old_end) - std::max(vaddr, old.vaddr));
			it = m_maps.erase(it);
			if (old.vaddr < vaddr) {
				m_maps.emplace(old.vaddr,
				               MapEntry {old.vaddr, vaddr - old.vaddr, old.backing_offset, old.mode});
			}
			if (end < old_end) {
				m_maps.emplace(end, MapEntry {end, old_end - end,
				                             old.backing_offset + end - old.vaddr, old.mode});
			}
		}
		m_maps_epoch.fetch_add(1, std::memory_order_release);
	}
#endif

	bool FindContainingUnlocked(uint64_t vaddr, uint64_t size, MapEntry* out) const {
		EXIT_IF(out == nullptr);

		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		auto next = m_maps.upper_bound(vaddr);
		if (next == m_maps.begin()) {
			return false;
		}
		const auto& entry = std::prev(next)->second;
		if (vaddr + size <= entry.vaddr + entry.size) {
			*out = entry;
			return true;
		}

		return false;
	}

	[[nodiscard]] bool RangeOk(uint64_t backing_offset, uint64_t size) const {
		return size != 0 && backing_offset < m_size && size <= m_size - backing_offset;
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	static bool ApplyProtection(void* ptr, uint64_t size, VirtualMemory::Mode mode) {
		DWORD old_protect = 0;
		return VirtualProtect(ptr, size, GetProtect(mode), &old_protect) != 0;
	}

	struct KeptProtection {
		uint64_t vaddr   = 0;
		uint64_t size    = 0;
		DWORD    protect = 0;
	};

	// The protections, other than the view's own, of the parts of a view that stay mapped when
	// [vaddr, vaddr + size) is unmapped from it.
	static std::vector<KeptProtection> SnapshotProtections(const MapEntry& view, uint64_t vaddr,
	                                                       uint64_t size) {
		std::vector<KeptProtection> kept;
		const auto                  view_protect = GetProtect(view.mode);
		for (const auto& [start, end]:
		     {std::pair {view.vaddr, vaddr}, std::pair {vaddr + size, view.vaddr + view.size}}) {
			for (auto current = start; current < end;) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0) {
					break;
				}
				const auto region_end =
				    std::min(end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
				if (region_end <= current) {
					break;
				}
				if (info.State == MEM_COMMIT && info.Protect != view_protect) {
					kept.push_back({current, region_end - current, info.Protect});
				}
				current = region_end;
			}
		}
		return kept;
	}

	static void RestoreProtections(const std::vector<KeptProtection>& kept) {
		for (const auto& part: kept) {
			DWORD old_protect = 0;
			if (VirtualProtect(reinterpret_cast<void*>(part.vaddr), part.size, part.protect,
			                   &old_protect) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t view split: protection 0x%lx of [0x%016" PRIx64 ", 0x%016" PRIx64
				           ") not restored, error=0x%08" PRIx32 "\n",
				           static_cast<unsigned long>(part.protect), part.vaddr,
				           part.vaddr + part.size, static_cast<uint32_t>(GetLastError()));
			}
		}
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return true;
		}
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	void* ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                             VirtualMemory::Mode mode, FailureReason* failure_reason) {
		const DWORD map_protect =
		    mode == VirtualMemory::Mode::NoAccess ? PAGE_READWRITE : GetProtect(mode);
		void* ptr =
		    MapViewOfFile3(m_handle, GetCurrentProcess(), reinterpret_cast<void*>(vaddr),
		                   backing_offset, size, MEM_REPLACE_PLACEHOLDER, map_protect, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return nullptr;
		}

		if (mode == VirtualMemory::Mode::NoAccess && !ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE m_handle = nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	[[nodiscard]] static uint64_t GetMappableTestSize() { return 0x4000; }

	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	void* MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (const auto callback = g_test_before_backing_map) {
			g_test_before_backing_map = nullptr;
			callback(vaddr, size);
		}
#endif
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode),
		                 MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return nullptr;
		}
		return ptr;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		void* ptr = mmap(vaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		return ptr != MAP_FAILED && ptr == vaddr;
	}

	int m_fd = -1;

	bool                 m_tracking = false;
	uint64_t             m_host_page = 0;
	std::vector<uint16_t> m_refs;   // views mapping each host page
	std::vector<uint8_t>  m_pstate; // PAGE_* per host page
#endif

	uint8_t*                     m_backing_base = nullptr;
	uint64_t                     m_size         = 0;
	std::shared_mutex            m_mutex;
	std::map<uint64_t, MapEntry> m_maps;
	// Moves (under the unique lock, after the change) whenever m_maps changes: a mapping entry remembered
	// together with the epoch it was read at is still the entry while the epoch has not moved.
	std::atomic<uint64_t>        m_maps_epoch {1};
};

class GuestAddressSpace {
public:
	explicit GuestAddressSpace(uint64_t backing_size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
		ReserveGuestRegions();
		m_backing = new GuestBackingStore(backing_size);
		if (!m_backing->IsAvailable()) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory. Windows commits\n"
			     "this up front, so the paging file is usually what needs to be larger.\n",
			     backing_size / (1024U * 1024U));
#else
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory\n",
			     backing_size / (1024U * 1024U));
#endif
		}
	}

	~GuestAddressSpace() {
		delete m_backing;
		m_backing = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [start, size]: m_owned) {
			const auto end     = start + size;
			auto       current = start;
			while (current < end) {
				MEMORY_BASIC_INFORMATION info {};
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.State == MEM_COMMIT) {
					EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					EXIT_IF(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			munmap(reinterpret_cast<void*>(start), size);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] GuestBackingStore& GetBacking() { return *m_backing; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing->GetBackingBase(); }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing->GetBackingSize(); }

	bool ZeroBacking(uint64_t backing_offset, uint64_t size) {
		return m_backing->ZeroRange(backing_offset, size);
	}

	void NoteBackingReleased(uint64_t backing_offset, uint64_t size) {
		m_backing->NoteReleased(backing_offset, size);
	}

	[[nodiscard]] GuestBackingStore::ReleaseGauges GetBackingReleaseGauges() {
		return m_backing->GetReleaseGauges();
	}

	[[nodiscard]] bool Owns(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return vaddr >= range.first && vaddr + size <= range.first + range.second;
		});
	}

	[[nodiscard]] bool OverlapsOwned(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		return m_backing->TryWriteBacking(vaddr, data, size);
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		auto            mapping = m_mapped.upper_bound(vaddr);
		if (mapping != m_mapped.begin()) {
			--mapping;
		}
		for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
			const auto& region = mapping->second;
			if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
				return false;
			}
		}
		return m_backing->TryReadSparseBacking(vaddr, data, size);
	}

	// See GuestBackingStore::RangeIsNeverWritten. Ranges that touch private (non-file) mappings
	// are never reported as holes.
	bool RangeIsNeverWritten(uint64_t vaddr, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		{
			std::shared_lock lock(m_mutex);
			auto            mapping = m_mapped.upper_bound(vaddr);
			if (mapping != m_mapped.begin()) {
				--mapping;
			}
			for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
				const auto& region = mapping->second;
				if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
					return false;
				}
			}
		}
		return m_backing->RangeIsNeverWritten(vaddr, size);
	}

	bool RangeReadsAsZero(uint64_t vaddr, uint64_t size) {
		return Owns(vaddr, size) && m_backing->RangeReadsAsZero(vaddr, size);
	}

	bool BackingContains(uint64_t vaddr, uint64_t size) { return m_backing->Contains(vaddr, size); }

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr) {
		std::lock_guard lock(m_mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		const bool replacing = ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
#else
		constexpr bool replacing = false;
#endif
		if (!replacing && !ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;
		if (m_backing->MapFixed(vaddr, size, backing_offset, host_mode, failure_reason)) {
			if (!replacing) {
				EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Backing));
			}
			return true;
		}
		if (!replacing) {
			AddFreeUnlocked(vaddr, size);
		}
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
		bool placeholder_preserved = false;
		if (!m_backing->Unmap(vaddr, size, &placeholder_preserved)) {
			return false;
		}
		if (!placeholder_preserved) {
			return false;
		}
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	uint64_t ReserveAligned(uint64_t search_addr, uint64_t size, uint64_t alignment) {
		if (size == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment != 0 ? alignment : PageSize(), PageSize());
		std::lock_guard lock(m_mutex);
		if (search_addr == 0) {
			auto addr = FindFreeAlignedUnlocked(HOST_SYSTEM_MANAGED_MIN,
			                                    HOST_SYSTEM_MANAGED_MAX + 1u, size, alignment);
			return addr != 0 ? addr
			                 : FindFreeAlignedUnlocked(HOST_USER_MIN, HOST_USER_MAX + 1u, size,
			                                           alignment);
		}
		return FindFreeAlignedUnlocked(search_addr, HOST_USER_MAX + 1u, size, alignment);
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		if (size == 0 || alignment == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::lock_guard lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment);
	}

	bool ReserveFixed(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool TestContainsFree(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}
#endif

	bool ReleaseFree(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
		                          GetProtect(host_mode), nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			AddFreeUnlocked(vaddr, size);
			return false;
		}
		EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private));
		return true;
#else
		if (VirtualMemory::Protect(vaddr, size, host_mode)) {
			EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private));
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		while (current < end) {
			const auto it       = FindMappedContainingUnlocked(current);
			const auto part_end = std::min(end, it->second.vaddr + it->second.size);
			regions.emplace_back(current, part_end - current);
			current = part_end;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [region_vaddr, region_size]: regions) {
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		EXIT_IF(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Private));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	bool SelfTest() {
		const auto addr = ReserveAligned(0, PageSize(), PageSize());
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: 16 KiB range unavailable\n");
			return false;
		}
		if (!Commit(addr, PageSize(), VirtualMemory::Mode::ReadWrite)) {
			LOGF_COLOR(Log::Color::Red, "\t guest address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, PageSize())) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: release-committed failed\n");
			return false;
		}

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
	};

	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		if (alignment == 0) {
			return 0;
		}
		const auto remainder = value % alignment;
		const auto increment = (remainder != 0 ? alignment - remainder : 0);
		return increment <= UINT64_MAX - value ? value + increment : 0;
	}

	static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
		return value & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	[[nodiscard]] bool IsGranularityAligned(uint64_t value) const {
		return (value & (m_granularity - 1u)) == 0;
	}

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start < range_end &&
			    !VirtualMemory::Protect(range_start, range_end - range_start, mode)) {
				return false;
			}
		}
		return true;
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		auto it = FindContaining(vaddr, size);
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		it = m_free.find(vaddr);
		if (it == m_free.end() || it->second != size) {
			return false;
		}
		m_free.erase(it);
		return true;
	}

	std::map<uint64_t, MappedRegion>::iterator FindMappedContainingUnlocked(uint64_t vaddr) {
		auto next = m_mapped.upper_bound(vaddr);
		if (next == m_mapped.begin()) {
			return m_mapped.end();
		}
		auto it = std::prev(next);
		return vaddr < it->second.vaddr + it->second.size ? it : m_mapped.end();
	}

	bool ContainsMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.type != type) {
				return false;
			}
			current = std::min(end, it->second.vaddr + it->second.size);
		}
		return true;
	}

	bool AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end  = vaddr + size;
		auto       next = m_mapped.lower_bound(vaddr);
		if (next != m_mapped.end() && next->second.vaddr < end) {
			return false;
		}
		if (next != m_mapped.begin()) {
			const auto& previous = std::prev(next)->second;
			if (previous.vaddr + previous.size > vaddr) {
				return false;
			}
		}
		return m_mapped.emplace(vaddr, MappedRegion {vaddr, size, type}).second;
	}

	bool RemoveMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (!ContainsMappedUnlocked(vaddr, size, type)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			auto       it       = FindMappedContainingUnlocked(current);
			const auto original = it->second;
			const auto part_end = std::min(end, original.vaddr + original.size);
			m_mapped.erase(it);
			if (original.vaddr < current) {
				const auto left_size = current - original.vaddr;
				EXIT_IF(!AddMappedUnlocked(original.vaddr, left_size, original.type));
			}
			if (part_end < original.vaddr + original.size) {
				const auto right_size = original.vaddr + original.size - part_end;
				EXIT_IF(!AddMappedUnlocked(part_end, right_size, original.type));
			}
			current = part_end;
		}
		return true;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}
#endif

	void ReserveGuestRegions() {
		const std::array<std::pair<uint64_t, uint64_t>, 3> regions {{
		    {HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_RESERVED_MAX + 1u},
		    {HOST_USER_MIN, HOST_USER_MAX + 1u},
		    {kExtendedMemoryBase, kExtendedMemoryBase + kExtendedMemorySize},
		}};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		constexpr uint64_t MinimumRegionSize = 0x1000000;
		const auto         process           = GetCurrentProcess();
		for (const auto& [guest_start, guest_end]: regions) {
			auto next_addr = guest_start;
			while (next_addr < guest_end) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(next_addr), &info, sizeof(info)) == 0) {
					EXIT("failed to inspect guest address space at 0x%016" PRIx64 "\n", next_addr);
				}

				const auto region_base = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end = std::min<uint64_t>(
				    guest_end, region_base + static_cast<uint64_t>(info.RegionSize));
				const auto reserve_start =
				    AlignUp(std::max(region_base, guest_start), m_granularity);
				const auto reserve_end = AlignDown(region_end, m_granularity);

				if (info.State == MEM_FREE && reserve_end > reserve_start &&
				    reserve_end - reserve_start > MinimumRegionSize) {
					auto* ptr = VirtualAlloc2(
					    process, reinterpret_cast<void*>(reserve_start), reserve_end - reserve_start,
					    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
					if (reinterpret_cast<uint64_t>(ptr) != reserve_start) {
						EXIT("failed to reserve guest address space at 0x%016" PRIx64
						     ", size 0x%016" PRIx64 "\n",
						     reserve_start, reserve_end - reserve_start);
					}
					AddFreeUnlocked(reserve_start, reserve_end - reserve_start);
					m_owned.emplace_back(reserve_start, reserve_end - reserve_start);
				}

				if (region_end <= next_addr) {
					EXIT("guest address-space scan did not advance at 0x%016" PRIx64 "\n", next_addr);
				}
				next_addr = region_end;
			}
		}
#else
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		EXIT_IF(maps == nullptr);

		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long mapping_start = 0;
			unsigned long long mapping_end   = 0;
			if (sscanf(line, "%llx-%llx", &mapping_start, &mapping_end) == 2) {
				occupied.emplace_back(static_cast<uint64_t>(mapping_start),
				                      static_cast<uint64_t>(mapping_end));
			}
		}
		fclose(maps);

		auto reserve_range = [this](uint64_t start, uint64_t end) {
			start = AlignUp(start, PageSize());
			end   = AlignDown(end, PageSize());
			if (start == 0 || end <= start) {
				return;
			}

			const auto size = end - start;
			int        flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif

			void* ptr = mmap(reinterpret_cast<void*>(start), size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, size);
				}
				return;
			}

			AddFreeUnlocked(start, size);
			m_owned.emplace_back(start, size);
		};

		for (const auto& [region_start, region_end]: regions) {
			auto current = region_start;

			for (const auto& [mapping_start, mapping_end]: occupied) {
				if (mapping_end <= current) {
					continue;
				}
				if (mapping_start >= region_end) {
					break;
				}

				if (mapping_start > current) {
					reserve_range(current, std::min(mapping_start, region_end));
				}

				current = std::max(current, mapping_end);
				if (current >= region_end) {
					break;
				}
			}

			if (current < region_end) {
				reserve_range(current, region_end);
			}
		}
#else
		for (const auto& [start, end]: regions) {
			int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif
			void* ptr = mmap(reinterpret_cast<void*>(start), end - start, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, end - start);
				}
				EXIT("failed to reserve guest address space at 0x%016" PRIx64
				     ", size 0x%016" PRIx64 "\n",
				     start, end - start);
			}
			AddFreeUnlocked(start, end - start);
			m_owned.emplace_back(start, end - start);
		}
#endif
#endif
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment) {
		if (search_start >= search_end || size > search_end - search_start) {
			return 0;
		}
		for (const auto& [base, range_size]: m_free) {
			const auto candidate = AlignUp(std::max(base, search_start), alignment);
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		return 0;
	}

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it  = std::prev(next);
		auto end = it->first + it->second;
		return (vaddr >= it->first && vaddr + size <= end ? it : m_free.end());
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base == vaddr && end == vaddr + size) {
			return true;
		}

		if (base != vaddr) {
			const auto left_size = vaddr - base;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           base, end - base, vaddr, size, static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           it->first, it->second, vaddr, size,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0) {
			return;
		}
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (CoalescePlaceholders(base, end - base)) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
#else
			m_free.erase(first, last);
			m_free.emplace(base, end - base);
#endif
		}
	}

	void RestorePlaceholder(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		// The address remains owned for the process lifetime; MAP_FIXED only replaces one of our
		// mappings and never acquires a new guest address.
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		EXIT_IF(ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != vaddr);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::shared_mutex                          m_mutex;
	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
};
