#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// What an image binding was resolved from, so that the next draw that binds the same descriptor
	// in the same slot can keep it (--texture-binding-reuse, TextureCache::RefindImage).
	struct ImageSource {
		ShaderRecompiler::IR::ImageResource   resource;
		// The shader's own resource record, when it was recorded from one: a compiled program is never
		// freed or changed, so the same address is the same resource and needs no field-by-field compare.
		const ShaderRecompiler::IR::ImageResource* resource_address = nullptr;
		ShaderRecompiler::IR::DescriptorValue value;
		ImageId                               found;              // FindImage's result
		uint64_t                              generation = 0;     // 0: not reusable
		uint32_t                              metadata_base_layer = 0;
		// --incremental-draw (incrementalDraw.h): the slot's binding (image, view, desc) was left complete by a full rebind as a
		// plain sampled colour texture, and the next draw that finds the same inputs replays it without a lookup. `replayed`
		// marks the draw that did, until its rebind step has run; `stamp` says which LRU touch and access-tick store are done.
		bool                                  kept     = false;
		bool                                  replayed = false;
		TextureCache::KeptStamp               stamp;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	std::vector<TextureBinding>           images;
	std::vector<ImageSource>              image_sources; // parallel to `images`
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              loop_guard {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

// Prints the number of fallback bindings (target-flagged image without an attachment) at exit.
// Idempotent; called once from the emulator's run path, after the logging shutdown hooks.
void RegisterTargetFallbackSummary();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
