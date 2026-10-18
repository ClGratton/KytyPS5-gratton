#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGECOPYKERNELS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGECOPYKERNELS_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <span>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

// --compute-image-copy: linear buffer <-> image copies as compute dispatches. vkCmdCopyBufferToImage / vkCmdCopyImageToBuffer move
// 25-65 GB/s on the NVIDIA driver for the full-screen render targets the texture cache round-trips through guest memory; a kernel
// that moves the same words through an unsigned integer view of the image runs at the memory bandwidth, and it leaves the image in
// GENERAL, which is where Upload ends anyway.
//
// Exact: the image is seen as R32_UINT / R16G16B16A16_UINT / R32G32B32A32_UINT (same format class as the texel size, so a view of
// the image's own format), the words are moved unchanged, and the buffer and image barriers cover the same accesses as the
// transfer path. Anything the kernel does not model (depth and stencil, block-compressed, volume, multisampled, 1 and 2 byte
// texels, copies too small to matter) is left to the caller's vkCmd copy: Upload and Download return false and record nothing.
class ImageCopyKernels final {
public:
	ImageCopyKernels(GraphicContext& graphics, CommandScheduler& scheduler);
	~ImageCopyKernels();
	KYTY_CLASS_NO_COPY(ImageCopyKernels);

	// Copies smaller than this many texels in total stay on the transfer path (a dispatch and its push descriptor cost about as much
	// as the small copy itself).
	static constexpr uint64_t MinTexels = 16384;

	[[nodiscard]] bool Upload(Image& image, std::span<const vk::BufferImageCopy> copies,
	                          vk::Buffer buffer, uint64_t offset, uint64_t size);
	[[nodiscard]] bool Download(Image& image, std::span<const vk::BufferImageCopy> copies,
	                            vk::Buffer buffer, uint64_t offset, uint64_t size);

private:
	struct Plan {
		uint32_t   texel_words = 0; // 1, 2 or 4
		vk::Format view_format = vk::Format::eUndefined;
		uint64_t   range_offset = 0; // storage buffer descriptor offset (aligned down)
		uint64_t   range_size   = 0;
	};
	struct Region {
		uint32_t buffer_word;
		uint32_t row_words;
		uint32_t slice_words;
		uint32_t width;
		uint32_t height;
		uint32_t layers;
		int32_t  x;
		int32_t  y;
		int32_t  layer;
	};

	[[nodiscard]] bool        MakePlan(const Image& image, std::span<const vk::BufferImageCopy> copies,
	                                   uint64_t offset, uint64_t size, Plan& plan) const;
	[[nodiscard]] vk::Pipeline GetPipeline(bool upload, uint32_t texel_words);
	void Record(Image& image, std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer,
	            const Plan& plan, bool upload);

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	std::array<vk::Pipeline, 6> m_pipelines {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGECOPYKERNELS_H_
