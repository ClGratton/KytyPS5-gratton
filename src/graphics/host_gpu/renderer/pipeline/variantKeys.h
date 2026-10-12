#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTKEYS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTKEYS_H_

// Conversions between the translator's own key types and the persisted variant records
// (variantStore.h). Kept inline and free of Vulkan so the shader tests can check that a variant
// survives the round trip and keeps the same ProgramKey static state.

#include "graphics/host_gpu/renderer/pipeline/variantStore.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

namespace Libs::Graphics {

// The translator inputs of a compute dispatch that BuildStageStaticKey() reads, plus the ProgramKey
// user data count. Anything else in ShaderComputeInputInfo is runtime state (group counts,
// lds_storage) and is not part of a variant.
[[nodiscard]] inline Variants::ComputeStatic ToVariantStatic(const ShaderComputeInputInfo& info,
                                                             uint32_t user_data_count) {
	Variants::ComputeStatic st;
	st.user_data_count     = user_data_count;
	st.workgroup_register  = static_cast<uint32_t>(info.workgroup_register);
	st.wave_size           = info.wave_size;
	st.float_mode          = info.float_mode;
	st.host_subgroup_size  = info.host_subgroup_size;
	st.thread_ids_num      = static_cast<uint32_t>(info.thread_ids_num);
	st.lds_size_dwords     = info.lds_size_dwords;
	st.scratch_size_dwords = info.scratch_size_dwords;
	for (int i = 0; i < 3; i++) {
		st.threads_num[i] = info.threads_num[i];
	}
	st.flags = (info.dispatch_thread_dimensions ? 1u : 0u) | (info.async_compute ? 2u : 0u) |
	           (info.tg_size_en ? 4u : 0u) | (info.group_id[0] ? 8u : 0u) |
	           (info.group_id[1] ? 16u : 0u) | (info.group_id[2] ? 32u : 0u);
	return st;
}

inline void ApplyVariantStatic(const Variants::ComputeStatic& st, ShaderComputeInputInfo& info) {
	info                            = {};
	info.workgroup_register         = static_cast<int>(st.workgroup_register);
	info.wave_size                  = st.wave_size;
	info.float_mode                 = static_cast<uint8_t>(st.float_mode);
	info.host_subgroup_size         = st.host_subgroup_size;
	info.thread_ids_num             = static_cast<int>(st.thread_ids_num);
	info.lds_size_dwords            = st.lds_size_dwords;
	info.scratch_size_dwords        = st.scratch_size_dwords;
	for (int i = 0; i < 3; i++) {
		info.threads_num[i] = st.threads_num[i];
	}
	info.dispatch_thread_dimensions = (st.flags & 1u) != 0;
	info.async_compute              = (st.flags & 2u) != 0;
	info.tg_size_en                 = (st.flags & 4u) != 0;
	info.group_id[0]                = (st.flags & 8u) != 0;
	info.group_id[1]                = (st.flags & 16u) != 0;
	info.group_id[2]                = (st.flags & 32u) != 0;
}

[[nodiscard]] inline Variants::SpecRecord ToVariantSpec(const ShaderRecompiler::IR::ResourceSpecialization& spec) {
	Variants::SpecRecord record;
	record.buffers.reserve(spec.buffers.size());
	for (const auto& b: spec.buffers) {
		record.buffers.push_back({.packed_stride              = b.packed_stride,
		                          .descriptor_format          = static_cast<uint32_t>(b.descriptor_format),
		                          .descriptor_swizzle         = b.descriptor_swizzle,
		                          .zero_stride_oob            = b.zero_stride_oob ? 1u : 0u,
		                          .indirect_root              = b.indirect_root,
		                          .indirect_mapping_offset    = b.indirect_mapping_offset,
		                          .indirect_search_iterations = b.indirect_search_iterations});
	}
	record.images.reserve(spec.images.size());
	for (const auto& i: spec.images) {
		record.images.push_back({.numeric_class              = static_cast<uint32_t>(i.numeric_class),
		                         .dimension                  = static_cast<uint32_t>(i.dimension),
		                         .mip_count                  = i.mip_count,
		                         .conversion_format          = static_cast<uint32_t>(i.conversion_format),
		                         .shader_swizzle             = i.shader_swizzle,
		                         .indirect_root              = i.indirect_root,
		                         .indirect_mapping_offset    = i.indirect_mapping_offset,
		                         .indirect_search_iterations = i.indirect_search_iterations,
		                         .flags = (i.cube ? 1u : 0u) | (i.fmask ? 2u : 0u)});
	}
	return record;
}

[[nodiscard]] inline ShaderRecompiler::IR::ResourceSpecialization
FromVariantSpec(const Variants::SpecRecord& record) {
	using Spec = ShaderRecompiler::IR::ResourceSpecialization;
	Spec spec;
	spec.buffers.reserve(record.buffers.size());
	for (const auto& b: record.buffers) {
		Spec::Buffer buffer;
		buffer.packed_stride              = b.packed_stride;
		buffer.descriptor_format          = static_cast<Prospero::BufferFormat>(b.descriptor_format);
		buffer.descriptor_swizzle         = b.descriptor_swizzle;
		buffer.zero_stride_oob            = b.zero_stride_oob != 0;
		buffer.indirect_root              = b.indirect_root;
		buffer.indirect_mapping_offset    = b.indirect_mapping_offset;
		buffer.indirect_search_iterations = b.indirect_search_iterations;
		spec.buffers.push_back(buffer);
	}
	spec.images.reserve(record.images.size());
	for (const auto& i: record.images) {
		Spec::Image image;
		image.numeric_class              = static_cast<Prospero::TextureNumericClass>(i.numeric_class);
		image.dimension                  = static_cast<ShaderRecompiler::Decoder::ImageDimension>(i.dimension);
		image.mip_count                  = i.mip_count;
		image.conversion_format          = static_cast<Prospero::BufferFormat>(i.conversion_format);
		image.shader_swizzle             = i.shader_swizzle;
		image.indirect_root              = i.indirect_root;
		image.indirect_mapping_offset    = i.indirect_mapping_offset;
		image.indirect_search_iterations = i.indirect_search_iterations;
		image.cube                       = (i.flags & 1u) != 0;
		image.fmask                      = (i.flags & 2u) != 0;
		spec.images.push_back(image);
	}
	return spec;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_VARIANTKEYS_H_
