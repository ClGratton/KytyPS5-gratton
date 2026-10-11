#include "graphics/host_gpu/renderer/pipeline/pipelineDump.h"

#include <cstdio>
#include <fmt/format.h>
#include <mutex>

namespace Libs::Graphics::PipelineDump {

namespace {

struct State {
	std::mutex            mutex;
	std::filesystem::path dir;
	uint64_t              gfx_seq = 0;
	uint64_t              cs_seq  = 0;
	uint64_t              gen_seq = 0;
};

State& GetState() {
	static State* state = new State; // leaked on purpose: used from quick_exit paths
	return *state;
}

void WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
	std::FILE* f = std::fopen(path.string().c_str(), "wb");
	if (f == nullptr) {
		std::fprintf(stderr, "pipeline dump: cannot write %s\n", path.string().c_str());
		return;
	}
	std::fwrite(data, 1, size, f);
	std::fclose(f);
}

void AppendLine(const std::filesystem::path& path, const std::string& line) {
	std::FILE* f = std::fopen(path.string().c_str(), "ab");
	if (f == nullptr) {
		return;
	}
	std::fputs(line.c_str(), f);
	std::fclose(f);
}

template <class E>
uint32_t U(E e) {
	return static_cast<uint32_t>(e);
}

uint32_t Flags(vk::ShaderStageFlags f) {
	return static_cast<uint32_t>(static_cast<VkShaderStageFlags>(f));
}

std::string BindingsJson(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	std::string s     = "[";
	bool        first = true;
	for (const auto& b: bindings) {
		s += fmt::format("{}{{\"binding\":{},\"type\":{},\"count\":{},\"stages\":{}}}",
		                 first ? "" : ",", b.binding, U(b.descriptorType), b.descriptorCount,
		                 Flags(b.stageFlags));
		first = false;
	}
	return s + "]";
}

} // namespace

void Configure(const std::filesystem::path& dir) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.dir = dir;
	if (dir.empty()) {
		detail::g_enabled.store(false, std::memory_order_relaxed);
		return;
	}
	std::filesystem::create_directories(dir / "spv");
	std::filesystem::create_directories(dir / "pipelines");
	std::filesystem::create_directories(dir / "compute");
	detail::g_enabled.store(true, std::memory_order_relaxed);
}

void WriteSpirv(uint64_t shader_id, std::span<const uint32_t> code, const char* stage_name,
                uint64_t shader_hash) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	WriteFile(state.dir / "spv" / fmt::format("{}.spv", shader_id), code.data(), code.size_bytes());
	AppendLine(state.dir / "shaders.jsonl",
	           fmt::format("{{\"shader_id\":{},\"stage\":\"{}\",\"hash\":\"{:016x}\",\"words\":{}}}\n",
	                       shader_id, stage_name, shader_hash, code.size()));
}

std::string WriteGeneratedSpirv(const char* tag, std::span<const uint32_t> code) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	const auto      name = fmt::format("spv/gen_{}_{}.spv", state.gen_seq++, tag);
	WriteFile(state.dir / name, code.data(), code.size_bytes());
	return name;
}

void WriteGraphicsPipeline(const GraphicsDump& d) {
	std::string j = "{";
	j += fmt::format("\"vs_id\":{},\"ps_id\":{},\"mesh\":{},\"rect_list\":{},\"tess\":{},", d.vs_id,
	                 d.ps_id, d.mesh, d.rect_list, d.tess);
	j += fmt::format("\"game_driver_ns\":{},\"game_total_ns\":{},", d.game_driver_ns,
	                 d.game_total_ns);
	j += "\"stages\":[";
	for (size_t i = 0; i < d.stages.size(); i++) {
		j += fmt::format("{}{{\"stage\":{},\"shader_id\":{},\"spv\":\"{}\"}}", i ? "," : "",
		                 d.stages[i].vk_stage, d.stages[i].shader_id, d.stages[i].spv);
	}
	j += "],";
	if (d.vertex_input != nullptr) {
		const auto& vi = *d.vertex_input;
		j += "\"vertex_input\":{\"bindings\":[";
		for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++) {
			const auto& b = vi.pVertexBindingDescriptions[i];
			j += fmt::format("{}{{\"binding\":{},\"stride\":{},\"rate\":{}}}", i ? "," : "",
			                 b.binding, b.stride, U(b.inputRate));
		}
		j += "],\"attributes\":[";
		for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++) {
			const auto& a = vi.pVertexAttributeDescriptions[i];
			j += fmt::format("{}{{\"location\":{},\"binding\":{},\"format\":{},\"offset\":{}}}",
			                 i ? "," : "", a.location, a.binding, U(a.format), a.offset);
		}
		j += "]},";
	}
	if (d.input_assembly != nullptr) {
		j += fmt::format("\"input_assembly\":{{\"topology\":{},\"restart\":{}}},",
		                 U(d.input_assembly->topology),
		                 static_cast<uint32_t>(d.input_assembly->primitiveRestartEnable));
	}
	if (d.tessellation != nullptr) {
		j += fmt::format("\"tess_patch_control_points\":{},", d.tessellation->patchControlPoints);
	}
	j += fmt::format("\"negative_one_to_one\":{},", d.negative_one_to_one);
	const auto& r = *d.raster;
	j += fmt::format(
	    "\"raster\":{{\"depth_clamp\":{},\"has_depth_clip_ext\":{},\"depth_clip\":{},"
	    "\"provoking\":{},\"cull\":{},\"front_face\":{},\"polygon_mode\":{},\"line_width\":{}}},",
	    static_cast<uint32_t>(r.depthClampEnable), d.has_depth_clip_ext, d.depth_clip_enable,
	    d.provoking_mode, static_cast<uint32_t>(static_cast<VkCullModeFlags>(r.cullMode)),
	    U(r.frontFace), U(r.polygonMode), r.lineWidth);
	const auto& m = *d.multisample;
	j += fmt::format(
	    "\"multisample\":{{\"samples\":{},\"sample_shading\":{},\"min_sample_shading\":{}}},",
	    U(m.rasterizationSamples), static_cast<uint32_t>(m.sampleShadingEnable),
	    m.minSampleShading);
	if (d.depth_stencil != nullptr) {
		j += fmt::format("\"depth_stencil\":{{\"bounds_test\":{},\"min\":{},\"max\":{}}},",
		                 static_cast<uint32_t>(d.depth_stencil->depthBoundsTestEnable),
		                 d.depth_stencil->minDepthBounds, d.depth_stencil->maxDepthBounds);
	}
	const auto& cb = *d.color_blend;
	j += fmt::format("\"blend\":{{\"color_write_ext\":{},\"attachments\":[", d.has_color_write_ext);
	for (uint32_t i = 0; i < cb.attachmentCount; i++) {
		const auto& a = cb.pAttachments[i];
		j += fmt::format(
		    "{}{{\"write_mask\":{},\"enable\":{},\"src_color\":{},\"dst_color\":{},"
		    "\"color_op\":{},\"src_alpha\":{},\"dst_alpha\":{},\"alpha_op\":{}}}",
		    i ? "," : "",
		    static_cast<uint32_t>(static_cast<VkColorComponentFlags>(a.colorWriteMask)),
		    static_cast<uint32_t>(a.blendEnable), U(a.srcColorBlendFactor),
		    U(a.dstColorBlendFactor), U(a.colorBlendOp), U(a.srcAlphaBlendFactor),
		    U(a.dstAlphaBlendFactor), U(a.alphaBlendOp));
	}
	j += "]},\"dynamic_states\":[";
	for (size_t i = 0; i < d.dynamic_states.size(); i++) {
		j += fmt::format("{}{}", i ? "," : "", U(d.dynamic_states[i]));
	}
	j += "],";
	const auto& ri = *d.rendering;
	j += "\"rendering\":{\"color_formats\":[";
	for (uint32_t i = 0; i < ri.colorAttachmentCount; i++) {
		j += fmt::format("{}{}", i ? "," : "", U(ri.pColorAttachmentFormats[i]));
	}
	j += fmt::format("],\"depth\":{},\"stencil\":{}}},", U(ri.depthAttachmentFormat),
	                 U(ri.stencilAttachmentFormat));
	j += fmt::format("\"layout\":{{\"push_descriptors\":{},\"bindings\":{},\"push_range\":{{\"stages\":{},"
	                 "\"offset\":{},\"size\":{}}}}}",
	                 d.push_descriptors, BindingsJson(d.layout_bindings),
	                 Flags(d.push_range.stageFlags), d.push_range.offset, d.push_range.size);
	j += "}\n";

	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	const auto      seq = state.gfx_seq++;
	// The sequence number is the first member so the file reads in capture order.
	j.insert(1, fmt::format("\"seq\":{},", seq));
	WriteFile(state.dir / "pipelines" / fmt::format("{:05}.json", seq), j.data(), j.size());
}

void WriteComputePipeline(const ComputeDump& d) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	const auto      seq = state.cs_seq++;
	const auto      j =
	    fmt::format("{{\"seq\":{},\"cs_id\":{},\"spv\":\"spv/{}.spv\",\"required_subgroup_size\":{},"
	                "\"game_driver_ns\":{},\"layout\":{{\"push_descriptors\":{},\"bindings\":{},"
	                "\"push_range\":{{\"stages\":32,\"offset\":0,\"size\":128}}}}}}\n",
	                seq, d.cs_id, d.cs_id, d.required_subgroup_size, d.game_driver_ns,
	                d.push_descriptors, BindingsJson(d.layout_bindings));
	WriteFile(state.dir / "compute" / fmt::format("{:05}.json", seq), j.data(), j.size());
}

} // namespace Libs::Graphics::PipelineDump
