#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMPUTEGPUTIMES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMPUTEGPUTIMES_H_

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;

// --compute-gpu-times <seconds>: the GPU time of every compute dispatch, per shader and thread
// group count. Two timestamps bracket the dispatch in the command buffer; once it has executed a
// priority operation adds the difference to a table, which is printed (busiest first) and written to
// _compute_gpu_times.csv every N seconds. The timestamps are taken at the compute stage, so a
// dispatch that overlaps an earlier one is charged the overlap: compare two runs of the same
// workload, not a dispatch with the sum of the frame. Adds queries and a lock per dispatch: never an
// fps baseline.
namespace ComputeGpuTimes {

struct Scope {
	uint32_t slot   = 0; // first of the two query slots
	bool     active = false;
};

// Records the first timestamp of a dispatch (inactive when the option is off or the device cannot).
[[nodiscard]] Scope Begin(CommandBuffer& command);

// Records the second timestamp and queues the read-out under the dispatch's identity. `generic` is
// whether the variant that ran is a --generic-compute one.
void End(CommandBuffer& command, const Scope& scope, uint64_t shader_hash, uint32_t groups_x,
         uint32_t groups_y, uint32_t groups_z, bool generic);

} // namespace ComputeGpuTimes
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMPUTEGPUTIMES_H_
