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

// KYTY_FRAME_PATH (common/framePath.h): GPU begin/end of every command buffer in host CLOCK_MONOTONIC
// ns. Needs --gpu-timestamps gpu (the query ring and the calibration are shared). Debug only.
// FramePathStamp records a timestamp at `point` in `command` and returns its slot (kNoSlot if the
// ring is full or the mode is not available); FramePathCommit ties the two slots of a command buffer
// to the timeline tick it signals; FramePathResolve logs a GpuExec event for every committed buffer
// whose end timestamp is available.
constexpr uint32_t kNoSlot = 0xffffffffu;
[[nodiscard]] uint32_t FramePathStamp(CommandBuffer& command, Point point);
void                   FramePathCommit(uint64_t tick, uint32_t begin_slot, uint32_t end_slot);
void                   FramePathResolve();

} // namespace GpuTimestamps
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
