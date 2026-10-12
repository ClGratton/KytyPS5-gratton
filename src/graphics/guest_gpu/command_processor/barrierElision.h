#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_BARRIERELISION_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_BARRIERELISION_H_

// Pure rule for --elide-barriers (K14): when a guest cache flush/invalidate or partial-flush event
// may leave out its global memory barrier. Unit tested in tests/BarrierElisionTests.cpp; the command
// processor owns one instance.
//
// Every such event became a full ALL_COMMANDS memory barrier (and ended the rendering instance),
// hundreds a frame in Astro Bot. The events come in runs. A barrier makes all earlier writes
// available and visible to everything after it, so a second barrier with the same scope orders
// nothing new when no command was recorded since the first one, in the same command buffer. A
// new command buffer always gets the barrier: submits are not ordered for memory by the queue.
//
// "Work" is a counter of every command-buffer access made for recording (CommandBuffer::Handle()),
// which only grows, so any recorded command, from any queue's processor, breaks a run.
//
// Ported from ogeidexe/KytyPS5@5ed6e6561 (same rule, counted per recording thread there);
// GPL-2.0, re-implemented.

#include <cstdint>

namespace Libs::Graphics {

class GlobalBarrierElider {
public:
	// `command_id` identifies the open command buffer (its tick), `work` the recorded-work counter.
	[[nodiscard]] bool CanElide(uint64_t command_id, uint64_t work) const {
		return m_valid && m_command_id == command_id && m_work == work;
	}

	// A barrier was recorded; `work` is the counter read after recording it.
	void Emitted(uint64_t command_id, uint64_t work) {
		m_valid      = true;
		m_command_id = command_id;
		m_work       = work;
	}

	void Reset() { m_valid = false; }

private:
	bool     m_valid      = false;
	uint64_t m_command_id = 0;
	uint64_t m_work       = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_BARRIERELISION_H_
