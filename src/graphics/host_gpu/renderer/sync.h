#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_

#include "kernel/eventQueue.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;
class Buffer;
class RenderContext;

namespace Sync {

[[nodiscard]] bool     ScaleReferenceClock(uint64_t host_ticks, uint64_t host_frequency,
                                           uint64_t& value);
[[nodiscard]] uint64_t ReadReferenceClock();

void TriggerEopEventAtEndOfPipe(CommandBuffer& buffer, int event_id, uint32_t context_id);

// Writes the data of an end-of-pipe packet (a label the guest polls) to guest memory. With
// --eop-labels-at-completion (default) the guest sees it when the GPU has finished the command buffer that
// holds the packet, in GPU order and in front of the interrupt of the same packet; otherwise, and for a GPU
// clock sample (GpuTimestamps stores its own value now and rewrites it with the GPU's time), it is written
// when the packet is parsed. Parsed early, a label tells a title that a frame is finished while the GPU thread
// is still up to a few frames ahead of the GPU. Returns true when the label was queued for completion; false
// means the caller writes it now (its own guest-store path), at parse time.
[[nodiscard]] bool PublishLabel(CommandBuffer& buffer, void* dst, uint64_t value, uint32_t size,
                  bool gpu_clock_sample = false);

void WriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                        uint32_t value);
void WriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer& buffer, uint64_t* dst_gpu_addr,
                        uint64_t value);
void WriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                           uint32_t dw_offset, uint32_t dw_num);
void WriteAtEndOfPipeWithWriteBack32(uint64_t submit_id, CommandBuffer& buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value);
void WriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer& buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value);
void WriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer& buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value, int event_id,
                                     uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer& buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value, int event_id,
                                     uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer& buffer,
                                              uint32_t* dst_gpu_addr, uint32_t value, int event_id,
                                              uint32_t context_id = 0);
void WriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer& buffer,
                                              uint64_t* dst_gpu_addr, uint64_t value, int event_id,
                                              uint32_t context_id = 0);

[[nodiscard]] uint64_t PrepareVideoOutFlip(CommandBuffer& buffer, int handle, int index,
                                           int flip_mode, int64_t flip_arg);
void WriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer& buffer, int handle, int index,
                              int flip_mode, int64_t flip_arg, uint64_t request_id);
void WriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                                uint32_t value, int handle, int index, int flip_mode,
                                int64_t flip_arg, uint64_t request_id);
void WriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer& buffer,
                                                  uint32_t* dst_gpu_addr, uint32_t value,
                                                  int handle, int index, int flip_mode,
                                                  int64_t flip_arg, uint64_t request_id,
                                                  int event_id);

int  AddEqEvent(RenderContext& renderer, LibKernel::EventQueue::KernelEqueue eq, int id,
                void* udata);
int  DeleteEqEvent(RenderContext& renderer, LibKernel::EventQueue::KernelEqueue eq, int id);
void ReadGds(const Buffer& gds, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

} // namespace Sync
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SYNC_H_
