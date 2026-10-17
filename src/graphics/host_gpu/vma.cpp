#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/gpuPhaseStats.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vramPolicy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <fmt/format.h>
#include <mutex>
#include <optional>
#include <string>

namespace Libs::Graphics {

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	// 64 MiB blocks (VMA's default is 256 MiB on large heaps): a freed image leaves a smaller
	// hole, so more blocks become completely empty and are handed back to the driver.
	info.preferredLargeHeapBlockSize = 64ull << 20;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}
	if (memory_priority_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		VRAM_LOG("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	FreeBallast();
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		VRAM_LOG("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

void GraphicContext::LogMemoryBudgetPeriodic() const {
	if (allocator == nullptr || !memory_budget_ext_enabled) {
		return;
	}
	static std::atomic<int64_t> last_ms {0};
	const auto                  now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                           std::chrono::steady_clock::now().time_since_epoch())
	                           .count();
	auto previous = last_ms.load(std::memory_order_relaxed);
	if (now_ms - previous < 5000 ||
	    !last_ms.compare_exchange_strong(previous, now_ms, std::memory_order_relaxed)) {
		return;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < physical_device_memory_properties.memoryHeapCount; i++) {
		if (!(physical_device_memory_properties.memoryHeaps[i].flags &
		      vk::MemoryHeapFlagBits::eDeviceLocal)) {
			continue;
		}
		VRAM_LOG("VRAM heap %u: usage=%" PRIu64 " MiB, budget=%" PRIu64 " MiB, allocated=%" PRIu64
		     " MiB, blocks=%" PRIu64 " MiB\n",
		     i, static_cast<uint64_t>(budgets[i].usage) >> 20,
		     static_cast<uint64_t>(budgets[i].budget) >> 20,
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes) >> 20,
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes) >> 20);
		VRAM_LOG("VRAM heap %u: driver usage minus VMA blocks=%" PRId64 " MiB, pipelines gfx=%u comp=%u, "
		     "vma blocks=%u allocs=%u dedicated_images=%u host_fallback=%u\n",
		     i,
		     (static_cast<int64_t>(budgets[i].usage) -
		      static_cast<int64_t>(budgets[i].statistics.blockBytes)) >> 20,
		     pipelines_graphics.load(), pipelines_compute.load(),
		     budgets[i].statistics.blockCount, budgets[i].statistics.allocationCount,
		     dedicated_images.load(), host_fallback_images.load());
	}
	static constexpr const char* names[] = {"tex",       "rt",        "depth",
	                                        "buf_local", "buf_bda",   "buf_upload",
	                                        "buf_download", "buf_stream"};
	std::string split;
	for (size_t i = 0; i < vram_bytes.size(); i++) {
		split += fmt::format(" {}={}MiB({})", names[i], vram_bytes[i].load() >> 20,
		                     vram_count[i].load());
	}
	VRAM_LOG("VRAM split:%s ballast=%" PRIu64 "MiB\n", split.c_str(), ballast_bytes >> 20);
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	// The debug ballast simulates a smaller GPU: the caches must not see it as usage.
	return usage > ballast_bytes ? usage - ballast_bytes : 0;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		budget = budget > ballast_bytes ? budget - ballast_bytes : 0;
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

// A pooled image goes back to the driver: the accounting a delete does, then vmaDestroyImage.
static void DestroyPooledImage(GraphicContext& graphics, const GraphicContext::PooledImage& pooled) {
	graphics.AccountVram(pooled.vram_class, -static_cast<int64_t>(pooled.alloc_size));
	graphics.dedicated_images.fetch_sub(1, std::memory_order_relaxed);
	vmaDestroyImage(graphics.allocator, pooled.image, pooled.allocation);
}

void GraphicContext::FlushImagePool() {
	std::vector<PooledImage> taken;
	{
		std::lock_guard lock(m_image_pool_mutex);
		taken.swap(m_image_pool);
		m_image_pool_bytes = 0;
	}
	for (const auto& pooled: taken) {
		DestroyPooledImage(*this, pooled);
	}
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::VkAlloc);
	KYTY_PROFILER_FUNCTION();
	if (Config::ImagePoolEnabled()) {
		std::optional<PooledImage> hit;
		{
			std::lock_guard lock(m_image_pool_mutex);
			for (size_t i = 0; i < m_image_pool.size(); i++) {
				const auto& p = m_image_pool[i];
				if (p.format == image_info.format && p.image_type == image_info.imageType &&
				    p.extent == image_info.extent && p.layers == image_info.arrayLayers &&
				    p.mip_levels == image_info.mipLevels && p.samples == static_cast<uint32_t>(image_info.samples) &&
				    p.usage == image_info.usage && p.flags == image_info.flags &&
				    image_info.tiling == vk::ImageTiling::eOptimal && image_info.initialLayout == vk::ImageLayout::eUndefined) {
					hit = p;
					m_image_pool_bytes -= p.alloc_size;
					m_image_pool.erase(m_image_pool.begin() + static_cast<std::ptrdiff_t>(i));
					break;
				}
			}
		}
		if (hit) {
			// The pooled image keeps its memory (and whatever it held); the first use transitions it from
			// UNDEFINED like a new one. It was accounted when it was first created and still is.
			image.image      = hit->image;
			image.allocation = hit->allocation;
			image.alloc_size = hit->alloc_size;
			image.vram_class = hit->vram_class;
			image.dedicated  = true;
			image.format     = image_info.format;
			image.image_type = image_info.imageType;
			image.extent     = image_info.extent;
			image.layers     = image_info.arrayLayers;
			image.mip_levels = image_info.mipLevels;
			image.samples    = static_cast<uint32_t>(image_info.samples);
			image.usage      = image_info.usage;
			image.flags      = image_info.flags;
			image.state      = {.layout = image_info.initialLayout};
			image.subresource_states.clear();
			image_churn.pool_hits.fetch_add(1, std::memory_order_relaxed);
			image_churn.creates.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
	} else if (!m_image_pool.empty()) {
		FlushImagePool();
	}
	const auto churn_start = std::chrono::steady_clock::now();
	struct ChurnNote {
		GraphicContext& graphics;
		VulkanImage&    image;
		std::chrono::steady_clock::time_point start;
		~ChurnNote() {
			auto& c = graphics.image_churn;
			c.creates.fetch_add(1, std::memory_order_relaxed);
			c.create_bytes.fetch_add(image.alloc_size, std::memory_order_relaxed);
			c.dedicated_creates.fetch_add(image.dedicated ? 1 : 0, std::memory_order_relaxed);
			c.create_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			                          std::chrono::steady_clock::now() - start).count()),
			                      std::memory_order_relaxed);
		}
	} churn_note {*this, image, churn_start};
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	// Classification for priorities and --vram-stats. Block-compressed images are textures.
	// Depth images and large single-level uncompressed color images are treated as render
	// targets; everything else is a sampled texture.
	const bool is_depth =
	    static_cast<bool>(image_info.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment);
	// VK_FORMAT_BC1_RGB_UNORM_BLOCK (131) .. VK_FORMAT_ASTC_12x12_SRGB_BLOCK (184).
	const auto format_value = static_cast<int>(image_info.format);
	const bool is_block     = format_value >= 131 && format_value <= 184;
	const bool is_render_target =
	    !is_depth && !is_block &&
	    static_cast<bool>(image_info.usage & vk::ImageUsageFlagBits::eColorAttachment) &&
	    image_info.extent.width >= 512 && image_info.extent.height >= 512 &&
	    image_info.mipLevels == 1 && image_info.arrayLayers == 1;
	const VramClass vram_class = is_depth ? VramClass::ImageDepth
	                             : is_render_target ? VramClass::ImageRenderTarget
	                                                : VramClass::ImageTexture;

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	// Large images get their own VkDeviceMemory: freeing one returns exactly its size to the
	// driver instead of leaving a hole in a shared block (the texture-cache reclaim could free
	// 830 MB of allocations and only 73 MB of blocks).
	const VramPolicy::ImageShape shape {image_info.extent.width,  image_info.extent.height,
	                                    image_info.extent.depth,  image_info.arrayLayers,
	                                    image_info.mipLevels,     static_cast<uint32_t>(image_info.samples),
	                                    static_cast<int>(image_info.format)};
	if (VramPolicy::UseDedicatedAllocation(VramPolicy::EstimateImageBytes(shape),
	                                       dedicated_images.load(std::memory_order_relaxed))) {
		alloc_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	}
	if (memory_priority_enabled) {
		// Depth and render targets are rewritten every frame: keep them resident. VMA only
		// applies a priority to dedicated allocations, so those get their own VkDeviceMemory.
		// Sampled textures stay pooled at the default 0.5 and are the first to be paged out.
		const auto choice   = VramPolicy::ImagePriority(is_depth, is_render_target);
		alloc_info.priority = choice.priority;
		if (choice.dedicated) {
			alloc_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		}
	}

	vk::Image::CType native_image = VK_NULL_HANDLE;
	vk::Result       result       = vk::Result::eSuccess;
	const auto       try_allocate = [&](VramPolicy::AllocStage stage) {
		if (stage == VramPolicy::AllocStage::HostFallback) {
			// Any compatible memory type (system memory, or paged out by the driver when
			// --vram-paging is on).
			VRAM_LOG("VRAM exhausted: retrying image allocation without DEVICE_LOCAL\n");
			alloc_info.requiredFlags  = 0;
			alloc_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			alloc_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		}
		native_image     = VK_NULL_HANDLE;
		image.allocation = nullptr;
		result           = static_cast<vk::Result>(vmaCreateImage(
            allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info), &alloc_info,
            &native_image, &image.allocation, nullptr));
		if (result != vk::Result::eSuccess) {
			if (stage == VramPolicy::AllocStage::Normal) {
				VRAM_LOG("Image allocation failed: %s extent=%ux%ux%u format=%d layers=%u levels=%u\n",
				         vk::to_string(result).c_str(), image_info.extent.width,
				         image_info.extent.height, image_info.extent.depth,
				         static_cast<int>(image_info.format), image_info.arrayLayers,
				         image_info.mipLevels);
			}
			LogMemoryBudget();
		}
		if (stage == VramPolicy::AllocStage::AfterReclaim) {
			VRAM_LOG("Image allocation retry after reclaim: %s\n", vk::to_string(result).c_str());
		} else if (stage == VramPolicy::AllocStage::HostFallback) {
			VRAM_LOG("VRAM exhausted: fallback allocation result: %s\n",
			         vk::to_string(result).c_str());
		}
		return result == vk::Result::eSuccess;
	};
	const auto outcome = VramPolicy::RunAllocationLadder(
	    try_allocate,
	    [&] {
		    // Free what the caches can spare before trying once more.
		    FlushImagePool(); // pooled images are the cheapest memory to give back
		    in_reclaim = true;
		    reclaim_memory();
		    in_reclaim = false;
	    },
	    reclaim_memory && !in_reclaim);
	if (outcome.ok && outcome.stage == VramPolicy::AllocStage::HostFallback) {
		host_fallback_images.fetch_add(1, std::memory_order_relaxed);
	}
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		return false;
	}

	VmaAllocationInfo allocation_result {};
	vmaGetAllocationInfo(allocator, image.allocation, &allocation_result);
	image.alloc_size = allocation_result.size;
	{
		VmaAllocationInfo2 info2 {};
		vmaGetAllocationInfo2(allocator, image.allocation, &info2);
		image.dedicated = info2.dedicatedMemory != VK_FALSE;
		if (image.dedicated) {
			dedicated_images.fetch_add(1, std::memory_order_relaxed);
		}
	}
	image.vram_class = vram_class;
	AccountVram(vram_class, static_cast<int64_t>(image.alloc_size));

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::VkAlloc);
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	const auto delete_start = std::chrono::steady_clock::now();
	if (Config::ImagePoolEnabled() && image.dedicated && image.alloc_size >= (256u << 10u)) {
		// Kept for the next image of this shape: still accounted, still counted as dedicated.
		const auto cap = std::min<uint64_t>(256ull << 20u, GetTotalMemoryBudget() / 32u);
		std::vector<PooledImage> expired;
		bool                     pooled = false;
		{
			std::lock_guard lock(m_image_pool_mutex);
			const auto now = std::chrono::steady_clock::now();
			for (size_t i = 0; i < m_image_pool.size();) {
				if (now - m_image_pool[i].since > std::chrono::milliseconds(1500)) {
					m_image_pool_bytes -= m_image_pool[i].alloc_size;
					expired.push_back(m_image_pool[i]);
					m_image_pool.erase(m_image_pool.begin() + static_cast<std::ptrdiff_t>(i));
				} else {
					i++;
				}
			}
			if (m_image_pool_bytes + image.alloc_size <= cap) {
				PooledImage p;
				p.format = image.format; p.image_type = image.image_type; p.extent = image.extent;
				p.layers = image.layers; p.mip_levels = image.mip_levels; p.samples = image.samples;
				p.usage = image.usage; p.flags = image.flags;
				p.image = image.image; p.allocation = image.allocation; p.alloc_size = image.alloc_size;
				p.vram_class = image.vram_class; p.since = now;
				m_image_pool_bytes += p.alloc_size;
				m_image_pool.push_back(p);
				pooled = true;
			}
		}
		for (const auto& e: expired) {
			DestroyPooledImage(*this, e);
		}
		if (pooled) {
			image.image      = nullptr;
			image.allocation = nullptr;
			image.alloc_size = 0;
			image.dedicated  = false;
			image_churn.pool_puts.fetch_add(1, std::memory_order_relaxed);
			image_churn.deletes.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
	AccountVram(image.vram_class, -static_cast<int64_t>(image.alloc_size));
	if (image.dedicated) {
		dedicated_images.fetch_sub(1, std::memory_order_relaxed);
		image.dedicated = false;
	}
	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
	image.alloc_size = 0;
	image_churn.deletes.fetch_add(1, std::memory_order_relaxed);
	image_churn.delete_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                    std::chrono::steady_clock::now() - delete_start).count()),
	                                std::memory_order_relaxed);
}

void GraphicContext::AccountVram(VramClass vram_class, int64_t delta) noexcept {
	const auto index = static_cast<size_t>(vram_class);
	vram_bytes[index].fetch_add(delta, std::memory_order_relaxed);
	vram_count[index].fetch_add(delta > 0 ? 1 : -1, std::memory_order_relaxed);
}

bool GraphicContext::AllocateBallast(uint64_t mebibytes) {
	constexpr uint64_t chunk_mb = 256;
	uint64_t           left     = mebibytes;
	while (left > 0) {
		const uint64_t this_mb = std::min(left, chunk_mb);
		VkBufferCreateInfo buffer_info {};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size  = this_mb << 20;
		buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo alloc_info {};
		alloc_info.flags         = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		alloc_info.priority      = 1.0f;
		VkBuffer      buffer     = VK_NULL_HANDLE;
		VmaAllocation allocation = nullptr;
		const auto    result =
		    static_cast<vk::Result>(vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer,
		                                             &allocation, nullptr));
		if (result != vk::Result::eSuccess) {
			VRAM_LOG("VRAM ballast: allocation failed after %" PRIu64 " MiB: %s\n",
			     ballast_bytes >> 20, vk::to_string(result).c_str());
			return false;
		}
		ballast.emplace_back(buffer, allocation);
		ballast_bytes += this_mb << 20;
		left -= this_mb;
	}
	VRAM_LOG("VRAM ballast: holding %" PRIu64 " MiB of device-local memory\n", ballast_bytes >> 20);
	return true;
}

void GraphicContext::FreeBallast() {
	for (auto& [buffer, allocation]: ballast) {
		vmaDestroyBuffer(allocator, buffer, allocation);
	}
	ballast.clear();
	ballast_bytes = 0;
}

} // namespace Libs::Graphics
