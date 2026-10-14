// Which PM4 packets announce a completion to the guest (command_processor/completionSignals.h).

#include "graphics/guest_gpu/command_processor/completionSignals.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "CompletionSignalTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint32_t Header(uint32_t len, uint32_t op, uint32_t r = 0) {
	return KYTY_PM4(len, op, r);
}

} // namespace

int main() {
	// Packets that write a label, raise an interrupt or flip.
	Check(PacketCanCompleteForGuest(Header(6, Pm4::IT_EVENT_WRITE_EOP)), "EVENT_WRITE_EOP");
	Check(PacketCanCompleteForGuest(Header(5, Pm4::IT_EVENT_WRITE_EOS)), "EVENT_WRITE_EOS");
	Check(PacketCanCompleteForGuest(Header(5, Pm4::IT_WRITE_DATA)), "WRITE_DATA");
	Check(PacketCanCompleteForGuest(Header(6, Pm4::IT_COPY_DATA)), "COPY_DATA");
	Check(PacketCanCompleteForGuest(Header(7, Pm4::IT_DMA_DATA)), "DMA_DATA");
	Check(PacketCanCompleteForGuest(Header(5, Pm4::IT_GET_LOD_STATS)), "GET_LOD_STATS writes a label");
	Check(PacketCanCompleteForGuest(Header(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM)), "RELEASE_MEM");
	Check(PacketCanCompleteForGuest(Header(7, Pm4::IT_NOP, Pm4::R_FLIP)), "FLIP");

	// Packets that do not.
	Check(!PacketCanCompleteForGuest(Header(4, Pm4::IT_DRAW_INDEX_AUTO)), "draws");
	Check(!PacketCanCompleteForGuest(Header(5, Pm4::IT_DISPATCH_DIRECT)), "dispatches");
	Check(!PacketCanCompleteForGuest(Header(4, Pm4::IT_SET_CONTEXT_REG)), "register writes");
	Check(!PacketCanCompleteForGuest(Header(4, Pm4::IT_SET_SH_REG)), "SH register writes");
	Check(!PacketCanCompleteForGuest(Header(4, Pm4::IT_EVENT_WRITE)), "plain event writes (flushes, occlusion dump)");
	Check(!PacketCanCompleteForGuest(Header(7, Pm4::IT_WAIT_REG_MEM)), "waits");
	Check(!PacketCanCompleteForGuest(Header(2, Pm4::IT_NOP)), "plain NOP");
	Check(!PacketCanCompleteForGuest(Header(2, Pm4::IT_NOP, Pm4::R_PUSH_MARKER)), "markers");
	Check(!PacketCanCompleteForGuest(Header(2, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE)), "waiting for a flip");
	Check(!PacketCanCompleteForGuest(Header(2, Pm4::IT_NOP, Pm4::R_ACQUIRE_MEM)), "acquire");
	Check(!PacketCanCompleteForGuest(Header(2, Pm4::IT_INDIRECT_BUFFER)), "indirect buffer jumps");

	std::printf("CompletionSignalTests: ok\n");
	return 0;
}
