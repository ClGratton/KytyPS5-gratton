#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GRAPHICSVARIANTKEYS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GRAPHICSVARIANTKEYS_H_

// Canonical byte forms of the graphics pipeline key and of a stage's input info for the persisted
// variants (variantStore.h). The pipeline key is written field by field (the in-memory structs have
// padding); the input infos are plain data and are stored as raw bytes with their runtime pointers
// cleared, behind a layout fingerprint and a sanity check so that a file written by a build with
// another struct layout is ignored instead of misread.

#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

static_assert(std::is_trivially_copyable_v<ShaderVertexInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderPixelInputInfo>);

[[nodiscard]] constexpr uint32_t LayoutMix(uint32_t hash, uint64_t value) {
	hash ^= static_cast<uint32_t>(value) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
	return hash * 16777619u;
}

// Changes whenever the size or the position of the fields that matter changes.
[[nodiscard]] constexpr uint32_t VertexInfoLayout() {
	uint32_t h = 2166136261u;
	for (const auto v: {sizeof(ShaderVertexInputInfo), offsetof(ShaderVertexInputInfo, buffers),
	                    offsetof(ShaderVertexInputInfo, logical_stage),
	                    offsetof(ShaderVertexInputInfo, resources_num),
	                    offsetof(ShaderVertexInputInfo, buffers_num),
	                    offsetof(ShaderVertexInputInfo, wave_size),
	                    offsetof(ShaderVertexInputInfo, clip_space), offsetof(ShaderVertexInputInfo, mesh),
	                    offsetof(ShaderVertexInputInfo, tess)}) {
		h = LayoutMix(h, v);
	}
	return h;
}

[[nodiscard]] constexpr uint32_t PixelInfoLayout() {
	uint32_t h = 2166136261u;
	for (const auto v: {sizeof(ShaderPixelInputInfo), offsetof(ShaderPixelInputInfo, input_num),
	                    offsetof(ShaderPixelInputInfo, target_output_mode),
	                    offsetof(ShaderPixelInputInfo, target_export_mapping),
	                    offsetof(ShaderPixelInputInfo, parameter_mode),
	                    offsetof(ShaderPixelInputInfo, dual_source_blending),
	                    offsetof(ShaderPixelInputInfo, ps_early_z)}) {
		h = LayoutMix(h, v);
	}
	return h;
}

[[nodiscard]] inline std::vector<uint8_t> EncodeVertexInfo(ShaderVertexInputInfo info) {
	info.stage = {};
	std::vector<uint8_t> bytes(sizeof(info));
	std::memcpy(bytes.data(), &info, sizeof(info));
	return bytes;
}

[[nodiscard]] inline std::vector<uint8_t> EncodePixelInfo(ShaderPixelInputInfo info) {
	info.stage = {};
	std::vector<uint8_t> bytes(sizeof(info));
	std::memcpy(bytes.data(), &info, sizeof(info));
	return bytes;
}

// False when the bytes are not an input info of this build (size) or hold impossible values.
[[nodiscard]] inline bool DecodeVertexInfo(const std::vector<uint8_t>& bytes, uint32_t layout,
                                           ShaderVertexInputInfo& info) {
	if (layout != VertexInfoLayout() || bytes.size() != sizeof(ShaderVertexInputInfo)) return false;
	std::memcpy(&info, bytes.data(), sizeof(info));
	info.stage = {};
	return info.resources_num >= 0 && info.resources_num <= ShaderVertexInputInfo::RES_MAX &&
	       info.buffers_num >= 0 && info.buffers_num <= ShaderVertexInputInfo::RES_MAX &&
	       (info.logical_stage == ShaderType::Vertex || info.logical_stage == ShaderType::Mesh);
}

[[nodiscard]] inline bool DecodePixelInfo(const std::vector<uint8_t>& bytes, uint32_t layout,
                                          ShaderPixelInputInfo& info) {
	if (layout != PixelInfoLayout() || bytes.size() != sizeof(ShaderPixelInputInfo)) return false;
	std::memcpy(&info, bytes.data(), sizeof(info));
	info.stage = {};
	return info.input_num <= 32;
}

namespace detail {
inline void Put32(std::vector<uint8_t>& out, uint32_t v) {
	for (uint32_t i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8u * i)));
}
inline bool Get32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& v) {
	if (pos + 4 > in.size()) return false;
	v = 0;
	for (uint32_t i = 0; i < 4; i++) v |= static_cast<uint32_t>(in[pos + i]) << (8u * i);
	pos += 4;
	return true;
}
} // namespace detail

// The pipeline key without the process-local shader ids.
[[nodiscard]] inline std::vector<uint8_t>
EncodePipelineKey(const PipelineRenderingState& rendering, const PipelineVertexInputState& vertex_input,
                  const PipelineStaticParameters& params) {
	std::vector<uint8_t> out;
	for (const auto format: rendering.color_formats) detail::Put32(out, static_cast<uint32_t>(format));
	detail::Put32(out, static_cast<uint32_t>(rendering.depth_format));
	detail::Put32(out, static_cast<uint32_t>(rendering.stencil_format));
	detail::Put32(out, rendering.color_count);
	detail::Put32(out, vertex_input.binding_count);
	detail::Put32(out, vertex_input.attribute_count);
	for (const auto& b: vertex_input.bindings) {
		detail::Put32(out, b.stride);
		detail::Put32(out, b.instance ? 1u : 0u);
	}
	for (const auto& a: vertex_input.attributes) {
		detail::Put32(out, a.offset);
		detail::Put32(out, a.binding);
	}
	static_assert(sizeof(PipelineStaticParameters) == 116);
	const auto* raw = reinterpret_cast<const uint8_t*>(&params); // packed: no padding
	out.insert(out.end(), raw, raw + sizeof(params));
	return out;
}

[[nodiscard]] inline bool DecodePipelineKey(const std::vector<uint8_t>& in, PipelineRenderingState& rendering,
                                            PipelineVertexInputState& vertex_input,
                                            PipelineStaticParameters& params) {
	size_t   pos = 0;
	uint32_t v   = 0;
	PipelineRenderingState   r;
	PipelineVertexInputState vi;
	for (auto& format: r.color_formats) {
		if (!detail::Get32(in, pos, v)) return false;
		format = static_cast<vk::Format>(v);
	}
	if (!detail::Get32(in, pos, v)) return false;
	r.depth_format = static_cast<vk::Format>(v);
	if (!detail::Get32(in, pos, v)) return false;
	r.stencil_format = static_cast<vk::Format>(v);
	if (!detail::Get32(in, pos, r.color_count) || r.color_count > RENDER_COLOR_ATTACHMENTS_MAX) return false;
	if (!detail::Get32(in, pos, v) || v > ShaderVertexInputInfo::RES_MAX) return false;
	vi.binding_count = static_cast<uint8_t>(v);
	if (!detail::Get32(in, pos, v) || v > ShaderVertexInputInfo::RES_MAX) return false;
	vi.attribute_count = static_cast<uint8_t>(v);
	for (auto& b: vi.bindings) {
		if (!detail::Get32(in, pos, b.stride) || !detail::Get32(in, pos, v) || v > 1u) return false;
		b.instance = v != 0;
	}
	for (auto& a: vi.attributes) {
		if (!detail::Get32(in, pos, a.offset) || !detail::Get32(in, pos, v) || v > 255u) return false;
		a.binding = static_cast<uint8_t>(v);
	}
	if (in.size() - pos != sizeof(PipelineStaticParameters)) return false;
	PipelineStaticParameters p;
	std::memcpy(&p, in.data() + pos, sizeof(p));
	const auto samples = p.samples;
	if (static_cast<uint32_t>(p.topology) > static_cast<uint32_t>(vk::PrimitiveTopology::ePatchList) ||
	    static_cast<uint32_t>(p.polygon_mode) > static_cast<uint32_t>(vk::PolygonMode::ePoint) ||
	    samples == 0 || samples > 64 || (samples & (samples - 1)) != 0) {
		return false;
	}
	params       = p;
	rendering    = r;
	vertex_input = vi;
	return true;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_GRAPHICSVARIANTKEYS_H_
