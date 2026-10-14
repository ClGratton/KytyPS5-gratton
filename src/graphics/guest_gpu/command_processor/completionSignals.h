#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMPLETION_SIGNALS_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMPLETION_SIGNALS_H

// Which PM4 packets can tell the guest CPU that GPU work has completed (common/guestRetirement.h).
//
// The guest reuses the memory of a submission only after it has observed a completion. The GPU
// thread makes completions visible by writing guest memory (labels), triggering interrupts and
// flips. Every packet below does one of those, and is announced to the retirement tracker BEFORE
// it executes. Packets that only change GPU state, draw, dispatch or synchronise inside the GPU
// (WAIT_REG_MEM, ACQUIRE_MEM, event writes that only flush) tell the guest nothing.
//
// Not listed on purpose:
//   IT_EVENT_WRITE with event 0x39 (occlusion counter dump): Kyty publishes a synthetic always-ready
//     result at once, independent of any work, so it says nothing about completion.
//   IT_DUMP_CONST_RAM: constant-engine copy of constants into memory the draw engine reads.
// A new packet that writes guest-visible memory or raises an event belongs here.

#include "graphics/guest_gpu/pm4.h"

#include <cstdint>

namespace Libs::Graphics {

[[nodiscard]] constexpr bool PacketCanCompleteForGuest(uint32_t packet_header) noexcept {
	const uint32_t opcode = (packet_header >> 8u) & 0xffu;
	switch (opcode) {
		case Pm4::IT_EVENT_WRITE_EOP:
		case Pm4::IT_EVENT_WRITE_EOS:
		case Pm4::IT_WRITE_DATA:
		case Pm4::IT_COPY_DATA:
		case Pm4::IT_DMA_DATA:
		case Pm4::IT_GET_LOD_STATS: return true;
		case Pm4::IT_NOP:
			switch (KYTY_PM4_R(packet_header)) {
				case Pm4::R_RELEASE_MEM:
				case Pm4::R_FLIP: return true;
				default: return false;
			}
		default: return false;
	}
}

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_COMPLETION_SIGNALS_H
