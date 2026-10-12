#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;

// The 64-bit GPU timestamps a guest reads: EVENT_WRITE_EOP with the GPU clock as data source and
// COPY_DATA from the GPU clock. A title that sizes a dynamic resolution from them (the time
// between begin/end markers) decides its render resolution from what it reads here.
//
// --gpu-timestamps legacy: the reference clock when the emulator parses the packet.
// --gpu-timestamps gpu:    that value first (so an early read sees what legacy would give), then,
//                          once the command buffer executes, the host GPU's own timestamp of that
//                          point, converted to the reference clock.
// --gpu-time-scale:        multiplies the durations in either source (see GpuClock::TimeScaler).
namespace GpuTimestamps {

// Where in the pipeline the guest timestamp is taken.
enum class Point {
	TopOfPipe,    // the command processor reaches the packet (COPY_DATA)
	BottomOfPipe, // all earlier work finished (end-of-pipe events)
};

// The value to store at dst (size bytes, 4 or 8) now. In gpu mode a later rewrite of the 64-bit
// value at dst with the GPU time may be queued on the command buffer's scheduler.
[[nodiscard]] uint64_t SampleGuestClock(CommandBuffer& command, void* dst, uint32_t size,
                                        Point point);

// A guest flip was recorded in command: the frame boundary the time scale stretches from.
void OnGuestFlip(CommandBuffer& command);

} // namespace GpuTimestamps
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
