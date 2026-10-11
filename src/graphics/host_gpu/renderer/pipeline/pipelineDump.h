#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEDUMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEDUMP_H_

// D2 spike (shaders.md Decision 2, Step 1): --dump-pipelines <dir>.
//
// Debug-only, default off. On every graphics (and compute) pipeline miss it writes
//   <dir>/spv/<shader_id>.spv         SPIR-V of every translated program (written at translation)
//   <dir>/spv/gen_<n>_{tcs,tes}.spv   generated rect-list tessellation shaders
//   <dir>/pipelines/<seq>.json        everything CreatePipelineInternal feeds to
//                                     vkCreateGraphicsPipelines + the descriptor layout bindings
//   <dir>/compute/<seq>.json          compute pipeline (SPIR-V name, layout bindings, subgroup size)
// The standalone tools/gpl-bench program replays these files. Nothing here runs unless the flag
// is given (one relaxed atomic load at the call sites).

#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::PipelineDump {

namespace detail {
inline std::atomic<bool> g_enabled {false};
} // namespace detail

[[nodiscard]] inline bool Enabled() noexcept {
	return detail::g_enabled.load(std::memory_order_relaxed);
}

// Called once from Config::Load(). An empty path disables the dump.
void Configure(const std::filesystem::path& dir);

void WriteSpirv(uint64_t shader_id, std::span<const uint32_t> code, const char* stage_name,
                uint64_t shader_hash);
// Returns the file name relative to the dump dir.
std::string WriteGeneratedSpirv(const char* tag, std::span<const uint32_t> code);

struct StageDump {
	uint32_t    vk_stage  = 0;
	uint64_t    shader_id = 0; // 0 for generated (rect-list) modules
	std::string spv;           // relative to the dump dir
};

struct GraphicsDump {
	std::vector<StageDump>                          stages;
	const vk::PipelineVertexInputStateCreateInfo*   vertex_input    = nullptr; // null for mesh
	const vk::PipelineInputAssemblyStateCreateInfo* input_assembly  = nullptr;
	const vk::PipelineTessellationStateCreateInfo*  tessellation    = nullptr;
	bool                                            negative_one_to_one = false;
	const vk::PipelineRasterizationStateCreateInfo* raster          = nullptr;
	bool                                            has_depth_clip_ext = false;
	bool                                            depth_clip_enable  = false;
	int                                             provoking_mode     = -1; // -1 none
	const vk::PipelineMultisampleStateCreateInfo*   multisample     = nullptr;
	const vk::PipelineDepthStencilStateCreateInfo*  depth_stencil   = nullptr;
	const vk::PipelineColorBlendStateCreateInfo*    color_blend     = nullptr;
	bool                                            has_color_write_ext = false;
	std::span<const vk::DynamicState>               dynamic_states;
	const vk::PipelineRenderingCreateInfo*          rendering       = nullptr;
	std::span<const vk::DescriptorSetLayoutBinding> layout_bindings;
	bool                                            push_descriptors = false;
	vk::PushConstantRange                           push_range {};
	bool                                            mesh = false, rect_list = false, tess = false;
	uint64_t                                        vs_id = 0, ps_id = 0;
	int64_t                                         game_driver_ns = 0;
	int64_t                                         game_total_ns  = 0;
};

void WriteGraphicsPipeline(const GraphicsDump& dump);

struct ComputeDump {
	uint64_t                                        cs_id                  = 0;
	uint32_t                                        required_subgroup_size = 0; // 0 = none
	std::span<const vk::DescriptorSetLayoutBinding> layout_bindings;
	bool                                            push_descriptors = false;
	int64_t                                         game_driver_ns   = 0;
};

void WriteComputePipeline(const ComputeDump& dump);

} // namespace Libs::Graphics::PipelineDump

#endif
