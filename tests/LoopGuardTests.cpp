#include "common/loopGuard.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace Common::LoopGuard;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LoopGuardTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint32_t Limit = 100000;

void TestSlots() {
	static_assert(BufferBytes == Slots * sizeof(uint32_t));
	Check(SlotFor(0x0123456789abcdefull) < Slots, "slot in range");
	Check(SlotFor(1) != SlotFor(2) || SlotFor(2) != SlotFor(3), "slots spread");
	Check(ReportThreshold(1u << 20) == 16384u, "threshold is a 64th of the limit");
	Check(ReportThreshold(100) == 16u, "threshold has a floor");
}

void TestTripIsLoudAndOnce() {
	Monitor monitor;
	monitor.Register(0xabc, "cs");
	std::array<uint32_t, Slots> tail {};
	Check(monitor.Observe(tail, Limit).empty(), "nothing reported when nothing ran long");
	tail[SlotFor(0xabc)] = Limit;
	auto lines = monitor.Observe(tail, Limit);
	Check(lines.size() == 1, "a trip is reported");
	Check(lines[0].find("LOOP GUARD TRIPPED") != std::string::npos, "the trip is loud");
	Check(lines[0].find("cs 0000000000000abc") != std::string::npos, "the shader is named");
	Check(monitor.Observe(tail, Limit).empty(), "a trip is reported once per shader");
}

void TestHighWaterMarks() {
	Monitor monitor;
	monitor.Register(0x10, "ps");
	std::array<uint32_t, Slots> tail {};
	tail[SlotFor(0x10)] = 5000;
	auto lines = monitor.Observe(tail, Limit);
	Check(lines.size() == 1 && lines[0].find("used up to 5000") != std::string::npos,
	      "a heavy shader is reported with its count");
	tail[SlotFor(0x10)] = 6000;
	Check(monitor.Observe(tail, Limit).empty(), "small growth stays quiet");
	tail[SlotFor(0x10)] = 12000;
	Check(monitor.Observe(tail, Limit).size() == 1, "a doubling is reported");
	tail[SlotFor(0x10)] = 100;
	Check(monitor.Observe(tail, Limit).empty(), "below the threshold is never reported");
	const auto summary = monitor.Summary(Limit);
	Check(summary.size() == 1 && summary[0].find("max 12000") != std::string::npos, "summary keeps the maximum");
}

void TestSlotSharing() {
	Monitor monitor;
	uint64_t a = 0x1111, b = 0;
	for (b = 0x2000; b < 0x2000 + 100000; b++) {
		if (SlotFor(b) == SlotFor(a)) break;
	}
	monitor.Register(a, "vs");
	monitor.Register(b, "ps");
	std::array<uint32_t, Slots> tail {};
	tail[SlotFor(a)] = Limit;
	const auto lines = monitor.Observe(tail, Limit);
	Check(lines.size() == 1 && lines[0].find(" or ") != std::string::npos,
	      "a shared slot names every candidate shader");
}

void TestUnregistered() {
	Monitor monitor;
	std::array<uint32_t, Slots> tail {};
	tail[7] = Limit;
	const auto lines = monitor.Observe(tail, Limit);
	Check(lines.size() == 1 && lines[0].find("unregistered") != std::string::npos,
	      "a trip of an unknown slot is still reported");
}

} // namespace

int main() {
	TestSlots();
	TestTripIsLoudAndOnce();
	TestHighWaterMarks();
	TestSlotSharing();
	TestUnregistered();
	std::printf("LoopGuardTests: all passed\n");
	return 0;
}
