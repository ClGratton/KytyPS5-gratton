#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCOST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCOST_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {
class CommandScheduler;
struct GraphicContext;
namespace ShaderRecompiler::IR {
struct CompiledShaderInfo;
}
} // namespace Libs::Graphics

// Debug-only GPU cost attribution (env KYTY_GPU_COST=<output prefix>, nothing is on by default).
//
// The Vulkan dispatcher's vkCmd* entry points that do GPU work (draws, dispatches, copies, clears,
// barriers, render passes) are replaced by wrappers that call the driver and then write a GPU
// timestamp, so the time between two consecutive timestamps is attributed to the operation that
// ended at the later one. Draws and dispatches carry the shader hashes the renderer tagged just
// before binding the pipeline. Results go to CSV files, see gpuCost.cpp for the columns.
//
//   KYTY_GPU_COST_MODE   cb      only one begin and one end timestamp per command buffer (cheap)
//                        bottom  + BOTTOM_OF_PIPE timestamp after every operation (default)
//                        top     + TOP_OF_PIPE timestamp after every operation
//                        both    BOTTOM_OF_PIPE after and TOP_OF_PIPE before every operation
//   KYTY_GPU_COST_EVERY  per-operation timestamps for KYTY_GPU_COST_WINDOW guest frames out of
//   KYTY_GPU_COST_WINDOW every EVERY frames (default 100 / 20); cb timestamps are always on
//   KYTY_GPU_COST_START  first guest frame (flip count) that can open a window (default 0)
namespace Libs::Graphics::GpuCost {

extern std::atomic<bool> g_enabled;

[[nodiscard]] inline bool Enabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
}

// Reads the environment and, if requested, wraps the dispatcher. Once per process.
void Initialize(GraphicContext& graphics);

// Shader hashes of the draw / dispatch whose pipeline is bound next (this thread).
void Tag(uint64_t shader0, uint64_t shader1);

// Writes one line per shader the first time it is tagged (stage, size, features).
void DescribeShader(const char* stage, const ShaderRecompiler::IR::CompiledShaderInfo& program,
                    uint32_t threads_per_group);

// A guest flip was recorded: the frame boundary.
void OnFlip();

// The scheduler began / submitted a command buffer.
void OnBegin(vk::CommandBuffer buffer);
void OnSubmit(vk::CommandBuffer buffer, uint64_t tick, CommandScheduler& scheduler);

} // namespace Libs::Graphics::GpuCost

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCOST_H_
