#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SHADOW_TYPES_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SHADOW_TYPES_H

#include <cstdint>

namespace Libs::Graphics {

// What a shadow CommandProcessor (--pipeline-lookahead) reports for a draw or a dispatch.
struct ShadowDraw {
	bool     indexed             = false;
	bool     indirect            = false; // arguments live in guest memory and are not read
	uint32_t index_count         = 0;     // vertex count for non-indexed draws
	uint32_t instance_count      = 0;
	uint32_t index_type_and_size = 0;
};

struct ShadowDispatch {
	uint32_t x        = 0;
	uint32_t y        = 0;
	uint32_t z        = 0;
	uint32_t mode     = 0;
	bool     indirect = false;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SHADOW_TYPES_H
