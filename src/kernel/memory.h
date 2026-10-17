#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/virtualMemory.h"

#include <utility>
#include <vector>

namespace Libs::Graphics {
class RenderContext;
enum class PageFaultAccess;
} // namespace Libs::Graphics

namespace Libs::LibKernel::Memory {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Memory";
	static constexpr auto        initialize = Libs::LibKernel::Memory::Initialize;
	static constexpr auto        shutdown   = Libs::LibKernel::Memory::Shutdown;
};

using callback_func_t = void (*)(uintptr_t addr, size_t size);

constexpr uint32_t KERNEL_MAXIMUM_NAME_LENGTH = 32;
constexpr uint64_t kExtendedMemoryBase       = 0x080000000000ull;
constexpr uint64_t kExtendedMemorySize       = 512ull * 1024 * 1024 * 1024;

struct VirtualQueryInfo {
	uintptr_t start;
	uintptr_t end;
	uint64_t  offset;
	int32_t   protection;
	int32_t   memory_type;
	uint32_t  is_flexible  : 1;
	uint32_t  is_direct    : 1;
	uint32_t  is_stack     : 1;
	uint32_t  is_pooled    : 1;
	uint32_t  is_committed : 1;
	uint32_t  is_gpu_prt   : 1;
	uint32_t  amm_usage    : 1;
	uint32_t  reserved     : 1;
	char      name[KERNEL_MAXIMUM_NAME_LENGTH];
	uint8_t   gpu_mask_id;
	uint8_t   reserved2;
};

static_assert(sizeof(VirtualQueryInfo) == 72, "VirtualQueryInfo struct size is incorrect");

struct KernelBatchMapEntry {
	void*         start;
	uint64_t      offset;
	uint64_t      length;
	unsigned char protection;
	unsigned char type;
	int16_t       reserved;
	int32_t       operation;
};

static_assert(sizeof(KernelBatchMapEntry) == 32, "KernelBatchMapEntry struct size is incorrect");

struct KernelMemoryPoolBatchEntry {
	uint32_t op;
	uint32_t flags;
	union {
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} commit;
		struct {
			void*    addr;
			uint64_t len;
		} decommit;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
		} protect;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} type_protect;
		struct {
			void*    dst;
			void*    src;
			uint64_t len;
		} move;
		uintptr_t padding[3];
	};
};

static_assert(sizeof(KernelMemoryPoolBatchEntry) == 32,
              "KernelMemoryPoolBatchEntry struct size is incorrect");

struct KernelMemoryPoolBlockStats {
	int32_t available_flushed_blocks;
	int32_t available_cached_blocks;
	int32_t allocated_flushed_blocks;
	int32_t allocated_cached_blocks;
};

static_assert(sizeof(KernelMemoryPoolBlockStats) == 16,
              "KernelMemoryPoolBlockStats struct size is incorrect");

void                   RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);
void                   SetFlexibleMemorySize(uint64_t size);
int AllocateDirectMemory(int64_t search_start, int64_t search_end, size_t size, size_t alignment,
                         int memory_type, int64_t* phys_addr_out, bool automatic = false);
int MapAutomaticMemory(uint64_t vaddr, size_t size, int type, int prot);
bool                   TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size);
bool                   TryReadBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size);
// GPU thread: the range holds bytes the GPU wrote that the guest backing does not have yet (a
// TryReadBufferBacking of it would read them back). Never synchronizes.
bool                   HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
// --known-stores: GPU-dirty bytes whose value the GPU thread does not already know (a dirty range
// every dword of which a recorded dispatch is proven to have stored as a constant is not counted).
bool                   HasUnknownGpuDirtyBytes(uint64_t vaddr, uint64_t size);
// Replaces the GPU-dirty, known dwords of `data` (a copy of [vaddr, vaddr + bytes) from the guest
// backing) by their known values.
void                   OverlayKnownGpuBytes(uint64_t vaddr, uint32_t* data, uint64_t bytes);
// GPU thread: copies guest bytes from the backing, without a page fault and without a GPU
// readback, when none of them is GPU-written (--clean-guest-reads). False reads nothing.
bool                   TryReadCleanBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size);
// True when the whole range is file-backed guest memory that was never written, so it reads as
// zeros. Reads nothing and makes no page resident; false when in doubt.
bool                   GuestRangeNeverWritten(uint64_t vaddr, uint64_t size);
// Debug: reads the range with pread (no page becomes resident) and says whether it is all zeros.
bool                   GuestRangeReadsAsZero(uint64_t vaddr, uint64_t size);
[[nodiscard]] uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size);
// Remembered ClampRangeSize answers (--mapping-memo) of the calling thread.
struct MappingMemoStats {
	uint64_t hits   = 0;
	uint64_t misses = 0;
};
[[nodiscard]] MappingMemoStats GetMappingMemoStats();
// Whether guest mappings cover all of [vaddr, vaddr + size), and the parts they cover (as
// (address, size), in order): what can be read or written of a range that reaches into memory the
// guest unmapped.
[[nodiscard]] bool     IsFullyMapped(uint64_t vaddr, uint64_t size);
void                   MappedParts(uint64_t vaddr, uint64_t size,
                                   std::vector<std::pair<uint64_t, uint64_t>>* parts);
// Writes GPU data into guest memory: backed parts through the backing, private memory (stacks,
// TLS) at its own address; parts the guest unmapped meanwhile are dropped and logged.
void                   WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept;
void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
// For a fault in a guest range briefly unmapped while the emulator replaced its mapping (MAP_FIXED
// over a mapping, or a Windows file view split by a partial unmap): waits until it is mapped again
// and returns the range's id. 0 for any other fault.
[[nodiscard]] uint64_t WaitForTransientUnmap(uint64_t fault_vaddr) noexcept;
// Whether such a fault can be retried (false once the same page keeps faulting).
[[nodiscard]] bool RetryTransientUnmapFault(uint64_t fault_vaddr, uint64_t window) noexcept;
void                   InstallGpuResources(Graphics::RenderContext* renderer) noexcept;
[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept;

int KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags,
                                               const char* name);
int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len);
int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len);
size_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
int KYTY_SYSV_ABI    KernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                                     size_t alignment, int64_t* phys_addr_out,
                                                     size_t* size_out);
int KYTY_SYSV_ABI    KernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total,
                                             int* gpu_available);
int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type,
                                                 int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len);

// Counters of --mem-stats (all zero unless it is on). For tests and diagnostics.
struct MemStatsSnapshot {
	struct Pair {
		uint64_t calls = 0;
		uint64_t bytes = 0;
	};
	Pair     direct_release;
	Pair     munmap_direct;
	Pair     munmap_flexible;
	Pair     munmap_pooled;
	Pair     pool_decommit;
	Pair     flexible_map;
	Pair     flexible_unmap;
	Pair     punch;
	uint64_t released_unmapped_bytes       = 0;
	uint64_t realloc_bytes                 = 0;
	uint64_t realloc_after_punch_bytes     = 0;
	uint64_t punch_failures                = 0;
};
MemStatsSnapshot KernelMemStatsSnapshot();

// --mem-residency (Linux). Reads of the guest backing of at least 16 KiB are classified by whether
// their pages are resident (mincore) and, for the ones that are not, whether the page was ever
// written (a hole in the backing file) or is swapped out.
enum class BackingReadTag : uint8_t { Other, Image };
class ScopedBackingReadTag {
public:
	explicit ScopedBackingReadTag(BackingReadTag tag) noexcept;
	~ScopedBackingReadTag();
	KYTY_CLASS_NO_COPY(ScopedBackingReadTag);

private:
	BackingReadTag m_previous;
};

struct BackingReadStats {
	struct Counts {
		uint64_t reads            = 0; // reads of at least 16 KiB
		uint64_t bytes            = 0;
		uint64_t nonresident      = 0; // bytes on pages that were not resident when read
		uint64_t hole             = 0; // ... of which never written (no data in the backing file)
		uint64_t pread_bytes      = 0; // bytes read with pread instead of a mapping (--mem-hole-reads)
	};
	Counts   image; // reads for GPU image uploads
	Counts   other;
	uint64_t pread_failures = 0;
};
BackingReadStats KernelBackingReadStats();
// --mem-residency: who writes guest memory from the host side. Kyty writes the guest copy of GPU
// results (buffer downloads), of expanded color metadata and of timestamps; `newly_resident` is how
// many bytes of those writes landed on pages that were not resident before, i.e. memory the
// emulator made resident (an upper bound of what the game would not have touched itself).
enum class BackingWriteTag : uint8_t { Other, Download, Metadata, Count };
class ScopedBackingWriteTag {
public:
	explicit ScopedBackingWriteTag(BackingWriteTag tag) noexcept;
	~ScopedBackingWriteTag();
	KYTY_CLASS_NO_COPY(ScopedBackingWriteTag);

private:
	BackingWriteTag m_previous;
};
struct BackingWriteStats {
	struct Counts {
		uint64_t writes         = 0;
		uint64_t bytes          = 0;
		uint64_t newly_resident = 0;
	};
	Counts by_tag[static_cast<size_t>(BackingWriteTag::Count)];
};
BackingWriteStats KernelBackingWriteStats();

// A committed range of the guest address space with the name and kind the guest gave it
// (--mem-residency attributes resident guest memory to these).
struct GuestRangeInfo {
	uint64_t    start       = 0;
	uint64_t    size        = 0;
	int         protection  = 0;
	int         memory_type = 0;
	const char* kind        = "";
	char        name[KERNEL_MAXIMUM_NAME_LENGTH] = {};
};
std::vector<GuestRangeInfo> KernelGuestRangesSnapshot();

// Bytes of [vaddr, vaddr + size) whose backing pages are resident in RAM now (mincore on the
// backing file; swapped-out pages do not count). Gaps in the guest mapping count as zero.
uint64_t         KernelGuestResidentBytes(uint64_t vaddr, uint64_t size);
// Resident bytes of the whole guest backing file.
uint64_t         KernelGuestResidentTotalBytes();

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags,
                                             int64_t direct_memory_start, size_t alignment,
                                             const char* name);
int KYTY_SYSV_ABI KernelSetPrtAperture(int index, void* addr, size_t len);
int KYTY_SYSV_ABI KernelGetPrtAperture(int index, void** addr, size_t* len);
int KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled();
int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info,
                                     uint64_t info_size);
int KYTY_SYSV_ABI KernelIsStack(void* addr, void** start, void** end);
int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot);
int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot);
int KYTY_SYSV_ABI KernelBatchMap(KernelBatchMapEntry* entries, int num_entries,
                                 int* num_entries_out);
int KYTY_SYSV_ABI KernelBatchMap2(KernelBatchMapEntry* entries, int num_entries,
                                  int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolReserve(void* addr_in, size_t len, size_t alignment, int flags,
                                          void** addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int type, int prot, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolBatch(const KernelMemoryPoolBatchEntry* entries, int num_entries,
                                        int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(KernelMemoryPoolBlockStats* output,
                                                size_t                      output_size);

uint64_t AllocateProgramMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name);
void SetProgramMemoryProtection(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
uint64_t AllocateRuntimeMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name,
                               bool fixed = false);
uint64_t AllocateGuestStackMemory(uint64_t search_addr, uint64_t size,
                                  Common::VirtualMemory::Mode mode, const char* name);
bool     ProtectGuestMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode,
                            Common::VirtualMemory::Mode* old_mode = nullptr);
// Transient PageManager watch state; does not change the guest mapping's semantic protection.
bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
bool FreeGuestMemory(uint64_t vaddr, uint64_t size);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void     TestBeforeNextBackingMap(callback_func_t callback);
void     TestSetBackingReadCallback(callback_func_t callback);
void     TestFailNextPhysicalMemoryUnmap();
void     TestFailPhysicalMemoryUnmapAfter(uint32_t successful_unmaps);
void     TestFailGuestBackingStoreUnmapAfter(uint32_t successful_unmaps);
void     TestFailNextFixedReserveRangeRegistration();
void     TestFailNextVirtualRangeReplacement();
bool     TestPlaceholderRangeIsFree(uint64_t vaddr, uint64_t size);
bool     TestGuestAddressRangeIsOwned(uint64_t vaddr, uint64_t size);
bool     TestGuestBackingOutsideAddressSpace();
uint64_t TestGuestBackingSize();
bool     TestGuestFreeRangeBounds();
// Reads a 4 MiB backing store whose 1st and 3rd MiB were written and whose other MiB are holes,
// with --mem-hole-reads switched on or off for the call.
struct TestBackingScan {
	bool                      ran             = false; // false where the scan is not supported
	bool                      data_ok         = false; // the read returned the bytes (zeros on holes)
	uint64_t                  resident_before = 0;
	uint64_t                  resident_after  = 0;
	BackingReadStats::Counts  delta;                    // 'other' counters changed by the read
	uint64_t                  pread_failures  = 0;
	bool                      never_written_ok = false; // RangeIsNeverWritten/RangeReadsAsZero verdicts before the read
	bool                      hole_after_read  = false; // RangeIsNeverWritten of the 2nd MiB after the read
};
void     TestBackingReadScan(TestBackingScan* out, bool hole_reads);
#endif

} // namespace Libs::LibKernel::Memory

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
