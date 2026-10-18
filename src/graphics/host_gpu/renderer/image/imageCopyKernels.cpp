#include "graphics/host_gpu/renderer/image/imageCopyKernels.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/gpuPhaseStats.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_down16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_down4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_down8_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_up16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_up4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_up8_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageView.h"

#include <algorithm>
#include <limits>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint32_t GroupWidth  = 16;
constexpr uint32_t GroupHeight = 8;

[[nodiscard]] uint32_t PipelineIndex(bool upload, uint32_t texel_words) {
	return (upload ? 0u : 3u) + (texel_words == 1 ? 0u : (texel_words == 2 ? 1u : 2u));
}

} // namespace

ImageCopyKernels::ImageCopyKernels(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	static_assert(sizeof(Region) == 36);
	std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
	bindings[0] = {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute,
	               nullptr};
	bindings[1] = {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute,
	               nullptr};
	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create ImageCopyKernels descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Region)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &m_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create ImageCopyKernels pipeline layout");
}

ImageCopyKernels::~ImageCopyKernels() {
	for (auto pipeline: m_pipelines) {
		if (pipeline != nullptr) {
			m_graphics.device.destroyPipeline(pipeline, nullptr);
		}
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

vk::Pipeline ImageCopyKernels::GetPipeline(bool upload, uint32_t texel_words) {
	const auto index = PipelineIndex(upload, texel_words);
	if (m_pipelines[index] != nullptr) {
		return m_pipelines[index];
	}
	struct Shader {
		const uint32_t* code;
		size_t          words;
	};
	static constexpr std::array<Shader, 6> shaders {{
	    {GPU_TILER_COPY_UP4_SPV, std::size(GPU_TILER_COPY_UP4_SPV)},
	    {GPU_TILER_COPY_UP8_SPV, std::size(GPU_TILER_COPY_UP8_SPV)},
	    {GPU_TILER_COPY_UP16_SPV, std::size(GPU_TILER_COPY_UP16_SPV)},
	    {GPU_TILER_COPY_DOWN4_SPV, std::size(GPU_TILER_COPY_DOWN4_SPV)},
	    {GPU_TILER_COPY_DOWN8_SPV, std::size(GPU_TILER_COPY_DOWN8_SPV)},
	    {GPU_TILER_COPY_DOWN16_SPV, std::size(GPU_TILER_COPY_DOWN16_SPV)},
	}};
	const auto module = CompileSPV({shaders[index].code, shaders[index].words}, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo create {};
	create.stage      = stage;
	create.layout     = m_pipeline_layout;
	const auto result = m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr,
	                                                             &m_pipelines[index]);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create ImageCopyKernels pipeline");
	return m_pipelines[index];
}

bool ImageCopyKernels::MakePlan(const Image& image, std::span<const vk::BufferImageCopy> copies,
                                uint64_t offset, uint64_t size, Plan& plan) const {
	const auto& backing = image.backing;
	if (copies.empty() || backing.image == nullptr || backing.samples != 1 ||
	    backing.image_type != vk::ImageType::e2D || backing.layers == 0 ||
	    !(backing.usage & vk::ImageUsageFlagBits::eStorage) ||
	    (backing.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) ||
	    !(backing.flags & vk::ImageCreateFlagBits::eMutableFormat) ||
	    vk::planeCount(backing.format) != 1 || vk::texelsPerBlock(backing.format) != 1) {
		return false;
	}
	switch (vk::blockSize(backing.format)) {
		case 4:
			plan.texel_words = 1;
			plan.view_format = vk::Format::eR32Uint;
			break;
		case 8:
			plan.texel_words = 2;
			plan.view_format = vk::Format::eR16G16B16A16Uint;
			break;
		case 16:
			plan.texel_words = 4;
			plan.view_format = vk::Format::eR32G32B32A32Uint;
			break;
		default: return false;
	}
	if (!ImageViewOps::FormatsCompatible(backing.format, plan.view_format)) {
		return false;
	}
	const uint64_t texel_bytes = uint64_t {plan.texel_words} * 4;
	uint64_t       lo          = std::numeric_limits<uint64_t>::max();
	uint64_t       hi          = 0;
	uint64_t       texels      = 0;
	for (const auto& copy: copies) {
		const auto& subresource = copy.imageSubresource;
		const auto& extent      = copy.imageExtent;
		if (subresource.aspectMask != vk::ImageAspectFlagBits::eColor || copy.imageOffset.z != 0 ||
		    extent.depth != 1 || extent.width == 0 || extent.height == 0 ||
		    subresource.layerCount == 0 || subresource.layerCount == VK_REMAINING_ARRAY_LAYERS ||
		    subresource.mipLevel >= backing.mip_levels ||
		    uint64_t {subresource.baseArrayLayer} + subresource.layerCount > backing.layers ||
		    copy.bufferOffset % 4 != 0 || copy.imageOffset.x < 0 || copy.imageOffset.y < 0) {
			return false;
		}
		const uint64_t level_width  = std::max<uint32_t>(backing.extent.width >> subresource.mipLevel, 1);
		const uint64_t level_height = std::max<uint32_t>(backing.extent.height >> subresource.mipLevel, 1);
		if (uint64_t {static_cast<uint32_t>(copy.imageOffset.x)} + extent.width > level_width ||
		    uint64_t {static_cast<uint32_t>(copy.imageOffset.y)} + extent.height > level_height) {
			return false;
		}
		const uint64_t row_length   = copy.bufferRowLength != 0 ? copy.bufferRowLength : extent.width;
		const uint64_t image_height = copy.bufferImageHeight != 0 ? copy.bufferImageHeight : extent.height;
		if (row_length < extent.width || image_height < extent.height ||
		    row_length * image_height > std::numeric_limits<uint32_t>::max() / 4 / plan.texel_words) {
			return false;
		}
		const uint64_t last_texel = (uint64_t {subresource.layerCount} - 1) * row_length * image_height +
		                            (uint64_t {extent.height} - 1) * row_length + extent.width;
		lo = std::min<uint64_t>(lo, copy.bufferOffset);
		hi = std::max<uint64_t>(hi, copy.bufferOffset + last_texel * texel_bytes);
		texels += uint64_t {extent.width} * extent.height * subresource.layerCount;
	}
	if (texels < MinTexels || lo >= hi || lo < offset || hi - offset > size) {
		return false;
	}
	const auto&    limits    = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	plan.range_offset        = Common::AlignDown(lo, alignment);
	plan.range_size          = hi - plan.range_offset;
	return plan.range_size <= limits.maxStorageBufferRange &&
	       plan.range_size <= std::numeric_limits<uint32_t>::max();
}

bool ImageCopyKernels::Upload(Image& image, std::span<const vk::BufferImageCopy> copies,
                              vk::Buffer buffer, uint64_t offset, uint64_t size) {
	Plan plan;
	if (buffer == nullptr || !MakePlan(image, copies, offset, size, plan)) {
		return false;
	}
	Record(image, copies, buffer, plan, true);
	return true;
}

bool ImageCopyKernels::Download(Image& image, std::span<const vk::BufferImageCopy> copies,
                                vk::Buffer buffer, uint64_t offset, uint64_t size) {
	Plan plan;
	if (buffer == nullptr || !MakePlan(image, copies, offset, size, plan)) {
		return false;
	}
	Record(image, copies, buffer, plan, false);
	return true;
}

void ImageCopyKernels::Record(Image& image, std::span<const vk::BufferImageCopy> copies,
                              vk::Buffer buffer, const Plan& plan, bool upload) {
	Common::GpuPhase::Scope gpu_phase_vk(Common::GpuPhase::Phase::VkCmd);
	m_scheduler.EndRendering();
	// The buffer range the regions touch: [plan.range_offset, +plan.range_size) is what the descriptor binds, and the barriers cover
	// the same bytes.
	vk::BufferMemoryBarrier2 buffer_barrier {};
	buffer_barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.srcAccessMask =
	    upload ? vk::AccessFlagBits2::eMemoryWrite
	           : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	buffer_barrier.dstAccessMask = upload ? vk::AccessFlagBits2::eShaderRead
	                                      : vk::AccessFlagBits2::eShaderRead |
	                                            vk::AccessFlagBits2::eShaderWrite;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer              = buffer;
	buffer_barrier.offset              = plan.range_offset;
	buffer_barrier.size                = plan.range_size;
	const auto image_barriers =
	    image.GetBarriers(vk::ImageLayout::eGeneral,
	                      upload ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eShaderRead,
	                      vk::PipelineStageFlagBits2::eComputeShader, {});
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &buffer_barrier;
	dependency.imageMemoryBarrierCount  = static_cast<uint32_t>(image_barriers.size());
	dependency.pImageMemoryBarriers     = image_barriers.data();
	auto command                        = m_scheduler.Current().Handle();
	command.pipelineBarrier2(dependency);

	command.bindPipeline(vk::PipelineBindPoint::eCompute, GetPipeline(upload, plan.texel_words));
	const vk::DescriptorBufferInfo buffer_info {buffer, plan.range_offset, plan.range_size};
	uint32_t                       bound_level = std::numeric_limits<uint32_t>::max();
	for (const auto& copy: copies) {
		const auto& subresource = copy.imageSubresource;
		if (subresource.mipLevel != bound_level) {
			bound_level = subresource.mipLevel;
			ImageViewInfo view_info {};
			view_info.format      = plan.view_format;
			view_info.type        = vk::ImageViewType::e2DArray;
			view_info.aspect      = vk::ImageAspectFlagBits::eColor;
			view_info.base_level  = subresource.mipLevel;
			view_info.level_count = 1;
			view_info.base_layer  = 0;
			view_info.layer_count = image.backing.layers;
			view_info.usage       = vk::ImageUsageFlagBits::eStorage;
			const vk::DescriptorImageInfo image_info {nullptr, image.FindView(view_info),
			                                          vk::ImageLayout::eGeneral};
			std::array<vk::WriteDescriptorSet, 2> writes {};
			writes[0].dstBinding      = 0;
			writes[0].descriptorCount = 1;
			writes[0].descriptorType  = vk::DescriptorType::eStorageBuffer;
			writes[0].pBufferInfo     = &buffer_info;
			writes[1].dstBinding      = 1;
			writes[1].descriptorCount = 1;
			writes[1].descriptorType  = vk::DescriptorType::eStorageImage;
			writes[1].pImageInfo      = &image_info;
			command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
			                             static_cast<uint32_t>(writes.size()), writes.data());
		}
		const uint32_t row_length   = copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
		const uint32_t image_height = copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
		const Region   region {
		      .buffer_word = static_cast<uint32_t>((copy.bufferOffset - plan.range_offset) / 4),
		      .row_words   = row_length * plan.texel_words,
		      .slice_words = row_length * image_height * plan.texel_words,
		      .width       = copy.imageExtent.width,
		      .height      = copy.imageExtent.height,
		      .layers      = subresource.layerCount,
		      .x           = copy.imageOffset.x,
		      .y           = copy.imageOffset.y,
		      .layer       = static_cast<int32_t>(subresource.baseArrayLayer),
        };
		command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(region),
		                      &region);
		command.dispatch((region.width + GroupWidth - 1) / GroupWidth,
		                 (region.height + GroupHeight - 1) / GroupHeight, region.layers);
	}

	buffer_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	buffer_barrier.srcAccessMask = upload ? vk::AccessFlagBits2::eShaderRead
	                                      : vk::AccessFlagBits2::eShaderRead |
	                                            vk::AccessFlagBits2::eShaderWrite;
	buffer_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	buffer_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	dependency.imageMemoryBarrierCount = 0;
	dependency.pImageMemoryBarriers    = nullptr;
	command.pipelineBarrier2(dependency);
	if (upload) {
		// The same final state as Image::Upload: the image is readable by every later use.
		image.Transit(vk::ImageLayout::eGeneral,
		              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {},
		              command);
	}
}

} // namespace Libs::Graphics
