#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPS_H_

// Process-wide instance of the --debug-gpu-ops ring (gpuOpRing.h) and its reports.

#include "graphics/host_gpu/renderer/gpuOpRing.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>

namespace Libs::Graphics {
struct GraphicContext;
class MasterSemaphore;
} // namespace Libs::Graphics

namespace Libs::Graphics::GpuOps {

extern std::atomic<bool> g_enabled;

[[nodiscard]] inline bool Enabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
}

// mode: 0 off, 1 ring, 2 ring + NV checkpoints (the device must have been created with the
// extension, see GraphicContext::op_checkpoints_enabled). Registers a hang-watchdog provider.
void Initialize(GraphicContext& graphics, MasterSemaphore& master, uint32_t mode);

// Records one op carried by the command buffer being recorded (`tick` = its submission's tick) and,
// in checkpoint mode, marks the command buffer. Call right before the vkCmd that issues the work.
void Record(vk::CommandBuffer command, Op op, uint64_t tick);

// Prints the unfinished ops and the queue's reached NV checkpoints to stdout and the log. `why`
// names the trigger.
void Report(const char* why, uint64_t completed_tick);

} // namespace Libs::Graphics::GpuOps

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPS_H_
