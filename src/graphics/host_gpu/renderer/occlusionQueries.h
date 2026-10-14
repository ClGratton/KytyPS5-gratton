#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;

// The guest's occlusion counters, backed by host occlusion queries (--occlusion-queries).
//
// A title brackets draws with two ZPASS_DONE events, a begin dump at address A and an end dump at
// A + 8, and reads the difference of the counters as the number of samples that passed; many titles
// cull objects with it. Each dump writes the counter of all 16 DBs to address + db * 16 with bit 63
// set once the value is there. Here the whole count is in DB 0 (the others read zero), and a dump
// publishes the running total of the samples of every earlier segment of the draw stream, so any
// begin/end pairing (nested too) reads the samples drawn between its two dumps.
//
// Host occlusion queries count the samples, only while a guest query is open and only inside a
// render pass instance (they never split one). The counters reach guest memory once the GPU has
// executed the command buffer that holds the dump (a priority operation, like the GPU timestamps),
// and before a frame's flip the GPU thread waits for the dumps of earlier frames, so the counters of
// a frame are in guest memory by the next flip, as on a console whose GPU is at most about a frame
// behind. Without --occlusion-queries or without host query reset support a dump publishes a count
// that always reads visible.
namespace OcclusionQueries {

// A ZPASS_DONE dump to `address` (8-byte aligned) in the command buffer being recorded. Returns
// false when the host does not back the counters; the caller then publishes the visible count.
[[nodiscard]] bool Dump(CommandBuffer& command, uint64_t address);

// The command buffer began / is about to end a render pass instance.
void OnBeginRendering(const CommandBuffer& command);
void OnEndRendering(const CommandBuffer& command);

// A guest flip is being recorded: the frame boundary; waits for the dumps of earlier frames.
void OnGuestFlip(CommandBuffer& command);

} // namespace OcclusionQueries
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
