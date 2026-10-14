#include "graphics/host_gpu/memoryTracker.h"

#include "common/alignment.h"
#include "common/assert.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics {

namespace {

// KYTY_TRACKER_LOCKFREE_VERIFY=1: every lock-free query also takes the region lock and checks that
// the mirrors equal the dirty bits (the invariant the lock-free answer rests on) and that the
// locked answer equals the lock-free one. Under the lock nothing can change, so there is no race to
// excuse a mismatch. A line with the totals goes to stdout every 2^22 queries.
struct LockFreeVerifier {
	bool                  enabled = std::getenv("KYTY_TRACKER_LOCKFREE_VERIFY") != nullptr;
	std::atomic<uint64_t> checked {0};
	std::atomic<uint64_t> mismatches {0};
	void Note(bool agree) {
		const auto n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
		if (!agree) {
			mismatches.fetch_add(1, std::memory_order_relaxed);
			std::printf("MemoryTracker lock-free verify: MISMATCH at query %" PRIu64 "\n", n);
			std::fflush(stdout);
		}
		if ((n & ((1u << 22) - 1u)) == 0) {
			std::printf("MemoryTracker lock-free verify: %" PRIu64 " queries checked against the locked path, %" PRIu64
			            " mismatches\n",
			            n, mismatches.load(std::memory_order_relaxed));
			std::fflush(stdout);
		}
	}
};
LockFreeVerifier g_verifier;

template <DirtySource source>
bool LockFreeQuery(RegionManager* manager, uint64_t offset, uint64_t bytes) {
	if (!g_verifier.enabled) {
		return manager->IsModifiedLockFree<source>(offset, bytes);
	}
	std::scoped_lock lock(manager->lock);
	const bool       fast   = manager->IsModifiedLockFree<source>(offset, bytes);
	const bool       locked = manager->IsModified<source>(offset, bytes);
	g_verifier.Note(manager->MirrorsMatch() && fast == locked);
	return locked;
}

} // namespace

bool MemoryTracker::CpuModifiedLockFree(RegionManager* manager, uint64_t offset, uint64_t bytes) {
	return LockFreeQuery<DirtySource::Cpu>(manager, offset, bytes);
}

bool MemoryTracker::GpuModifiedLockFree(RegionManager* manager, uint64_t offset, uint64_t bytes) {
	return LockFreeQuery<DirtySource::Gpu>(manager, offset, bytes);
}

static_assert(std::atomic<void*>::is_always_lock_free);

MemoryTracker::MemoryTracker(PageManager& page_manager): m_page_manager(page_manager) {
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
}

MemoryTracker::~MemoryTracker() = default;

#if KYTY_BUILD == KYTY_BUILD_DEBUG
void MemoryTracker::ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                          const char* operation) const noexcept {
	if (!GuestRange {vaddr, size}.Valid() || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("MemoryTracker: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		if (!dirty.Intersects(page, TRACKER_PAGE_SIZE)) {
			EXIT("MemoryTracker: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                              const char* operation) {
	ValidateRange(vaddr, size);
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		const bool has_dirty_bytes = dirty.Intersects(page, TRACKER_PAGE_SIZE);
		if (IsRegionGpuModified(page, TRACKER_PAGE_SIZE) != has_dirty_bytes) {
			EXIT("MemoryTracker: tracker and byte ownership disagree, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}
#endif

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("invalid memory tracker range\n");
	}
}

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager = std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	m_regions[index].store(ptr, std::memory_order_release);
	// a new region starts out fully CPU-modified (K15)
	BdaSync::NoteCpuModified(index * TRACKER_REGION_SIZE, TRACKER_REGION_SIZE);
	return ptr;
}

uint32_t MemoryTracker::RangeEpochs(uint64_t vaddr, uint64_t size, uint32_t (&out)[2]) const noexcept {
	if (size == 0 || vaddr / TRACKER_REGION_SIZE >= REGION_COUNT ||
	    (vaddr + size - 1) / TRACKER_REGION_SIZE >= REGION_COUNT || vaddr + size < vaddr) {
		return 0;
	}
	const uint64_t first = vaddr / TRACKER_REGION_SIZE;
	const uint64_t last  = (vaddr + size - 1) / TRACKER_REGION_SIZE;
	if (last - first > 1) {
		return 0;
	}
	uint32_t count = 0;
	for (uint64_t index = first; index <= last; ++index) {
		const auto* manager = m_regions[index].load(std::memory_order_acquire);
		if (manager == nullptr) {
			return 0;
		}
		out[count++] = manager->Epoch();
	}
	return count;
}

bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

MemoryTracker::DirtyState MemoryTracker::QueryDirty(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	DirtyState state;
	Iterate<true>(vaddr, size, [&state](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		state.cpu = state.cpu || manager->IsModified<DirtySource::Cpu>(offset, bytes);
		state.gpu = state.gpu || manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
	return state;
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionCpuModifiedLockFree(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		return CpuModifiedLockFree(manager, offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModifiedLockFree(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		return GpuModifiedLockFree(manager, offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, false>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::TakeGpuPagesForCpu(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	// Iterate<true>: the CPU claim creates a region that does not exist yet, which is what
	// MarkRegionAsCpuModified did on its own.
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		const auto     address = manager->GetCpuAddr() + offset;
		manager->ChangeState<DirtySource::Gpu, false>(address, bytes);
		manager->ChangeState<DirtySource::Cpu, true>(address, bytes);
	});
}

void MemoryTracker::UntrackMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::vector<RegionManager*> managers;
	managers.reserve((vaddr % TRACKER_REGION_SIZE + size + TRACKER_REGION_SIZE - 1) /
	                 TRACKER_REGION_SIZE);
	Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
		managers.push_back(manager);
	});

	std::vector<std::unique_lock<TrackingSpinLock>> locks;
	locks.reserve(managers.size());
	for (auto* manager: managers) {
		locks.emplace_back(manager->lock);
	}
	if (Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	    })) {
		EXIT("cannot untrack GPU-dirty memory\n");
	}
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

} // namespace Libs::Graphics
