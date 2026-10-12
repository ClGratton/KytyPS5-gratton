#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKPLAN_H_

// Pure planning for --readback-queue (K20): a synchronous GPU->guest readback copies on a second
// queue and waits only for the work that wrote the data, not for everything queued since. Unit
// tested in tests/ReadbackPlanTests.cpp.
//
// Ticks are the scheduler's command-buffer ticks (the master timeline semaphore value that
// buffer's submit signals). `buffer_write_tick` is the tick current when the buffer was last
// recorded as GPU-written (or created: a merge copies GPU data into it), `address_write_tick`
// the tick of the latest shader that may store through raw addresses into any buffer. The data
// the readback copies was written at or before the larger of the two; commands recorded after
// that tick never write the copied ranges.
//
// Ported from ogeidexe/KytyPS5@971c6a49c (second readback queue with a timeline wait on the tick
// of the buffer's last GPU write); GPL-2.0, re-implemented.

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

struct ReadbackPlan {
	uint64_t wait_tick   = 0;     // timeline value the copy must wait for (0: nothing to wait for)
	bool     flush_first = false; // the producer is still in the open command buffer: submit it
};

[[nodiscard]] constexpr ReadbackPlan PlanReadback(uint64_t buffer_write_tick,
                                                  uint64_t address_write_tick,
                                                  uint64_t current_tick) {
	ReadbackPlan plan;
	plan.wait_tick   = std::max(buffer_write_tick, address_write_tick);
	// The open command buffer will be submitted with tick `current_tick`; a producer stamped with
	// it (or, defensively, a later one) is not in the queue yet.
	plan.flush_first = plan.wait_tick >= current_tick;
	if (plan.wait_tick > current_tick) {
		plan.wait_tick = current_tick;
	}
	return plan;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKPLAN_H_
