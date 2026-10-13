#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export
#include "graphics/shader/recompiler/RobustBufferPolicy.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <vk_mem_alloc.h>

// VRAM diagnostics are always printed (LOGF is silent unless the log is enabled).
#define VRAM_LOG(...) ::Log::WriteToConsoleAndLog(::fmt::sprintf(__VA_ARGS__))

namespace Libs::Graphics {

struct VulkanImage;

// VRAM accounting classes (--vram-stats). Image classes are a heuristic: the usage flags do
// not distinguish render targets from textures (every uncompressed color image is allowed as
// attachment and storage), so only block-compressed images count as textures.
enum class VramClass : uint8_t {
	ImageTexture,
	ImageRenderTarget,
	ImageDepth,
	BufferDeviceLocal,
	BufferBda,
	BufferUpload,
	BufferDownload,
	BufferStream,
	Count,
};

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

struct GraphicContext {
	vk::Instance                       instance                              = nullptr;
	vk::DebugUtilsMessengerEXT         debug_messenger                       = nullptr;
	vk::PhysicalDevice                 physical_device                       = nullptr;
	vk::PhysicalDeviceProperties       physical_device_properties            = {};
	vk::PhysicalDeviceMemoryProperties physical_device_memory_properties     = {};
	vk::Device                         device                                = nullptr;
	VmaAllocator                       allocator                             = nullptr;
	bool                               memory_budget_ext_enabled             = false;
	bool                               memory_priority_enabled               = false;
	bool                               pageable_memory_enabled               = false;
	// Host GPU timestamps for the guest GPU clock (--gpu-timestamps gpu).
	bool                               calibrated_timestamps_enabled         = false;
	bool                               host_query_reset_enabled              = false;
	bool                               compute_subgroup_size_control_enabled = false;
	bool                               sample_rate_shading_enabled           = false;
	// firstInstance may be non-zero in indirect draw commands (--gpu-indirect-draws, plain draws).
	bool                               draw_indirect_first_instance_enabled  = false;
	bool                               shader_image_int64_atomics_enabled    = false;
	// bool fp64_denorm_preserve = false; // Temporarily disabled.
	bool                               attachment_feedback_loop_enabled      = false;
	bool                               provoking_vertex_last_enabled         = false;
	bool                               depth_color_copy_enabled               = false;
	// pipelineCreationCacheControl: FAIL_ON_PIPELINE_COMPILE_REQUIRED probes of a pipeline cache
	// (--pipelines-unoptimized-first).
	bool                               pipeline_cache_control_enabled         = false;
	bool                               supports_block_texel_view              = false;
	// VK_EXT_robustness2: robustBufferAccess2 enabled on the device, and the granularity the driver
	// checks storage-buffer accesses at (robustStorageBufferAccessSizeAlignment, 0 = unknown).
	bool                               robust_buffer_access2_enabled          = false;
	uint32_t                           robust_storage_buffer_alignment        = 0;
	// --debug-gpu-ops checkpoints: VK_NV_device_diagnostic_checkpoints is enabled on the device.
	bool                               op_checkpoints_enabled                = false;
	bool                                      mesh_shader_enabled                   = false;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	vk::ShaderStageFlags               required_subgroup_size_stages         = {};
	Common::Mutex                      queue_mutex;
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	vk::Queue                          queue        = nullptr;
	uint32_t                           queue_count  = 1; // queues created in queue_family
	// --readback-queue: a second queue of the same family for buffer readbacks (nullptr when the
	// family has one queue or the flag is off).
	Common::Mutex                      readback_queue_mutex;
	vk::Queue                          readback_queue = nullptr;

	[[nodiscard]] const vk::PhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] const vk::PhysicalDeviceMemoryProperties&
	GetPhysicalDeviceMemoryProperties() const {
		return physical_device_memory_properties;
	}

	[[nodiscard]] vk::FormatProperties GetFormatProperties(vk::Format format) const {
		std::scoped_lock lock(m_format_properties_mutex);
		auto [it, inserted] = m_format_properties.try_emplace(format);
		if (inserted) {
			physical_device.getFormatProperties(format, &it->second);
		}
		return it->second;
	}

	[[nodiscard]] vk::Result GetImageFormatProperties(vk::Format format, vk::ImageType type,
	                                                  vk::ImageTiling            tiling,
	                                                  vk::ImageUsageFlags        usage,
	                                                  vk::ImageCreateFlags       flags,
	                                                  vk::ImageFormatProperties* properties) const {
		using Key = std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
		                       vk::ImageCreateFlags>;
		std::scoped_lock lock(m_image_format_properties_mutex);
		auto [it, inserted] =
		    m_image_format_properties.try_emplace(Key {format, type, tiling, usage, flags});
		if (inserted) {
			it->second.first = physical_device.getImageFormatProperties(format, type, tiling, usage,
			                                                            flags, &it->second.second);
		}
		if (properties != nullptr) {
			*properties = it->second.second;
		}
		return it->second.first;
	}

	[[nodiscard]] bool SupportsComputeWave64() const noexcept {
		return subgroup_size == 64u || compute_subgroup_size_control_enabled;
	}

	// Which storage-buffer accesses the SPIR-V emitter may leave to the device (--spirv-robust-bounds).
	[[nodiscard]] ShaderRecompiler::RobustBufferPolicy SpirvRobustBufferPolicy() const {
		return ShaderRecompiler::MakeRobustBufferPolicy(
		    Config::SpirvRobustBoundsRequested(), robust_buffer_access2_enabled,
		    robust_storage_buffer_alignment, Config::SpirvRobustStoresRequested());
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	void               LogMemoryBudgetPeriodic() const;
	void               AccountVram(VramClass vram_class, int64_t delta) noexcept;
	// Debug: hold `mebibytes` of device-local memory (--vram-ballast-mb) to simulate a smaller GPU.
	[[nodiscard]] bool AllocateBallast(uint64_t mebibytes);
	void               FreeBallast();
	// Called once when an image allocation fails: should free memory (set by the texture cache).
	std::function<void()> reclaim_memory;
	bool                  in_reclaim = false;
	uint64_t              ballast_bytes = 0;
	// Diagnostics (--vram-stats): driver objects that live in VRAM outside VMA.
	std::atomic<uint32_t> dedicated_images {0};
	std::atomic<uint32_t> host_fallback_images {0};
	std::atomic<uint32_t> pipelines_graphics {0};
	std::atomic<uint32_t> pipelines_compute {0};
	std::array<std::atomic<int64_t>, static_cast<size_t>(VramClass::Count)> vram_bytes {};
	std::array<std::atomic<int64_t>, static_cast<size_t>(VramClass::Count)> vram_count {};
	std::vector<std::pair<VkBuffer, VmaAllocation>> ballast;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image);
	void                   DeleteImage(VulkanImage& image);

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
	mutable std::mutex                                 m_format_properties_mutex;
	mutable std::map<vk::Format, vk::FormatProperties> m_format_properties;
	mutable std::mutex                                 m_image_format_properties_mutex;
	mutable std::map<std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
	                            vk::ImageCreateFlags>,
	                 std::pair<vk::Result, vk::ImageFormatProperties>>
	    m_image_format_properties;
};

struct VulkanImageState {
	vk::PipelineStageFlags2 pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	vk::AccessFlags2        access_mask = vk::AccessFlagBits2::eNone;
	vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
};

struct VulkanImage {
	VulkanImage() = default;
	KYTY_CLASS_NO_COPY(VulkanImage);

	vk::Format                    format      = vk::Format::eUndefined;
	vk::ImageType                 image_type  = vk::ImageType::e2D;
	vk::Extent3D                  extent      = {1, 1, 1};
	uint32_t                      layers      = 1;
	uint32_t                      mip_levels  = 1;
	uint32_t                      samples     = 1;
	vk::ImageUsageFlags           usage       = {};
	vk::ImageCreateFlags          flags       = {};
	vk::Image                     image       = nullptr;
	VulkanImageState              state;
	std::vector<VulkanImageState> subresource_states;
	VmaAllocation                allocation = nullptr;
	uint64_t                     alloc_size = 0;
	VramClass                    vram_class = VramClass::ImageTexture;
	bool                         dedicated  = false;
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
