#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_

#include "graphics/shader/shader.h"

#include <span>
#include <string>

namespace Libs::Graphics {

struct ShaderVertexMetadata {
	int                             vertex_buffer_reg = -1;
	int                             vertex_attrib_reg = -1;
	std::span<const ShaderSemantic> input_semantics;
};

// Installed by the renderer (--clean-guest-reads): copies guest bytes from the backing when none
// of them is GPU-written, returning false (and reading nothing) otherwise. GPU thread only.
using ShaderCleanGuestReader = bool (*)(void* context, uint64_t vaddr, void* data, uint64_t size);
void ShaderSetCleanGuestReader(ShaderCleanGuestReader reader, void* context);
// The clean path alone: false when no reader is installed or the bytes cannot be proven current.
bool ShaderTryReadGuestClean(void* dst, const void* src, size_t size);
// memcpy from guest memory that, when the clean reader proves the bytes current, avoids the page
// fault (and the GPU drain) a plain load takes on a page shared with GPU-written data.
void ShaderReadGuest(void* dst, const void* src, size_t size);

// Validates the vertex metadata and returns a view into the shader header.
bool ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                              ShaderVertexMetadata& metadata, std::string* error);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_ */
