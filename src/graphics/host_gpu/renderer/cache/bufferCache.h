#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/gpuPhaseStats.h"
#include "common/lruCache.h"
#include "common/verifyRation.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memResidency.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/knownCopies.h"
#include "graphics/host_gpu/renderer/cache/hotPages.h"
#include "graphics/host_gpu/renderer/cache/knownFills.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/readbackFuture.h"
#include "graphics/host_gpu/renderer/cache/smallCopyDedup.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// --cp-direct-stores: the emulator stored to [vaddr, vaddr + size) itself. True: the tracker took it as a CPU
	// modification without touching any page protection; false: a page is GPU-dirty, take InvalidateMemory first.
	[[nodiscard]] bool     NoteGuestStore(uint64_t vaddr, uint64_t size);
	// Logs what the shader loop guards reported into the loop guard buffer.
	void                   PollLoopGuard();
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                  Common::GpuPhase::DrainCause cause =
	                                      Common::GpuPhase::DrainCause::BufferReadback);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	// --resolved-draw: ObtainBuffer calls (read-only, non-texel) answered from the memo of ranges found
	// clean, those that found the range dirty, small constants copied to the stream buffer, and the
	// rest. Kept by the GPU thread; read for the exit summary.
	struct ObtainStats {
		uint64_t calls = 0, memo_hits = 0, small_copies = 0, written = 0, texel = 0, stored = 0;
		uint64_t small_copy_reuse = 0, small_copy_reuse_bytes = 0; // --small-copy-dedup: answered by an earlier copy
	};
	[[nodiscard]] const ObtainStats& GetObtainStats() const noexcept { return m_obtain_stats; }
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	// --gpu-indirect-draws: device-addressable ring for per-draw parameter records that mesh shaders
	// load by address and that GPU passes write. Created on first use (4 MiB).
	[[nodiscard]] StreamBuffer& GetDrawRecordBuffer();
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] const Buffer* GetLoopGuardBuffer() const noexcept { return &m_loop_guard_buffer; }
	// Both the first registration and the first consumer clear the table, so unowned entries read as zero.
	[[nodiscard]] Buffer* GetBdaPageTableBuffer();
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// DMA between two GDS ranges (offsets into the GDS buffer); they may overlap.
	void CopyGdsBuffer(uint64_t dst_offset, uint64_t src_offset, uint64_t size);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	// Bumped whenever a buffer is registered or unregistered (K15).
	[[nodiscard]] uint64_t BufferSetGeneration() const noexcept {
		return m_buffer_set_generation.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// --known-fills: true when [vaddr, vaddr + size) was last written, on the GPU, by one fill with
	// *value. Lets a caller use a GPU-resident fill result without draining the GPU to read it.
	[[nodiscard]] bool TryGetKnownFill(uint64_t vaddr, uint64_t size, uint32_t* value);
	// Records that the GPU work recorded last filled [vaddr, vaddr + size) with `value`.
	void RecordKnownFill(uint64_t vaddr, uint64_t size, uint32_t value);
	// --known-stores: dwords a dispatch just recorded is proven to leave with a constant value
	// (knownStores.h). Any later GPU or CPU write of a dword, and any recorded work with address
	// writes, makes it unknown again. GPU thread records; any thread forgets.
	void RecordKnownStores(std::span<const std::pair<uint64_t, uint32_t>> dwords);
	// True, and `data` filled, when every dword of [vaddr, vaddr + size) is known (dword aligned
	// range of at most 1 KiB) and the flag is on. The caller still decides whether it needs the
	// range to be GPU-dirty.
	[[nodiscard]] bool TryReadKnownStores(uint64_t vaddr, uint64_t size, uint32_t* data);
	// Replaces, in `data` (a copy of [vaddr, vaddr + bytes) from the guest backing), every dword
	// that is GPU-dirty and known by its known value. Unknown dirty dwords keep the backing bytes.
	void OverlayKnownDirtyDwords(uint64_t vaddr, uint64_t bytes, uint32_t* data);
	// HasGpuDirtyBytes, except that a dirty range whose every dword is known (--known-stores) is
	// not counted: its value is available without waiting for the GPU. With --debug-known-verify a
	// known range is, once per interval, downloaded and compared instead (see UseKnownStores).
	[[nodiscard]] bool HasUnknownGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	// The value of a GPU-dirty range of at most 1 KiB (dword aligned) from the known-store table:
	// true and `data` filled when every dword is known. Once per --debug-known-verify interval (or on
	// every call with --debug-known-verify-all) the GPU is drained instead, the real bytes are
	// compared with the known ones (a mismatch is logged loudly and the knowledge dropped) and `data`
	// holds the real bytes. GPU thread only.
	[[nodiscard]] bool UseKnownStores(uint64_t vaddr, uint64_t size, uint32_t* data, bool count_hit);
	void               ForgetKnownStores(uint64_t vaddr, uint64_t size);
	// --known-copies: the GPU work recorded last copied `bytes` of dwords from `src` to `dst`
	// (knownCopies.h). When the source is readable without waiting for the GPU, the destination's
	// content is recorded as runs of equal dwords (at most 64; a source with more is not tracked).
	void RecordKnownCopy(const KnownCopy& copy);
	// True when [vaddr, vaddr + size) was last written by a proven copy whose source was one run of
	// equal dwords; *value is that dword. False when the flag is off or the range is not known.
	[[nodiscard]] bool TryGetKnownCopy(uint64_t vaddr, uint64_t size, uint32_t* value);
	void               ForgetKnownCopies(uint64_t vaddr, uint64_t size);
	// The current value of the dword at `vaddr` when the GPU thread has it without waiting for the
	// GPU: guest memory no GPU work has written, or a known store. GPU thread only.
	[[nodiscard]] bool TryReadCurrentDword(uint64_t vaddr, uint32_t* value);
	// Appends the known stores inside [vaddr, vaddr + size), at most `limit` in all.
	void CollectKnownStores(uint64_t vaddr, uint64_t size, std::vector<std::pair<uint64_t, uint32_t>>& out,
	                        size_t limit);
	// Drops the GPU-modified state of [vaddr, vaddr + size) without downloading it, for a range the
	// caller is about to overwrite completely from the CPU. Refuses (false, nothing changed) when
	// GPU-modified bytes outside the range share its tracking pages. GPU thread only.
	[[nodiscard]] bool TryDiscardGpuModified(uint64_t vaddr, uint64_t size);
	// Copies [vaddr, vaddr + size) from the guest backing when guest memory is authoritative for
	// exactly those bytes: none is GPU-modified, and no GPU-modified image overlaps. Otherwise false
	// and nothing is read. Never faults and never waits for the GPU. GPU thread only.
	[[nodiscard]] bool TryReadCleanBytes(uint64_t vaddr, void* data, uint64_t size);
	// A shader that may store through raw addresses into any buffer was recorded in the current
	// command buffer (so a second-queue readback must wait for it).
	void NoteAddressWrites() noexcept;
	// --eager-readback: before a submit, copies the pending GPU writes of the hot pages (pages a
	// game thread read-faulted on) out in the open command buffer; the bytes are written to guest
	// memory when it completes, after which the page is unprotected. GPU thread only.
	void               WriteBackHotPages();
	// --readback-futures (notes/frame-pipelining.md section 3). The faulting game thread runs
	// ReadMemoryFuture (it returns false when the old synchronous path must serve the fault); the
	// other two are the steps that run on the GPU thread and never wait for the GPU: StartReadFuture
	// copies the GPU-written bytes of the range out, queues `release` behind their write-back and
	// submits; FinishReadFuture, once `release` fired, unprotects the pages (true) unless a newer GPU
	// write kept one protected (false).
	// `is_write`: the thread writes (a CPU write over GPU-written pages): the pages are claimed for
	// the CPU once the bytes landed, as the synchronous path does.
	[[nodiscard]] bool        ReadMemoryFuture(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] FutureStart StartReadFuture(uint64_t vaddr, uint64_t size,
	                                          const std::shared_ptr<FutureRelease>& release,
	                                          bool is_write = false);
	// `last_round`: the thread has no round left, so pages that stay protected are marked contended.
	// `own_tick`: the release's tick (FutureRelease::tick), 0 when the caller has none.
	[[nodiscard]] bool        FinishReadFuture(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                           bool last_round = false, uint64_t own_tick = 0);
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	// Verification of --bda-dirty-log: while set, every range UploadDirtyRanges uploads is appended here
	// (GPU thread only).
	void               SetUploadCapture(std::vector<std::pair<uint64_t, uint64_t>>* capture) noexcept {
		m_upload_capture = capture;
	}
	void               RunGarbageCollector();
	// --mem-residency. GPU thread: the guest ranges of the registered buffers.
	void CollectResidencyRanges(MemResidency::Snapshot& snapshot);
	// One [upload-ring] line every 10 s under --mem-stats or --upload-ring-rebar.
	void LogRingStatsPeriodic();

private:
	friend struct BufferCacheTestAccess;

	// Device memory is not zero-initialized, and a zero entry is what shaders read as unmapped.
	void EnsureBdaPageTableInitialized();

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	void UploadDirtyRanges(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// --eager-readback. Only the GPU thread touches these.
	[[nodiscard]] Buffer* FindExistingBuffer(uint64_t vaddr);
	[[nodiscard]] bool    EagerDownload(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Finishes the eager copies of hot pages in [vaddr, vaddr + size): those that completed are
	// unprotected; with `wait` the others are waited for first.
	void                  RetireHotPages(uint64_t vaddr, uint64_t size, bool wait);
	[[nodiscard]] bool    VerifyWrittenBack(uint64_t page);
	// Schedules the eager copy of one hot page that has GPU-written bytes and no copy in flight.
	[[nodiscard]] bool    ScheduleEagerPage(uint64_t page, HotPageSet::Entry& entry);
	// True while an eager copy has taken bytes of the range out of m_gpu_modified_ranges and has not
	// written them to guest memory yet (the guest copy of those bytes is stale).
	[[nodiscard]] bool    HasEagerPending(uint64_t vaddr, uint64_t size);
	void CollectDownloadCopies(uint64_t buffer_address, uint64_t vaddr, uint64_t size,
	                           std::vector<vk::BufferCopy>& copies, uint64_t& total_size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_loop_guard_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	bool                                              m_bda_pagetable_initialized = false;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	HotPageSet                                        m_hot_pages;
	// --readback-futures: pages whose rounds were exhausted, page -> value of m_future_starts then.
	std::map<uint64_t, uint64_t>                      m_future_contended;
	uint64_t                                          m_future_starts = 0;
	RangeSet                                          m_eager_pending_ranges;
	// --readback-queue: copy on the second queue after a timeline wait on the producer's tick.
	[[nodiscard]] bool ReadbackOnSecondQueue(const Buffer& buffer, uint64_t buffer_tick,
	                                         const std::vector<vk::BufferCopy>& copies,
	                                         uint64_t total_size, const Buffer& download,
	                                         uint64_t offset);
	uint64_t        m_address_write_tick = 0;
	vk::CommandPool m_readback_pool      = nullptr;
	vk::CommandBuffer m_readback_command = nullptr;
	vk::Semaphore   m_readback_semaphore = nullptr; // timeline, signaled by readback submits
	uint64_t        m_readback_value     = 0;
	std::mutex                                        m_known_fills_mutex;
	KnownFills                                        m_known_fills;
	void ForgetKnownFills(uint64_t vaddr, uint64_t size);
	std::mutex                                        m_known_stores_mutex;
	KnownFills                                        m_known_stores;
	std::atomic<size_t>                               m_known_stores_count {0};
	Common::VerifyRation                              m_known_stores_verify;
	std::mutex                                        m_known_copies_mutex;
	KnownFills                                        m_known_copies;
	std::atomic<size_t>                               m_known_copies_count {0};
	std::vector<uint32_t>                             m_known_copy_scratch; // GPU thread only
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	std::unique_ptr<StreamBuffer>                     m_draw_record_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	// Bytes of buffers registered in this cache (m_total_used_memory is replaced by the device total
	// when the driver reports it). Input of BufferGcGate.
	uint64_t                                          m_registered_bytes   = 0;
	std::atomic<uint64_t>                             m_buffer_set_generation {0};
	// --resolved-draw: ranges ObtainBuffer found clean (no CPU- or GPU-dirty page) with the tracker
	// epochs read before the check and the buffer-set generation; an equal epoch and generation later
	// mean the range is still clean and its buffer is still the one found (direct mapped, GPU thread).
	struct alignas(64) ObtainMemo {
		uint64_t vaddr = 0, size = 0;
		BufferId id;
		uint64_t buffer_generation = 0;
		uint32_t epoch_count       = 0;
		uint32_t epochs[2]         = {};
		bool     valid             = false;
		bool     seen              = false; // this key missed before: worth the extra check next time
	};
	// --incremental-draw indexes the whole table: a frame names thousands of distinct ranges and 512 slots evict most of them before
	// the next frame asks again. The entries check their own key, so any slot count is exact; the stock size stays when the flag is off.
	static constexpr size_t kObtainMemoSlotsStock = 512;
	static constexpr size_t kObtainMemoSlots      = 16384;
	std::array<ObtainMemo, kObtainMemoSlots> m_obtain_memo {};
	ObtainStats                              m_obtain_stats {};
	// --small-copy-dedup: the bytes and ring offsets of this tick's small constant copies.
	SmallCopyDedup                           m_small_copy_dedup;
	std::vector<std::pair<uint64_t, uint64_t>>* m_upload_capture = nullptr;
	// Cumulative garbage-collection counters (GPU thread only), logged under memory pressure.
	struct GcStats {
		uint64_t runs       = 0; // collector runs past the trigger
		uint64_t aggressive = 0; // of those, aggressive ones
		uint64_t freed      = 0; // clean buffers deleted
		uint64_t downloads  = 0; // dirty buffers downloaded
		uint64_t drains     = 0; // download batches, each a GPU drain
	};
	GcStats                                           m_gc_stats;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	uint64_t m_gc_runs            = 0;
	std::chrono::steady_clock::time_point m_ring_log_next {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
