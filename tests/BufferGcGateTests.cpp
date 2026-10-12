#include "graphics/host_gpu/renderer/cache/bufferGcGate.h"

#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::BufferGcGate;

constexpr uint64_t MiB = 1024ull * 1024;
constexpr uint64_t GiB = 1024ull * MiB;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "BufferGcGateTests: failed: %s\n", message);
		std::abort();
	}
}

void TestStockWhenShareIsZero() {
	// Share 0 is the stock rule: whole-device usage alone decides.
	Check(Aggressive(5 * GiB, 4 * GiB, 0, 0), "above critical, share 0, no buffers: aggressive");
	Check(Aggressive(4 * GiB, 4 * GiB, 1, 0), "at critical counts as above");
	Check(!Aggressive(4 * GiB - 1, 4 * GiB, 4 * GiB, 0), "below critical is never aggressive");
}

void TestTexturePinnedDevice() {
	// The texture cache pins the device above critical; the buffer cache holds little: not aggressive.
	const uint64_t critical = 4 * GiB;
	Check(!Aggressive(6 * GiB, critical, 200 * MiB, 25), "200 MiB of buffers under a 4 GiB mark");
	Check(!Aggressive(6 * GiB, critical, GiB - 1, 25), "one byte below 25 percent");
	Check(Aggressive(6 * GiB, critical, GiB, 25), "exactly 25 percent goes aggressive");
	Check(Aggressive(6 * GiB, critical, 3 * GiB, 25), "a big buffer cache goes aggressive");
}

void TestDeviceBelowCriticalWins() {
	// Owning most of the budget does not matter while the device itself has room.
	Check(!Aggressive(2 * GiB, 4 * GiB, 2 * GiB, 25), "buffers alone below critical: not aggressive");
}

void TestShareScalesWithBudget() {
	// The threshold is a share of whatever critical is, so an 8 GiB card and a 24 GiB card
	// behave the same way relative to their budget.
	for (uint64_t critical: {2 * GiB, 4 * GiB, 12 * GiB, 20 * GiB}) {
		const uint64_t quarter = critical / 4;
		Check(Aggressive(critical, critical, quarter, 25), "quarter of any budget is enough");
		Check(!Aggressive(critical, critical, quarter - MiB, 25), "a bit less is not");
		Check(Aggressive(critical, critical, critical / 2, 50), "50 percent share");
		Check(!Aggressive(critical, critical, critical / 2 - MiB, 50), "just under 50 percent");
	}
}

void TestHugeValuesDoNotOverflow() {
	const uint64_t big = 1ull << 49;
	Check(Aggressive(big, big, big, 100), "100 percent of a huge budget");
	Check(!Aggressive(big, big, big - 1, 100), "one byte short of 100 percent");
}

void TestDefaultShareConstant() {
	Check(DefaultOwnSharePercent == 25, "default share is a quarter of the critical budget");
}

} // namespace

int main() {
	TestStockWhenShareIsZero();
	TestTexturePinnedDevice();
	TestDeviceBelowCriticalWins();
	TestShareScalesWithBudget();
	TestHugeValuesDoNotOverflow();
	TestDefaultShareConstant();
	std::printf("BufferGcGateTests: all passed\n");
	return 0;
}
