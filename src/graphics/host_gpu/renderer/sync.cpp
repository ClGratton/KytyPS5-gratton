#include "graphics/host_gpu/renderer/sync.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/gpuClock.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/videoOut.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Libs::Graphics::Sync {

bool ScaleReferenceClock(uint64_t host_ticks, uint64_t host_frequency, uint64_t& value) {
	return GpuClock::TscToReferenceClock(host_ticks, host_frequency, value);
}

uint64_t ReadReferenceClock() {
	const auto host_frequency = LibKernel::KernelGetTscFrequency();
	const auto host_ticks     = LibKernel::KernelReadTsc();
	uint64_t   value          = 0;
	if (!ScaleReferenceClock(host_ticks, host_frequency, value)) {
		EXIT("cannot scale host clock, ticks=0x%016" PRIx64 " frequency=%" PRIu64 "\n", host_ticks,
		     host_frequency);
	}
	return value;
}

enum class EndOfPipeWriteSize : uint32_t { Dword = 4, Qword = 8 };
enum class EndOfPipeWriteAction { Write, WriteBack, Interrupt, InterruptWriteBack };

static CommandBufferDebugOp DebugOperation(EndOfPipeWriteAction action) {
	switch (action) {
		case EndOfPipeWriteAction::Write: return CommandBufferDebugOp::EopWrite;
		case EndOfPipeWriteAction::WriteBack:
		case EndOfPipeWriteAction::InterruptWriteBack: return CommandBufferDebugOp::EopWriteBack;
		case EndOfPipeWriteAction::Interrupt: return CommandBufferDebugOp::EopInterrupt;
	}
	EXIT("unsupported end-of-pipe write action\n");
	return CommandBufferDebugOp::Unknown;
}

static bool TriggersInterrupt(EndOfPipeWriteAction action) {
	return action == EndOfPipeWriteAction::Interrupt ||
	       action == EndOfPipeWriteAction::InterruptWriteBack;
}

static void RecordEndOfPipeWrite(uint64_t submit_id, CommandBuffer& buffer, uint64_t destination,
                                 uint64_t value, EndOfPipeWriteSize size,
                                 EndOfPipeWriteAction action, int interrupt_event_id = 0,
                                 uint32_t context_id = 0) {
	EXIT_IF(destination == 0);
	buffer.AssertRecording();

	const auto width      = static_cast<uint32_t>(size);
	const auto value_low  = static_cast<uint32_t>(value);
	const auto value_high = static_cast<uint32_t>(value >> 32u);
	const auto operation  = static_cast<uint32_t>(DebugOperation(action));
	if (TriggersInterrupt(action)) {
		buffer.SetDebugInfo(operation, submit_id, width, context_id, value_low, value_high,
		                    destination);
		TriggerEopEventAtEndOfPipe(buffer, interrupt_event_id, context_id);
	} else {
		buffer.SetDebugInfo(operation, submit_id, width, value_low, value_high, 0, destination);
	}
}

void WriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                        uint32_t value) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Dword, EndOfPipeWriteAction::Write);
}

void WriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                           uint32_t dw_offset, uint32_t dw_num) {
	EXIT_IF(dst_gpu_addr == nullptr);
	buffer.AssertRecording();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWrite), submit_id,
	                    dw_offset, dw_num, 0, 0, reinterpret_cast<uint64_t>(dst_gpu_addr));
}

void WriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer& buffer, uint64_t* dst_gpu_addr,
                        uint64_t value) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Qword, EndOfPipeWriteAction::Write);
}

void WriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer& buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Qword, EndOfPipeWriteAction::WriteBack);
}

void WriteAtEndOfPipeWithWriteBack32(uint64_t submit_id, CommandBuffer& buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Dword, EndOfPipeWriteAction::WriteBack);
}

void WriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer& buffer,
                                              uint64_t* dst_gpu_addr, uint64_t value, int event_id,
                                              uint32_t context_id) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Qword, EndOfPipeWriteAction::InterruptWriteBack,
	                     event_id, context_id);
}

void WriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer& buffer,
                                              uint32_t* dst_gpu_addr, uint32_t value, int event_id,
                                              uint32_t context_id) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Dword, EndOfPipeWriteAction::InterruptWriteBack,
	                     event_id, context_id);
}

void WriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer& buffer,
                                     uint64_t* dst_gpu_addr, uint64_t value, int event_id,
                                     uint32_t context_id) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Qword, EndOfPipeWriteAction::Interrupt, event_id,
	                     context_id);
}

void WriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer& buffer,
                                     uint32_t* dst_gpu_addr, uint32_t value, int event_id,
                                     uint32_t context_id) {
	RecordEndOfPipeWrite(submit_id, buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), value,
	                     EndOfPipeWriteSize::Dword, EndOfPipeWriteAction::Interrupt, event_id,
	                     context_id);
}

uint64_t PrepareVideoOutFlip(CommandBuffer& buffer, int handle, int index, int flip_mode,
                             int64_t flip_arg) {
	for (;;) {
		uint64_t   request_id = 0;
		auto&      video_out  = buffer.GetContext().GetVideoOut();
		const auto result =
		    video_out.SubmitFlipFromGpu(buffer, handle, index, flip_mode, flip_arg, request_id);
		if (result == OK) {
			EXIT_IF(request_id == 0);
			return request_id;
		}
		if (result != VideoOut::VIDEO_OUT_ERROR_FLIP_QUEUE_FULL) {
			EXIT("GPU flip submission failed, result=%d handle=%d index=%d mode=%d arg=%" PRId64
			     "\n",
			     result, handle, index, flip_mode, flip_arg);
		}
		video_out.WaitForSubmitSlot(handle);
	}
}

void WriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer& buffer,
                                                  uint32_t* dst_gpu_addr, uint32_t value,
                                                  int handle, int index, int flip_mode,
                                                  int64_t flip_arg, uint64_t request_id,
                                                  int event_id) {
	EXIT_IF(dst_gpu_addr == nullptr);
	buffer.AssertRecording();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopWriteBackFlip), submit_id,
	                    static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                    static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));

	auto& renderer  = buffer.GetContext();
	auto& scheduler = renderer.GetCommandScheduler();
	EXIT_IF(!scheduler.Active() || &buffer != &scheduler.Current());
	scheduler.DeferPriorityOperation([&renderer, event_id, request_id] {
		renderer.GetVideoOut().CompleteFlip(request_id);
		renderer.TriggerInterrupt(event_id, 0);
	});
}

void WriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer& buffer, uint32_t* dst_gpu_addr,
                                uint32_t value, int handle, int index, int flip_mode,
                                int64_t flip_arg, uint64_t request_id) {
	EXIT_IF(dst_gpu_addr == nullptr);
	buffer.AssertRecording();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopFlip), submit_id,
	                    static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                    static_cast<uint32_t>(flip_mode), value, static_cast<uint64_t>(flip_arg));

	auto& renderer  = buffer.GetContext();
	auto& scheduler = renderer.GetCommandScheduler();
	EXIT_IF(!scheduler.Active() || &buffer != &scheduler.Current());
	scheduler.DeferPriorityOperation(
	    [&renderer, request_id] { renderer.GetVideoOut().CompleteFlip(request_id); });
}

void WriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer& buffer, int handle, int index,
                              int flip_mode, int64_t flip_arg, uint64_t request_id) {
	buffer.AssertRecording();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::EopOnlyFlip), submit_id,
	                    static_cast<uint32_t>(handle), static_cast<uint32_t>(index),
	                    static_cast<uint32_t>(flip_mode), 0, static_cast<uint64_t>(flip_arg));

	auto& renderer  = buffer.GetContext();
	auto& scheduler = renderer.GetCommandScheduler();
	EXIT_IF(!scheduler.Active() || &buffer != &scheduler.Current());
	scheduler.DeferPriorityOperation(
	    [&renderer, request_id] { renderer.GetVideoOut().CompleteFlip(request_id); });
}

void TriggerEopEventAtEndOfPipe(CommandBuffer& buffer, int event_id, uint32_t context_id) {
	buffer.AssertRecording();
	auto& renderer  = buffer.GetContext();
	auto& scheduler = renderer.GetCommandScheduler();
	EXIT_IF(!scheduler.Active() || &buffer != &scheduler.Current());
	scheduler.DeferPriorityOperation(
	    [&renderer, event_id, context_id] { renderer.TriggerInterrupt(event_id, context_id); });
}

namespace {

// The labels that are parsed but not yet in guest memory (--eop-labels-at-completion), by dword address. Added by the
// GPU thread when it parses the packet, removed by the priority-ops thread right after the store, so a reader that
// looks here first and in memory second always finds the value in one of the two.
// Ported from the label-latency work (crash2-eop-publish 9b3504777) without its diagnostics.
class PendingLabels {
public:
	uint64_t Add(const void* owner, uint64_t address, uint64_t value, uint32_t size) {
		const auto seq = m_seq.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((address & 3u) != 0) {
			return 0;
		}
		std::lock_guard lock(m_mutex);
		for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
			m_dwords[address + i * sizeof(uint32_t)] = {static_cast<uint32_t>(value >> (32u * i)), seq, owner};
		}
		m_count.fetch_add(1, std::memory_order_release);
		return seq;
	}

	void Remove(uint64_t address, uint32_t size, uint64_t seq) {
		if (seq == 0) {
			return;
		}
		{
			std::lock_guard lock(m_mutex);
			for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
				const auto it = m_dwords.find(address + i * sizeof(uint32_t));
				if (it != m_dwords.end() && it->second.seq == seq) {
					m_dwords.erase(it);
				}
			}
		}
		m_count.fetch_sub(1, std::memory_order_release);
	}

	uint64_t Read(const void* owner, const void* addr, uint32_t size, bool* from_pending) {
		const auto address = reinterpret_cast<uint64_t>(addr);
		const auto dwords  = size / sizeof(uint32_t);
		uint64_t   value   = 0;
		uint32_t   held    = 0; // dwords supplied by a held-back label
		if (m_count.load(std::memory_order_acquire) != 0 && (address & 3u) == 0) {
			std::lock_guard lock(m_mutex);
			for (uint32_t i = 0; i < dwords; i++) {
				const auto it = m_dwords.find(address + i * sizeof(uint32_t));
				if (it != m_dwords.end() && it->second.owner == owner) {
					value |= static_cast<uint64_t>(it->second.value) << (32u * i);
					held |= 1u << i;
				}
			}
		}
		// Memory is read outside the lock: a read fault (a GPU-modified page) must not hold it.
		for (uint32_t i = 0; i < dwords; i++) {
			if ((held & (1u << i)) == 0) {
				uint32_t dword = 0;
				std::memcpy(&dword, static_cast<const uint8_t*>(addr) + i * sizeof(uint32_t), sizeof(dword));
				value |= static_cast<uint64_t>(dword) << (32u * i);
			}
		}
		if (from_pending != nullptr) {
			*from_pending = held != 0;
		}
		return value;
	}

private:
	struct Entry {
		uint32_t    value = 0;
		uint64_t    seq   = 0;
		const void* owner = nullptr; // the command scheduler (queue) whose stream parsed the label
	};
	std::mutex                          m_mutex;
	std::unordered_map<uint64_t, Entry> m_dwords;
	std::atomic<uint32_t>               m_count {0};
	std::atomic<uint64_t>               m_seq {0};
};

PendingLabels& Pending() {
	static auto* labels = new PendingLabels; // leaked: priority operations may run while the process exits
	return *labels;
}

} // namespace

uint64_t ReadForCommandProcessor(const void* owner, const void* addr, uint32_t size, bool* from_pending) {
	EXIT_IF(addr == nullptr || (size != sizeof(uint32_t) && size != sizeof(uint64_t)));
	return Pending().Read(owner, addr, size, from_pending);
}

bool PublishLabel(CommandBuffer& buffer, void* dst, uint64_t value, uint32_t size, bool gpu_clock_sample) {
	EXIT_IF(dst == nullptr || (size != sizeof(uint32_t) && size != sizeof(uint64_t)));
	buffer.AssertRecording();
	auto& renderer  = buffer.GetContext();
	auto& scheduler = renderer.GetCommandScheduler();
	if (gpu_clock_sample || !Config::EopLabelsAtCompletionEnabled() || !scheduler.Active() ||
	    &buffer != &scheduler.Current()) {
		return false; // the caller writes it now, at parse time
	}
	// Published with the other completions of this command buffer (interrupts, flips), in the order they were
	// parsed: the label first, then the interrupt of the same packet. Like a GPU timestamp rewrite or a read-back
	// it goes through the backing view: a completion is not allowed to take a tracker fault, and memory the guest
	// unmapped meanwhile simply has nothing to receive it (logged by WriteBacking). The command processor keeps
	// seeing the label from now on (ReadForCommandProcessor).
	const auto seq = Pending().Add(&scheduler, reinterpret_cast<uint64_t>(dst), value, size);
	scheduler.DeferPriorityOperation([dst, value, size, seq] {
		LibKernel::Memory::WriteBacking(reinterpret_cast<uint64_t>(dst), &value, size);
		Pending().Remove(reinterpret_cast<uint64_t>(dst), size, seq);
	});
	return true;
}

static void InterruptEventResetFunc(LibKernel::EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void InterruptEventTriggerFunc(LibKernel::EventQueue::KernelEqueueEvent* event,
                                      void*                                     trigger_data) {
	EXIT_IF(event == nullptr);

	// Every interrupt is delivered with its own context id (the guest tells them apart by it).
	LibKernel::EventQueue::KernelQueueTrigger(event, reinterpret_cast<intptr_t>(trigger_data));
}

int AddEqEvent(RenderContext& renderer, LibKernel::EventQueue::KernelEqueue eq, int id,
               void* udata) {
	LibKernel::EventQueue::KernelEqueueEvent event;
	event.triggered           = false;
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.udata         = udata;
	event.event.fflags        = 0;
	event.event.data          = id;
	event.filter.reset_func   = InterruptEventResetFunc;
	event.filter.trigger_func = InterruptEventTriggerFunc;

	int result = LibKernel::EventQueue::KernelAddEvent(eq, event);

	if (result == 0) {
		renderer.AddInterruptEq(eq, id);
	}

	return result;
}

int DeleteEqEvent(RenderContext& renderer, LibKernel::EventQueue::KernelEqueue eq, int id) {
	int result = LibKernel::EventQueue::KernelDeleteEvent(
	    eq, static_cast<uintptr_t>(id), LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	if (result == OK || result == LibKernel::KERNEL_ERROR_ENOENT) {
		renderer.DeleteInterruptEq(eq, id);
	}

	return result;
}

void ReadGds(const Buffer& gds, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size) {
	const auto offset = uint64_t {dw_offset} * sizeof(uint32_t);
	const auto size   = uint64_t {dw_size} * sizeof(uint32_t);
	EXIT_IF(dst == nullptr || offset > gds.Size() || size > gds.Size() - offset ||
	        gds.Mapped().empty());
	std::memcpy(dst, gds.Mapped().data() + offset, static_cast<size_t>(size));
}

} // namespace Libs::Graphics::Sync
