#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

namespace HW {
class Context;
}

struct RenderColorInfo {
	// Discovery keeps guest image information but can remap the view into a larger cache image.
	TextureCache::ImageDesc         desc;
	ImageId                         image_id;
	uint32_t                        target_slot      = 0;
	uint32_t                        guest_mip_level   = 0;
	uint32_t                        guest_array_layer = 0;
	Prospero::ColorComponentMapping export_mapping;

	[[nodiscard]] vk::Extent2D Extent() const {
		return {std::max(desc.info.extent.width >> guest_mip_level, 1u),
		        std::max(desc.info.extent.height >> guest_mip_level, 1u)};
	}
};

// Everything ResolveRenderColorTarget computes from the registers alone (format, samples, export
// mapping, view); no image lookup. Returns false when the slot has no color output.
// Shared with the --pipeline-lookahead prefetch so both build the same pipeline key.
bool DescribeRenderColorTarget(const HW::Context& hw, RenderColorInfo& target,
                               uint32_t render_target_slice_offset, uint32_t render_target_slot,
                               bool ignore_target_mask = false);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
