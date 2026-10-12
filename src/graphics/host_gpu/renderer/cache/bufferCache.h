#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/gpuPhaseStats.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memResidency.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/knownFills.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <atomic>
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
	// Logs what the shader loop guards reported through the host tail of the GDS buffer.
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
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();
	// --mem-residency. GPU thread: the guest ranges of the registered buffers.
	void CollectResidencyRanges(MemResidency::Snapshot& snapshot);

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
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	bool                                              m_bda_pagetable_initialized = false;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
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
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
