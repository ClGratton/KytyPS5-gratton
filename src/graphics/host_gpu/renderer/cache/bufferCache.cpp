#include "common/emulatorConfig.h"
#include "common/gpuPhaseStats.h"
#include "common/loopGuard.h"
#include "graphics/host_gpu/bdaSyncGate.h"
#include "graphics/host_gpu/renderer/cache/gcClock.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/skipDepsHooks.h"
#include "graphics/host_gpu/renderer/cache/gdsCopyPlan.h"
#include "graphics/host_gpu/renderer/cache/bufferGcGate.h"
#include "graphics/host_gpu/renderer/cache/readbackPlan.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vramPolicy.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <thread>
#include <array>
#include <cstdio>
#include <chrono>
#include <fmt/format.h>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
// The guest's GDS is 64 KiB, and the buffer is exactly that, so a guest access past it is out of
// bounds for the shader too. The shader loop guard reports (common/loopGuard.h) go to a buffer of
// their own (LoopGuardBufferSize): sharing GDS made every guarded shader a GDS user.
constexpr uint64_t GdsBufferSize       = Common::LoopGuard::GuestGdsBytes;
constexpr uint64_t LoopGuardBufferSize = Common::LoopGuard::BufferBytes;

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

void BufferCache::EnsureBdaPageTableInitialized() {
	if (m_bda_pagetable_initialized) {
		return;
	}
	// Recorded before the first table write or read, and ordered by Fill barriers.
	m_bda_pagetable_buffer.Fill(0, m_bda_pagetable_buffer.Size(), 0);
	m_bda_pagetable_initialized = true;
}

Buffer* BufferCache::GetBdaPageTableBuffer() {
	EnsureBdaPageTableInitialized();
	return &m_bda_pagetable_buffer;
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	m_buffer_set_generation.fetch_add(1, std::memory_order_release); // K15
	EnsureBdaPageTableInitialized();
	auto& buffer = m_slot_buffers[id];
	if constexpr (insert) {
		// --bda-dirty-log: a new buffer may cover pages that were dirtied before it existed; the next
		// BDA pass has to synchronize it whatever the log says about those pages.
		if (BdaSync::g_dirty_log_active.load(std::memory_order_seq_cst)) {
			BdaSync::g_dirty_log.Note(buffer.CpuAddress(), buffer.Size());
		}
	}
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		m_registered_bytes += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_registered_bytes -= std::min<uint64_t>(m_registered_bytes, buffer.Size());
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

// Takes the GPU-modified ranges of [vaddr, vaddr + size) out of the cache's bookkeeping and lists
// the copies that read them back; dstOffset is the offset in a staging area of `total_size` bytes.
void BufferCache::CollectDownloadCopies(uint64_t buffer_address, uint64_t vaddr, uint64_t size,
                                        std::vector<vk::BufferCopy>& copies,
                                        uint64_t&                    total_size) {
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	CollectDownloadCopies(buffer_address, vaddr, size, copies, total_size);
	if (copies.empty()) {
		return false;
	}

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	if constexpr (!async) {
		if (m_graphics.readback_queue != nullptr && Config::ReadbackQueueEnabled()) {
			[[maybe_unused]] const auto first_address = buffer_address + copies.front().srcOffset;
			Common::GpuPhase::NoteReadback(first_address, total_size);
			if (ReadbackOnSecondQueue(buffer, buffer.last_gpu_write_tick, copies, total_size,
			                          download, offset)) {
				(temporary ? *temporary : m_download_buffer).Invalidate(offset, total_size);
				const Libs::LibKernel::Memory::ScopedBackingWriteTag write_tag(
				    Libs::LibKernel::Memory::BackingWriteTag::Download);
				for (const auto& copy: copies) {
					Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
					                                      mapped + (copy.dstOffset - offset),
					                                      copy.size);
				}
				return true;
			}
		}
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	[[maybe_unused]] const auto first_copy_address = buffer_address + copies.front().srcOffset;
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		const Libs::LibKernel::Memory::ScopedBackingWriteTag write_tag(
		    Libs::LibKernel::Memory::BackingWriteTag::Download);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		Common::GpuPhase::NoteReadback(first_copy_address, total_size);
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

void BufferCache::NoteAddressWrites() noexcept {
	m_address_write_tick = m_scheduler.CurrentTick();
	// A shader that writes through raw addresses can overwrite any dword the tracking above does not
	// see: everything recorded by --known-stores is unknown from here on.
	if (m_known_stores_count.load(std::memory_order_relaxed) != 0) {
		std::lock_guard lock(m_known_stores_mutex);
		m_known_stores.Clear();
		m_known_stores_count.store(0, std::memory_order_relaxed);
	}
	if (m_known_copies_count.load(std::memory_order_relaxed) != 0) {
		std::lock_guard lock(m_known_copies_mutex);
		m_known_copies.Clear();
		m_known_copies_count.store(0, std::memory_order_relaxed);
	}
}

bool BufferCache::ReadbackOnSecondQueue(const Buffer& buffer, uint64_t buffer_tick,
                                        const std::vector<vk::BufferCopy>& copies,
                                        uint64_t total_size, const Buffer& download,
                                        uint64_t offset) {
	const auto device = m_graphics.device;
	if (m_readback_pool == nullptr) {
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		pool_info.queueFamilyIndex = m_graphics.queue_family;
		if (device.createCommandPool(&pool_info, nullptr, &m_readback_pool) != vk::Result::eSuccess) {
			return false;
		}
		vk::CommandBufferAllocateInfo alloc_info {};
		alloc_info.commandPool        = m_readback_pool;
		alloc_info.level              = vk::CommandBufferLevel::ePrimary;
		alloc_info.commandBufferCount = 1;
		if (device.allocateCommandBuffers(&alloc_info, &m_readback_command) !=
		    vk::Result::eSuccess) {
			return false;
		}
		vk::SemaphoreTypeCreateInfo type_info {};
		type_info.semaphoreType = vk::SemaphoreType::eTimeline;
		vk::SemaphoreCreateInfo semaphore_info {};
		semaphore_info.pNext = &type_info;
		if (device.createSemaphore(&semaphore_info, nullptr, &m_readback_semaphore) !=
		    vk::Result::eSuccess) {
			return false;
		}
	}

	// The copy waits for the tick of the work that wrote the data, not for everything queued
	// since. If that work is still in the open command buffer, submit it first (no wait).
	Common::GpuPhase::NoteDrainFull(false);
	Common::GpuPhase::Scope gpu_phase_drain(Common::GpuPhase::Phase::DrainTick);
	const auto plan = PlanReadback(buffer_tick, m_address_write_tick, m_scheduler.CurrentTick());
	Common::GpuPhase::NoteQueueReadback(plan.flush_first,
	                                    m_scheduler.CurrentTick() - plan.wait_tick);
	if (plan.flush_first) {
		m_scheduler.Flush();
	}

	const auto command = m_readback_command;
	command.reset({});
	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	EXIT_IF(command.begin(&begin_info) != vk::Result::eSuccess);
	command.copyBuffer(buffer.Handle(), download.Handle(), static_cast<uint32_t>(copies.size()),
	                   copies.data());
	vk::BufferMemoryBarrier after {};
	after.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.buffer              = download.Handle();
	after.offset              = offset;
	after.size                = total_size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, 1, &after, 0, nullptr);
	EXIT_IF(command.end() != vk::Result::eSuccess);

	const uint64_t               signal_value   = ++m_readback_value;
	const vk::Semaphore          wait_semaphore = m_scheduler.GetMasterSemaphore().Handle();
	const uint64_t               wait_value     = plan.wait_tick;
	const vk::PipelineStageFlags wait_stage     = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.signalSemaphoreValueCount = 1;
	timeline_info.pSignalSemaphoreValues    = &signal_value;
	vk::SubmitInfo submit_info {};
	submit_info.pNext                = &timeline_info;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &command;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &m_readback_semaphore;
	if (wait_value != 0) {
		timeline_info.waitSemaphoreValueCount = 1;
		timeline_info.pWaitSemaphoreValues    = &wait_value;
		submit_info.waitSemaphoreCount        = 1;
		submit_info.pWaitSemaphores           = &wait_semaphore;
		submit_info.pWaitDstStageMask         = &wait_stage;
	}
	{
		Common::LockGuard lock(m_graphics.readback_queue_mutex);
		const auto        result = m_graphics.readback_queue.submit(1, &submit_info, nullptr);
		if (result != vk::Result::eSuccess) {
			EXIT("BufferCache: readback submit failed: %s\n", vk::to_string(result).c_str());
		}
	}
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_readback_semaphore;
	wait_info.pValues        = &signal_value;
	const auto wait_result   = device.waitSemaphores(&wait_info, UINT64_MAX);
	if (wait_result != vk::Result::eSuccess) {
		EXIT("BufferCache: readback wait failed: %s\n", vk::to_string(wait_result).c_str());
	}
	// Guest-memory completions queued for earlier ticks must land before this one does.
	m_scheduler.WaitPriorityOperations(plan.wait_tick);
	return true;
}

StreamBuffer& BufferCache::GetDrawRecordBuffer() {
	if (!m_draw_record_buffer) {
		m_draw_record_buffer = std::make_unique<StreamBuffer>(
		    m_graphics, m_scheduler, MemoryUsage::Stream, 4 * MiB,
		    vk::BufferUsageFlagBits::eShaderDeviceAddress);
		SetVulkanObjectNameF(m_graphics.device, m_draw_record_buffer->Handle(),
		                     "Draw Record Ring");
	}
	return *m_draw_record_buffer;
}

namespace {

// Sizes the placement policy needs, from the device's memory heaps.
VramPolicy::UploadRingPlan PlanStagingRing(const GraphicContext& graphics) {
	const auto& properties = graphics.GetPhysicalDeviceMemoryProperties();
	uint64_t    device_local_total = 0;
	uint64_t    rebar_heap         = 0;
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		if (properties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
			device_local_total += properties.memoryHeaps[i].size;
		}
	}
	const auto wanted = vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			rebar_heap = std::max<uint64_t>(rebar_heap,
			                                properties.memoryHeaps[properties.memoryTypes[i].heapIndex].size);
		}
	}
	return VramPolicy::PlanUploadRing(Config::UploadRingRebarEnabled(), device_local_total, rebar_heap);
}

} // namespace

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_loop_guard_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, LoopGuardBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      // --upload-ring-rebar: the Stream usage asks for device-local host-visible memory (sequential
      // CPU writes, write-combined); the ring is only ever written by the CPU. VramPolicy decides
      // from the device's heaps whether the card can spare it.
      m_staging_buffer(graphics, scheduler,
                       PlanStagingRing(graphics).device_local ? MemoryUsage::Stream : MemoryUsage::Upload,
                       PlanStagingRing(graphics).size),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	{
		const auto plan = PlanStagingRing(graphics);
		Log::WriteToConsoleAndLog(fmt::format(
		    "Upload ring: {} MiB in {} memory{}\n", m_staging_buffer.Size() >> 20,
		    m_staging_buffer.IsDeviceLocal() ? "device-local host-visible (ReBAR)" : "system",
		    m_staging_buffer.IsDeviceLocal() ? "" :
		    !Config::UploadRingRebarEnabled() ? " (--upload-ring-rebar off)" :
		    !plan.device_local ? " (no large host-visible device-local heap, or a card too small to spare it)" :
		                         " (the device-local allocation failed)"));
	}
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	std::memset(m_loop_guard_buffer.Mapped().data(), 0, static_cast<size_t>(m_loop_guard_buffer.Size()));
	m_loop_guard_buffer.Flush(0, m_loop_guard_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_loop_guard_buffer.Handle(), "Loop Guard Buffer");
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
	if (Config::GcBufferTriggerMb() != 0) {
		m_trigger_gc_memory = Config::GcBufferTriggerMb() * 1024ull * 1024;
	}
	if (Config::GcBufferCriticalMb() != 0) {
		m_critical_gc_memory = Config::GcBufferCriticalMb() * 1024ull * 1024;
	}
}

BufferCache::~BufferCache() {
	if (m_readback_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_readback_semaphore, nullptr);
	}
	if (m_readback_pool != nullptr) {
		m_graphics.device.destroyCommandPool(m_readback_pool, nullptr);
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::ForgetKnownFills(uint64_t vaddr, uint64_t size) {
	// Whatever the flag says: knowledge recorded while --known-stores was on must not survive a
	// write that happens while it is off (it is a runtime toggle).
	ForgetKnownStores(vaddr, size);
	ForgetKnownCopies(vaddr, size);
	if (!Config::KnownFillsEnabled()) {
		return;
	}
	std::lock_guard lock(m_known_fills_mutex);
	m_known_fills.Forget(vaddr, size);
}

void BufferCache::ForgetKnownCopies(uint64_t vaddr, uint64_t size) {
	if (m_known_copies_count.load(std::memory_order_relaxed) == 0) {
		return;
	}
	std::lock_guard lock(m_known_copies_mutex);
	m_known_copies.Forget(vaddr, size);
	m_known_copies_count.store(m_known_copies.Count(), std::memory_order_relaxed);
}

void BufferCache::RecordKnownCopy(const KnownCopy& copy) {
	// A source this big is a bulk copy, not a table of keys: not worth scanning.
	constexpr uint64_t kMaxCopyBytes = 4u << 20;
	if (!Config::KnownCopiesEnabled() || copy.bytes == 0 || copy.bytes > kMaxCopyBytes || (copy.bytes & 3u) != 0 ||
	    HasGpuDirtyBytes(copy.src, copy.bytes) || m_texture_cache.HasGpuModifiedImage(copy.src, copy.bytes)) {
		return;
	}
	m_known_copy_scratch.resize(copy.bytes / 4);
	if (!Libs::LibKernel::Memory::TryReadBacking(copy.src, m_known_copy_scratch.data(), copy.bytes)) {
		return;
	}
	constexpr size_t kMaxRuns = 64;
	std::vector<DwordRun> runs;
	if (!CollectRuns(m_known_copy_scratch, kMaxRuns, runs)) {
		return;
	}
	std::lock_guard lock(m_known_copies_mutex);
	constexpr size_t kMaxKnownCopies = 1024;
	if (m_known_copies.Count() + runs.size() > kMaxKnownCopies) {
		m_known_copies.Clear();
	}
	for (const auto& run: runs) {
		m_known_copies.Record(copy.dst + run.offset, run.bytes, run.value);
	}
	m_known_copies_count.store(m_known_copies.Count(), std::memory_order_relaxed);
	Common::GpuPhase::NoteKnownCopy(Common::GpuPhase::KnownCopy::Recorded, runs.size());
}

bool BufferCache::TryGetKnownCopy(uint64_t vaddr, uint64_t size, uint32_t* value) {
	if (m_known_copies_count.load(std::memory_order_relaxed) == 0 || !Config::KnownCopiesEnabled()) {
		return false;
	}
	std::lock_guard lock(m_known_copies_mutex);
	return m_known_copies.TryGet(vaddr, size, value);
}

void BufferCache::ForgetKnownStores(uint64_t vaddr, uint64_t size) {
	if (m_known_stores_count.load(std::memory_order_relaxed) == 0) {
		return;
	}
	std::lock_guard lock(m_known_stores_mutex);
	m_known_stores.Forget(vaddr, size);
	m_known_stores_count.store(m_known_stores.Count(), std::memory_order_relaxed);
}

void BufferCache::RecordKnownStores(std::span<const std::pair<uint64_t, uint32_t>> dwords) {
	if (dwords.empty()) {
		return;
	}
	// Bounded: a table that outgrows this is dropped whole (knowledge is only ever lost, never wrong).
	constexpr size_t kMaxKnownStores = 4096;
	std::lock_guard  lock(m_known_stores_mutex);
	if (m_known_stores.Count() + dwords.size() > kMaxKnownStores) {
		m_known_stores.Clear();
	}
	for (const auto& [address, value]: dwords) {
		m_known_stores.Record(address, 4, value);
	}
	m_known_stores_count.store(m_known_stores.Count(), std::memory_order_relaxed);
	Common::GpuPhase::NoteKnownStore(Common::GpuPhase::KnownStore::Recorded, dwords.size());
}

bool BufferCache::TryReadKnownStores(uint64_t vaddr, uint64_t size, uint32_t* data) {
	if (size == 0 || size > 1024 || m_known_stores_count.load(std::memory_order_relaxed) == 0 ||
	    !Config::KnownStoresEnabled()) {
		return false;
	}
	std::lock_guard lock(m_known_stores_mutex);
	return m_known_stores.TryReadDwords(vaddr, size, data);
}

void BufferCache::OverlayKnownDirtyDwords(uint64_t vaddr, uint64_t bytes, uint32_t* data) {
	if (bytes == 0 || (bytes & 3u) != 0 || (vaddr & 3u) != 0 ||
	    m_known_stores_count.load(std::memory_order_relaxed) == 0 || !Config::KnownStoresEnabled() ||
	    !HasGpuDirtyBytes(vaddr, bytes)) {
		return;
	}
	std::lock_guard lock(m_known_stores_mutex);
	for (uint64_t i = 0; i < bytes / 4; ++i) {
		const uint64_t address = vaddr + i * 4;
		uint32_t       value   = 0;
		if (m_gpu_modified_ranges.Intersects(address, 4) && m_known_stores.TryReadDwords(address, 4, &value)) {
			data[i] = value;
		}
	}
}

bool BufferCache::TryReadCurrentDword(uint64_t vaddr, uint32_t* value) {
	if ((vaddr & 3u) != 0) {
		return false;
	}
	if (HasGpuDirtyBytes(vaddr, 4)) {
		return TryReadKnownStores(vaddr, 4, value);
	}
	if (m_texture_cache.HasGpuModifiedImage(vaddr, 4)) {
		return false;
	}
	return Libs::LibKernel::Memory::TryReadBacking(vaddr, value, 4);
}

void BufferCache::CollectKnownStores(uint64_t vaddr, uint64_t size,
                                     std::vector<std::pair<uint64_t, uint32_t>>& out, size_t limit) {
	if (m_known_stores_count.load(std::memory_order_relaxed) == 0) {
		return;
	}
	std::lock_guard lock(m_known_stores_mutex);
	m_known_stores.CollectDwords(vaddr, size, out, limit);
}

bool BufferCache::UseKnownStores(uint64_t vaddr, uint64_t size, uint32_t* data, bool count_hit) {
	if (size == 0 || size > 1024 || ((vaddr | size) & 3u) != 0 || !TryReadKnownStores(vaddr, size, data)) {
		return false;
	}
	const auto seconds = Config::DebugKnownVerifySeconds();
	const auto now_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (!Config::DebugKnownVerifyAll() && (seconds == 0 || !m_known_stores_verify.Due(now_ns, seconds))) {
		if (count_hit) {
			Common::GpuPhase::NoteKnownStore(Common::GpuPhase::KnownStore::Hit);
		}
		return true;
	}
	// Verify: drain, read the real bytes, compare. The known answer is not used.
	std::array<uint32_t, 256> real;
	Common::GpuPhase::NoteGpuThreadFault();
	ReadMemory(vaddr, size);
	if (!Libs::LibKernel::Memory::TryReadBacking(vaddr, real.data(), size)) {
		return false;
	}
	Common::GpuPhase::NoteKnownStore(Common::GpuPhase::KnownStore::Verified);
	if (std::memcmp(real.data(), data, size) != 0) {
		Common::GpuPhase::NoteKnownStore(Common::GpuPhase::KnownStore::Mismatch);
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
			std::printf("KNOWN-STORES MISMATCH addr=0x%016" PRIx64 " size=%" PRIu64 " known0=0x%08x real0=0x%08x: "
			            "the recorded store knowledge was wrong, dropped\n",
			            vaddr, size, data[0], real[0]);
			std::fflush(stdout);
		}
		ForgetKnownStores(vaddr, size);
	}
	std::memcpy(data, real.data(), size);
	return true;
}

bool BufferCache::HasUnknownGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	if (!HasGpuDirtyBytes(vaddr, size)) {
		return false;
	}
	std::array<uint32_t, 256> scratch;
	// A verification drains and leaves the range clean (its real bytes are in the guest copy).
	return !UseKnownStores(vaddr, size, scratch.data(), true);
}

void BufferCache::RecordKnownFill(uint64_t vaddr, uint64_t size, uint32_t value) {
	if (!Config::KnownFillsEnabled()) {
		return;
	}
	std::lock_guard lock(m_known_fills_mutex);
	m_known_fills.Record(vaddr, size, value);
}

bool BufferCache::TryGetKnownFill(uint64_t vaddr, uint64_t size, uint32_t* value) {
	if (!Config::KnownFillsEnabled()) {
		return false;
	}
	std::lock_guard lock(m_known_fills_mutex);
	return m_known_fills.TryGet(vaddr, size, value);
}

bool BufferCache::TryDiscardGpuModified(uint64_t vaddr, uint64_t size) {
	const uint64_t page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const uint64_t page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	if (HasEagerPending(page_begin, page_end - page_begin)) {
		return false; // an eager copy would still write its bytes over what the caller is about to write
	}
	if ((page_begin < vaddr && m_gpu_modified_ranges.Intersects(page_begin, vaddr - page_begin)) ||
	    (vaddr + size < page_end &&
	     m_gpu_modified_ranges.Intersects(vaddr + size, page_end - (vaddr + size)))) {
		return false;
	}
	m_gpu_modified_ranges.Subtract(vaddr, size);
	m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
	ForgetKnownFills(vaddr, size);
	return true;
}

bool BufferCache::TryReadCleanBytes(uint64_t vaddr, void* data, uint64_t size) {
	if (size == 0 || size > UINT64_MAX - vaddr || m_gpu_modified_ranges.Intersects(vaddr, size) ||
	    HasEagerPending(vaddr, size)) {
		return false;
	}
	if (m_texture_cache.HasGpuModifiedImage(vaddr, size)) {
		return false;
	}
	return Libs::LibKernel::Memory::TryReadBacking(vaddr, data, size);
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	ForgetKnownFills(vaddr, size);
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] {
		                                  ReadMemory(vaddr, size, true,
		                                             Common::GpuPhase::DrainCause::ReadbackCpuWrite);
	                                  },
	                                  Config::WriteFaultAroundEnabled());
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write,
                             Common::GpuPhase::DrainCause cause) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (SkipDeps::Poisoned()) {
		// --debug-skip-log: the guest CPU reads (or overwrites) memory a skipped draw left stale.
		if (is_write) {
			SkipDeps::Global().NoteOverwrite({vaddr, size});
		} else {
			std::vector<SkipDeps::Hit> hits;
			SkipDeps::NoteRead({vaddr, size}, SkipDeps::ReadKind::CpuRead, 0, "cpu_readback", hits);
		}
	}
	// A fault taken by the GPU thread itself (it parses shader metadata straight from guest
	// memory) is a different problem from a game thread reading results.
	if (cause == Common::GpuPhase::DrainCause::ReadbackFault && GuestGpu::IsGpuThread()) {
		cause = Common::GpuPhase::DrainCause::ReadbackFaultGpu;
	}
	// A game thread's read fault (not the GPU thread's own, which an eager copy would only delay).
	const bool guest_read_fault = cause == Common::GpuPhase::DrainCause::ReadbackFault;
	// ... and a game thread's write over GPU-written pages: it needs the GPU's bytes in guest memory
	// first (the rest of the page must survive), but the GPU thread need not wait for them.
	const bool guest_write_fault =
	    is_write && cause == Common::GpuPhase::DrainCause::ReadbackCpuWrite && !GuestGpu::IsGpuThread();
	if ((guest_read_fault || guest_write_fault) && Config::ReadbackFuturesEnabled() &&
	    ReadMemoryFuture(vaddr, size, is_write)) {
		return;
	}
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, cause,
	                                                guest_read_fault] {
		Common::GpuPhase::DrainCauseScope drain_cause(cause);
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		if (guest_read_fault && Config::EagerReadbackEnabled()) {
			m_hot_pages.NoteFault(Common::AlignDown(vaddr, TRACKER_PAGE_SIZE));
		}
		// An eager write-back in flight has already taken its bytes out of the GPU-modified
		// ranges, so finish it (and unprotect what it covered) before looking for anything left to
		// read. This runs whether or not --eager-readback is on right now: the flag can be switched
		// off at run time (--toggle-control) while copies are in flight.
		{
			const auto first_page = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
			RetireHotPages(first_page, Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE) - first_page,
			               true);
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

		if (DownloadBufferMemory<false>(buffer, vaddr, size)) {
			// A write releases the same range below, in the same critical section as its CPU
			// claim: releasing it here as well would be a second lock acquisition on it, which is
			// the window this change closes.
			if (!is_write) {
				m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
			}
		}
		if (is_write) {
			// The guest writes these bytes, so whatever the GPU wrote is superseded -- and the
			// GPU's release and the CPU's claim have to happen under one lock. As two calls, an
			// upload on the renderer thread can re-mark a page GPU-dirty in between and
			// RegionManager then exits on "CPU dirty state conflicts with GPU dirty state". This
			// is also the path a write takes when there is nothing to download: the old code
			// marked the page CPU-dirty over the GPU's untouched claim.
			m_memory_tracker.TakeGpuPagesForCpu(vaddr, size);
		}
	});
}

// ---- --eager-readback ----------------------------------------------------------------------------
// Ported from ogeidexe/KytyPS5@76af431df and 663a8d887 (GPL-2.0, same licence as this fork), adapted
// to this tree; the bounded hot set follows BryanKAdams/BryKytyPS5@2fe8f1961 (see hotPages.h).

Buffer* BufferCache::FindExistingBuffer(uint64_t vaddr) {
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner) {
		return nullptr;
	}
	auto& buffer = m_slot_buffers[*owner];
	return buffer.is_deleted || !buffer.IsInBounds(vaddr, 1) ? nullptr : &buffer;
}

bool BufferCache::HasEagerPending(uint64_t vaddr, uint64_t size) {
	if (m_eager_pending_ranges.Empty() || !m_eager_pending_ranges.Intersects(vaddr, size)) {
		return false;
	}
	// The write-back may have landed since: finish what completed (never blocks), then look again.
	RetireHotPages(vaddr, size, false);
	return m_eager_pending_ranges.Intersects(vaddr, size);
}

// DownloadBufferMemory for an eager write-back, which is only an optimization and so must never
// block: staging space is reserved without waiting before any dirty range is taken, and when none
// is free the page is simply left for the next submit (or an on-demand read) to handle.
bool BufferCache::EagerDownload(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	uint64_t needed = 0;
	m_gpu_modified_ranges.ForEachInRange(vaddr, size, [&](uint64_t start, uint64_t end) {
		needed += Common::AlignUp(end - start, 64);
	});
	if (needed == 0) {
		return false;
	}
	const auto [mapped, offset] = m_download_buffer.Map(needed, 64, false);
	if (mapped == nullptr) {
		Common::GpuPhase::NoteEager(Common::GpuPhase::Eager::SkippedNoSpace);
		return false;
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total          = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	CollectDownloadCopies(buffer_address, vaddr, size, copies, total);
	if (copies.empty()) {
		return false;
	}
	EXIT_IF(total > needed);
	m_download_buffer.Commit();
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), m_download_buffer.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());
	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = m_download_buffer.Handle();
	after.offset        = offset;
	after.size          = needed;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	// The guest copy of these bytes is stale until the write-back below has run.
	for (const auto& copy: copies) {
		m_eager_pending_ranges.Add(buffer_address + copy.srcOffset, copy.size);
	}
	m_scheduler.DeferPriorityOperation([this, mapped, offset, needed, buffer_address,
	                                    copies = std::move(copies)] {
		m_download_buffer.Invalidate(offset, needed);
		const Libs::LibKernel::Memory::ScopedBackingWriteTag write_tag(
		    Libs::LibKernel::Memory::BackingWriteTag::Download);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
	});
	return true;
}

void BufferCache::WriteBackHotPages() {
	if (m_hot_pages.Empty()) {
		return;
	}
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		return;
	}
	RetireHotPages(0, UINT64_MAX, false);
	// With the flag off (it can be switched off at run time) copies already in flight are still
	// retired above, but no new ones are scheduled.
	if (!Config::EagerReadbackEnabled()) {
		return;
	}
	m_hot_pages.ForEachInRange(0, UINT64_MAX, [&](uint64_t page, HotPageSet::Entry& entry) {
		(void)ScheduleEagerPage(page, entry);
	});
}

bool BufferCache::ScheduleEagerPage(uint64_t page, HotPageSet::Entry& entry) {
	if (entry.in_flight_tick != 0 || !m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE)) {
		return false;
	}
	auto* buffer = FindExistingBuffer(page);
	if (buffer == nullptr) {
		return false;
	}
	const auto begin = std::max(page, buffer->CpuAddress());
	const auto end   = std::min(page + TRACKER_PAGE_SIZE, buffer->CpuAddress() + buffer->Size());
	if (begin < end && EagerDownload(*buffer, begin, end - begin)) {
		// The copy and its deferred write-back belong to the command buffer about to be
		// submitted as this tick.
		entry.in_flight_tick = m_scheduler.CurrentTick();
		Common::GpuPhase::NoteEager(Common::GpuPhase::Eager::Scheduled);
		return true;
	}
	return false;
}

void BufferCache::RetireHotPages(uint64_t vaddr, uint64_t size, bool wait) {
	if (!m_hot_pages.HasInFlight()) {
		return;
	}
	auto&      master = m_scheduler.GetMasterSemaphore();
	const auto end    = size > UINT64_MAX - vaddr ? UINT64_MAX : vaddr + size;
	master.Refresh();
	m_hot_pages.ForEachInRange(
	    Common::AlignDown(vaddr, TRACKER_PAGE_SIZE), end,
	    [&](uint64_t page, HotPageSet::Entry& entry) {
		    const auto tick = entry.in_flight_tick;
		    if (tick == 0) {
			    return;
		    }
		    if (!master.IsFree(tick) || !m_scheduler.PriorityOperationsDone(tick)) {
			    if (DecideRetire(false, wait, false) == HotRetire::Wait) {
				    return;
			    }
			    m_scheduler.Wait(tick);
			    m_scheduler.WaitPriorityOperations(tick);
		    }
		    entry.in_flight_tick = 0;
		    // The bytes are in guest memory now.
		    m_eager_pending_ranges.Subtract(page, TRACKER_PAGE_SIZE);
		    // A newer GPU write landed after the copy was recorded: stay protected; its bytes are
		    // written back by a later pass or read back on demand.
		    const bool newer = m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE);
		    if (DecideRetire(true, wait, newer) == HotRetire::StayProtected) {
			    return;
		    }
		    // The garbage collector may have downloaded and released the page meanwhile.
		    if (!m_memory_tracker.IsRegionGpuModified(page, TRACKER_PAGE_SIZE)) {
			    return;
		    }
		    if (Config::EagerReadbackVerifyEnabled() && !VerifyWrittenBack(page)) {
			    return;
		    }
		    m_memory_tracker.UnmarkRegionAsGpuModified(page, TRACKER_PAGE_SIZE);
		    Common::GpuPhase::NoteEager(Common::GpuPhase::Eager::Retired);
	    });
}

// --debug-eager-readback-verify: before unprotecting a written-back page, read the same bytes
// straight from the GPU buffer (a full synchronous readback, the old conservative path) and compare.
bool BufferCache::VerifyWrittenBack(uint64_t page) {
	m_scheduler.Finish();
	auto* buffer = FindExistingBuffer(page);
	if (buffer == nullptr) {
		return true;
	}
	const auto begin = std::max(page, buffer->CpuAddress());
	const auto end   = std::min(page + TRACKER_PAGE_SIZE, buffer->CpuAddress() + buffer->Size());
	const auto size  = end - begin;
	const auto [mapped, offset] = m_download_buffer.Map(size, 64);
	EXIT_IF(mapped == nullptr);
	m_download_buffer.Commit();
	auto&                   command = m_scheduler.Current();
	vk::BufferCopy          copy {begin - buffer->CpuAddress(), offset, size};
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer->Handle();
	barrier.offset              = 0;
	barrier.size                = buffer->Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &barrier, 0,
	                       nullptr);
	native.copyBuffer(buffer->Handle(), m_download_buffer.Handle(), 1, &copy);
	m_scheduler.Finish();
	m_download_buffer.Invalidate(offset, size);
	Common::GpuPhase::NoteEager(Common::GpuPhase::Eager::VerifyChecks);
	std::vector<uint8_t> backing(size);
	if (!Libs::LibKernel::Memory::TryReadBacking(begin, backing.data(), size)) {
		return true;
	}
	if (std::memcmp(backing.data(), mapped, size) == 0) {
		return true;
	}
	size_t first = 0;
	while (first < size && backing[first] == mapped[first]) {
		++first;
	}
	Common::GpuPhase::NoteEager(Common::GpuPhase::Eager::VerifyMismatches);
	std::printf("EAGER READBACK MISMATCH: page=0x%016" PRIx64 " first_diff=+0x%zx written_back=0x%02x "
	            "gpu=0x%02x last_gpu_write_tick=%" PRIu64 " gpu_done_tick=%" PRIu64 "\n",
	            page, first, backing[first], mapped[first], buffer->last_gpu_write_tick,
	            m_scheduler.GetMasterSemaphore().KnownGpuTick());
	std::fflush(stdout);
	return false;
}

// ---- --readback-futures ---------------------------------------------------------------------------
// A game thread's read of GPU-written memory used to stop the GPU thread: it copied, submitted and
// waited for the whole queue of submitted work before it published the bytes. Nothing the GPU
// thread does next depends on the copy, so only the reading thread waits now (the three steps are
// described in readbackFuture.h and notes/frame-pipelining.md section 3). The copy reuses the eager
// write-back bookkeeping: the bytes stay in m_eager_pending_ranges until they have landed, and the
// page stays protected until then.

bool BufferCache::ReadMemoryFuture(uint64_t vaddr, uint64_t size, bool is_write) {
	auto& gpu = m_scheduler.Context().GetGpu();
	for (uint32_t round = 0;; ++round) {
		auto        release = std::make_shared<FutureRelease>();
		FutureStart start   = FutureStart::Sync;
		gpu.SendCommandSync([&] { start = StartReadFuture(vaddr, size, release, is_write); });
		if (start == FutureStart::Sync) {
			Common::GpuPhase::NoteFuture(Common::GpuPhase::Future::Sync);
			return false;
		}
		Common::GpuPhase::NoteFuture(round == 0 ? Common::GpuPhase::Future::Started
		                                        : Common::GpuPhase::Future::Retried);
		const auto wait_begin = std::chrono::steady_clock::now();
		release->Wait();
		Common::GpuPhase::NoteFuture(
		    Common::GpuPhase::Future::Waited,
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - wait_begin)
		                              .count()));
		bool readable = false;
		const bool last_round = round + 1 >= kMaxFutureRounds;
		gpu.SendCommandSync([&] { readable = FinishReadFuture(vaddr, size, is_write, last_round, release->tick); });
		switch (NextFutureRound(round, readable)) {
			case FutureRound::Done: return true;
			case FutureRound::Retry: break;
			case FutureRound::Fallback:
				Common::GpuPhase::NoteFuture(Common::GpuPhase::Future::Exhausted);
				return false;
		}
	}
}

FutureStart BufferCache::StartReadFuture(uint64_t vaddr, uint64_t size,
                                         const std::shared_ptr<FutureRelease>& release,
                                         bool is_write) {
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return FutureStart::Sync; // the synchronous path returns at once for such a write
	}
	const auto first = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto last  = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	++m_future_starts;
	for (auto page = first; page < last; page += TRACKER_PAGE_SIZE) {
		const auto marked = m_future_contended.find(page);
		if (marked != m_future_contended.end()) {
			if (IsContended(marked->second, m_future_starts)) {
				Common::GpuPhase::NoteFuture(Common::GpuPhase::Future::Contended);
				return FutureStart::Sync;
			}
			m_future_contended.erase(marked);
		}
	}
	// What completed since the last submit lands first (never waits).
	RetireHotPages(first, last - first, false);
	uint32_t scheduled = 0;
	bool     in_flight = false;
	for (auto page = first; page < last; page += TRACKER_PAGE_SIZE) {
		if (m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE)) {
			// A page with a copy in flight and newer bytes cannot take a second copy yet: the
			// thread waits for the first, finds the page still protected and runs another round.
			m_hot_pages.NoteFault(page);
			if (m_hot_pages.Contains(page) && m_hot_pages.InFlight(page) == 0) {
				m_hot_pages.ForEachInRange(page, page + TRACKER_PAGE_SIZE,
				                           [&](uint64_t hot_page, HotPageSet::Entry& entry) {
					                           if (ScheduleEagerPage(hot_page, entry)) {
						                           ++scheduled;
					                           }
				                           });
			}
		}
		if (m_hot_pages.InFlight(page) != 0) {
			in_flight = true;
		}
	}
	if (DecideFutureStart(scheduled, in_flight) == FutureStart::Sync) {
		return FutureStart::Sync;
	}
	// The release runs behind the write-back (priority operations are served in order), after the
	// tick that holds the copy has completed. A copy recorded just now sits in the open command
	// buffer, which must be submitted for that; a copy already in flight is behind a tick that was
	// submitted, and the release simply follows that tick (no extra submit).
	uint64_t wait_tick = scheduled != 0 ? m_scheduler.CurrentTick() : 0;
	for (auto page = first; page < last; page += TRACKER_PAGE_SIZE) {
		wait_tick = std::max(wait_tick, m_hot_pages.InFlight(page));
	}
	release->tick = wait_tick;
	if (wait_tick < m_scheduler.CurrentTick()) {
		m_scheduler.DeferPriorityOperationAt([release] { release->Signal(); }, wait_tick);
		// Priority operations run in order: if one queued before this release waits for the open
		// command buffer (an event the submit coalescer is holding back), the release is stuck behind
		// it until somebody submits, and the GPU thread may have nothing else to do. Submit.
		if (m_scheduler.HasOpenTickPriorityOperation()) {
			m_scheduler.Flush();
		}
	} else {
		m_scheduler.DeferPriorityOperation([release] { release->Signal(); });
		m_scheduler.Flush();
	}
	return FutureStart::Wait;
}

bool BufferCache::FinishReadFuture(uint64_t vaddr, uint64_t size, bool is_write, bool last_round,
                                   uint64_t own_tick) {
	const auto first = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto last  = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	// The thread's own tick has completed and its write-back has run (the release came after both);
	// the priority-operations thread may still be flagging them done. Pages whose copy is a newer
	// one (another thread's, still in flight) stay protected: the thread runs another round instead
	// of the GPU thread waiting for that copy.
	const auto spin_begin = std::chrono::steady_clock::now();
	while (own_tick != 0 && !m_scheduler.PriorityOperationsDone(own_tick) &&
	       std::chrono::steady_clock::now() - spin_begin < std::chrono::milliseconds(2)) {
		std::this_thread::yield();
	}
	RetireHotPages(first, last - first, false);
	if (m_memory_tracker.IsRegionGpuModified(first, last - first)) {
		// A newer GPU write kept the pages protected through every round: remember them, so that
		// pages the GPU keeps rewriting are not retried again and again (bounded).
		if (last_round) {
			if (m_future_contended.size() >= 256) {
				m_future_contended.erase(m_future_contended.begin());
			}
			for (auto page = first; page < last; page += TRACKER_PAGE_SIZE) {
				if (m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE)) {
					m_future_contended[page] = m_future_starts;
				}
			}
		}
		return false;
	}
	if (is_write) {
		// Nothing newer is on the GPU: give the pages to the CPU in one critical section, as the
		// synchronous path does after its download. This runs on the GPU thread, so no GPU write
		// can be recorded between the check above and the claim.
		m_memory_tracker.TakeGpuPagesForCpu(vaddr, size);
		for (auto page = first; page < last; page += TRACKER_PAGE_SIZE) {
			m_hot_pages.Forget(page);
		}
	}
	return true;
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	// Merging copies the old buffers' contents, GPU-written ranges included, in the open command
	// buffer: that is the producer a readback of the new buffer has to wait for.
	m_slot_buffers[id].last_gpu_write_tick = m_scheduler.CurrentTick();
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	// Only mapped guest memory is read. Pages the guest unmapped stay CPU-dirty (UnmapMemory
	// invalidates them) until memory is mapped there again, and an image's range can reach into
	// them: a texture of a streamed pool whose layers the game released. The kernel's committed
	// ranges tell (a PRT aperture is mapped for the GPU as a whole, its pages one by one); not
	// GpuResourceManager's ranges, whose lock PrepareBda holds around this.
	if (LibKernel::Memory::IsFullyMapped(vaddr, size)) {
		UploadDirtyRanges(buffer, vaddr, size, is_written);
	} else {
		std::vector<std::pair<uint64_t, uint64_t>> parts;
		LibKernel::Memory::MappedParts(vaddr, size, &parts);
		for (const auto& [address, bytes]: parts) {
			UploadDirtyRanges(buffer, address, bytes, is_written);
		}
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

// The CPU-dirty pages of a mapped range copied into the buffer (the written ones then GPU-owned).
void BufferCache::UploadDirtyRanges(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                    bool is_written) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
		    if (m_upload_capture != nullptr) {
			    m_upload_capture->emplace_back(address, bytes);
		    }
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	// A buffer may cover non-resident pages of a PRT aperture, which read as zero; a host
	// read there would fault back into the tracker from inside its upload callback.
	const auto read_guest = [](void* destination, uint64_t address, uint64_t size) {
		if (!LibKernel::Memory::TryReadBacking(address, destination, size) &&
		    !LibKernel::Memory::TryReadSparseBacking(address, destination, size)) {
			std::memcpy(destination, reinterpret_cast<const void*>(address), size);
		}
	};
	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			read_guest(mapped + copy.srcOffset, address, copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		read_guest(temporary->Mapped().data() + copy.srcOffset, address, copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	// --resolved-draw: a read-only range found clean earlier whose tracker epochs and buffer-set
	// generation have not moved is still clean, in the same buffer. Texel buffers also look at images
	// and written ranges mark GPU ownership, so neither is memoized.
	const bool memoizable = Config::ResolvedDrawEnabled() && !is_written && !is_texel_buffer;
	ObtainMemo* memo      = nullptr;
	uint32_t    epochs[2] = {};
	uint32_t    epoch_count = 0;
	m_obtain_stats.calls++;
	m_obtain_stats.written += is_written;
	m_obtain_stats.texel += !is_written && is_texel_buffer;
	if (memoizable) {
		memo = &m_obtain_memo[((vaddr >> 4) * 0x9e3779b97f4a7c15ull ^ size) >> 40 & (kObtainMemoSlots - 1)];
		epoch_count = m_memory_tracker.RangeEpochs(vaddr, size, epochs);
		if (memo->valid && memo->vaddr == vaddr && memo->size == size && epoch_count != 0 &&
		    memo->epoch_count == epoch_count && memo->epochs[0] == epochs[0] &&
		    (epoch_count < 2 || memo->epochs[1] == epochs[1]) &&
		    memo->buffer_generation == BufferSetGeneration()) {
			BufferId use = memo->id;
			if (!IsBufferInvalid(id) && m_slot_buffers[id].IsInBounds(vaddr, size)) {
				use = id; // the caller's own buffer wins, as in the full path
			}
			if (!IsBufferInvalid(use) && m_slot_buffers[use].IsInBounds(vaddr, size)) {
				auto& buffer = m_slot_buffers[use];
				if (Config::ResolvedDrawVerify()) {
					const auto q = m_memory_tracker.QueryDirty(vaddr, size);
					// A guest write can land between the epoch check and this query: that is a legal
					// order of the two events, and the epochs have moved by then. Only a range that is
					// dirty while its epochs still equal the remembered ones is a wrong memo.
					uint32_t after[2] = {};
					const auto after_count = m_memory_tracker.RangeEpochs(vaddr, size, after);
					const bool unchanged = after_count == memo->epoch_count && after[0] == memo->epochs[0] &&
					                       (after_count < 2 || after[1] == memo->epochs[1]);
					if (unchanged && (q.cpu || q.gpu)) {
						EXIT("resolved draw: a remembered clean range is dirty (cpu=%d gpu=%d) addr=0x%016" PRIx64
						     " size=%" PRIu64 "\n",
						     q.cpu ? 1 : 0, q.gpu ? 1 : 0, vaddr, size);
					}
				}
				TouchBuffer(buffer);
				m_obtain_stats.memo_hits++;
				return {&buffer, buffer.Offset(vaddr)};
			}
		}
	}
	MemoryTracker::DirtyState dirty {true, true};
	bool                      dirty_known = false;
	if (!is_written && (size <= CACHING_PAGESIZE || (memo != nullptr && memo->seen))) {
		dirty       = m_memory_tracker.QueryDirty(vaddr, size);
		dirty_known = true;
	}
	if (!is_written && size <= CACHING_PAGESIZE && !dirty.gpu && dirty.cpu) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			std::memcpy(mapped, reinterpret_cast<const void*>(vaddr), size);
			m_stream_buffer.Commit();
			m_obtain_stats.small_copies++;
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (memo != nullptr) {
		if (dirty_known && !dirty.cpu && !dirty.gpu && epoch_count != 0) {
			// Clean when the epochs were read (before the query): remember it.
			*memo = {.vaddr = vaddr, .size = size, .id = id, .buffer_generation = BufferSetGeneration(),
			         .epoch_count = epoch_count, .epochs = {epochs[0], epochs[1]}, .valid = true, .seen = true};
			m_obtain_stats.stored++;
		} else {
			memo->valid = false;
			memo->vaddr = vaddr;
			memo->size  = size;
			memo->seen  = true;
		}
	}
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		buffer.last_gpu_write_tick = m_scheduler.CurrentTick();
		ForgetKnownFills(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	// --mem-residency attributes the guest reads below to GPU image uploads.
	const Libs::LibKernel::Memory::ScopedBackingReadTag read_tag(
	    Libs::LibKernel::Memory::BackingReadTag::Image);
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
	RecordKnownFill(vaddr, size, value);
}

void BufferCache::CopyGdsBuffer(uint64_t dst_offset, uint64_t src_offset, uint64_t size) {
	if (size == 0) {
		return;
	}
	if (((dst_offset | src_offset | size) & 3u) != 0 || dst_offset > m_gds_buffer.Size() ||
	    size > m_gds_buffer.Size() - dst_offset || src_offset > m_gds_buffer.Size() ||
	    size > m_gds_buffer.Size() - src_offset) {
		EXIT("BufferCache: invalid GDS copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     src_offset, dst_offset, size);
	}
	// On-chip store-to-store DMA. It runs in GPU order like the other GDS copies, and a range that
	// overlaps itself is copied in memmove order as non-overlapping regions.
	auto& command = m_scheduler.Current();
	for (const auto& chunk: PlanGdsCopy(dst_offset, src_offset, size)) {
		m_gds_buffer.CopyFrom(command, m_gds_buffer, chunk.source, chunk.destination, chunk.size);
	}
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size) || HasEagerPending(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::PollLoopGuard() {
	const auto limit = Config::GetShaderLoopLimit();
	if (limit == 0 || m_loop_guard_buffer.Mapped().empty()) {
		return;
	}
	// Best effort, like Sync::ReadGds: the shaders write the slots with device atomics into this
	// mapped stream buffer while it is read.
	std::array<uint32_t, Common::LoopGuard::Slots> tail {};
	std::memcpy(tail.data(), m_loop_guard_buffer.Mapped().data(), sizeof(tail));
	for (const auto& line: Common::LoopGuard::GetMonitor().Observe(tail, limit)) {
		LOGF("%s", line.c_str());
		std::printf("%s", line.c_str());
		std::fflush(stdout);
	}
}

void BufferCache::CollectResidencyRanges(MemResidency::Snapshot& snapshot) {
	auto& ranges = snapshot.ranges[static_cast<size_t>(MemResidency::RangeClass::Buffer)];
	for (const auto& [vaddr, id]: m_buffers) {
		if (IsBufferInvalid(id)) {
			continue;
		}
		ranges.emplace_back(vaddr, m_slot_buffers[id].Size());
	}
}

void BufferCache::LogRingStatsPeriodic() {
	if (!Config::MemStatsEnabled()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < m_ring_log_next) {
		return;
	}
	m_ring_log_next = now + std::chrono::seconds(10);
	const auto stats = m_staging_buffer.GetRingStats();
	Log::WriteToConsoleAndLog(fmt::format(
	    "[upload-ring] {} {} MiB: reserved {} MiB in {} wraps, {} blocking waits ({:.1f} ms total)\n",
	    m_staging_buffer.IsDeviceLocal() ? "device-local" : "system", m_staging_buffer.Size() >> 20,
	    stats.bytes >> 20, stats.wraps, stats.waits, static_cast<double>(stats.wait_ns) / 1e6));
	const auto& o = m_obtain_stats;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[obtain-buffer] total: calls={} memo_hits={} stored={} small_copies={} written={} texel={}\n", o.calls,
	    o.memo_hits, o.stored, o.small_copies, o.written, o.texel));
	const auto mapping = LibKernel::Memory::GetMappingMemoStats();
	Log::WriteToConsoleAndLog(fmt::format("[mapping-memo] gpu thread: hits={} misses={}\n", mapping.hits,
	                                      mapping.misses));
	const auto backing = LibKernel::Memory::GetBackingReadCacheStats();
	Log::WriteToConsoleAndLog(fmt::format("[backing-read-cache] gpu thread: hits={} misses={}\n", backing.hits,
	                                      backing.misses));
}

void BufferCache::RunGarbageCollector() {
	if ((m_gc_runs & 0xffu) == 0) {
		PollLoopGuard();
	}
	LogRingStatsPeriodic();
	const bool     frame_clock = Config::GcClockFrames();
	const uint64_t run         = m_gc_runs++;
	uint64_t       tick        = 0;
	if (frame_clock) {
		tick      = std::max(m_gc_tick, GcClock::Tick(GcClock::Mode::Frames, run,
		                                              Common::GpuPhase::Frames()));
		m_gc_tick = tick;
	} else {
		tick = m_gc_tick++;
	}
	const auto clock_mode = frame_clock ? GcClock::Mode::Frames : GcClock::Mode::Runs;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	// Device usage is shared with the texture cache, so it alone can keep this cache aggressive (a
	// full GPU drain per download) for pressure that freeing buffers cannot relieve (K42).
	const bool     aggressive = BufferGcGate::Aggressive(m_total_used_memory, m_critical_gc_memory,
	                                                      m_registered_bytes,
	                                                      Config::GcBufferOwnSharePercent());
	m_gc_stats.runs++;
	m_gc_stats.aggressive += aggressive ? 1 : 0;
	if (m_gc_stats.runs % 1024 == 0) {
		// Cumulative; the difference between two lines is a window. One line per 1024 runs that
		// were past the trigger, so it only appears under memory pressure.
		Log::WriteToConsoleAndLog(fmt::format(
		    "BufferGC: runs={} aggressive={} freed_clean={} downloaded_dirty={} drains={} "
		    "own={}MiB device={}MiB critical={}MiB share={}%\n",
		    m_gc_stats.runs, m_gc_stats.aggressive, m_gc_stats.freed, m_gc_stats.downloads,
		    m_gc_stats.drains, m_registered_bytes >> 20, m_total_used_memory >> 20,
		    m_critical_gc_memory >> 20, Config::GcBufferOwnSharePercent()));
	}
	const auto     ages       = GcClock::BufferAgesFor(clock_mode, Config::GetVblankFrequency());
	const uint64_t want_age   = aggressive ? ages.aggressive : ages.normal;
	if (!GcClock::AgeReached(clock_mode, tick, want_age)) {
		return;
	}
	const uint64_t age        = std::min<uint64_t>(want_age, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	m_gc_stats.freed += retire_count - dirty_buffers.size();
	if (dirty_buffers.empty()) {
		return;
	}
	m_gc_stats.downloads += dirty_buffers.size();
	m_gc_stats.drains++;

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	Common::GpuPhase::DrainCauseScope drain_cause(Common::GpuPhase::DrainCause::BufferGc);
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
