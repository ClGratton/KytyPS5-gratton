#include "graphics/host_gpu/renderer/cache/gcClock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::GcClock;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GcClockTests: failed: %s\n", message);
		std::abort();
	}
}

void TestTick() {
	Check(Tick(Mode::Runs, 1234, 99) == 1234, "runs mode ticks per collector run");
	Check(Tick(Mode::Frames, 1234, 99) == 99 + 2, "frames mode: frames plus one step per 512 runs");
	Check(Tick(Mode::Frames, 511, 7) == 7, "no step before 512 runs");
	// Monotone as either counter grows.
	uint64_t last = 0;
	for (uint64_t runs = 0; runs < 5000; runs += 37) {
		const auto tick = Tick(Mode::Frames, runs, runs / 100);
		Check(tick >= last, "tick never goes back");
		last = tick;
	}
	// A hundred runs a frame: 16 runs is a fraction of a frame in stock, a whole second in frames.
	Check(Tick(Mode::Runs, 16, 0) - Tick(Mode::Runs, 0, 0) == 16, "stock age 16 = 16 runs");
}

void TestStockAgesUnchanged() {
	const auto t = TextureAgesFor(Mode::Runs, 60);
	Check(t.normal == 16 && t.pressured == 80 && t.aggressive == 160, "stock texture ages");
	const auto b = BufferAgesFor(Mode::Runs, 144);
	Check(b.normal == 160 && b.aggressive == 80, "stock buffer ages ignore the refresh rate");
}

void TestFrameAgesScaleWithRefresh() {
	const auto t60  = TextureAgesFor(Mode::Frames, 60);
	const auto t120 = TextureAgesFor(Mode::Frames, 120);
	Check(t60.normal == 60 && t60.pressured == 300 && t60.aggressive == 600, "1/5/10 s at 60 Hz");
	Check(t120.normal == 120 && t120.aggressive == 1200, "twice the frames at 120 Hz");
	Check(t60.normal < t60.pressured && t60.pressured < t60.aggressive, "ordered like stock");
	const auto b60 = BufferAgesFor(Mode::Frames, 60);
	Check(b60.normal == 300 && b60.aggressive == 150, "buffers 5 s / 2.5 s");
	Check(TextureAgesFor(Mode::Frames, 0).normal == 60, "refresh 0 falls back to 60");
}

void TestAgeReached() {
	Check(AgeReached(Mode::Runs, 3, 160), "runs mode clamps the age instead");
	Check(!AgeReached(Mode::Frames, 59, 60), "frames mode waits for the age");
	Check(AgeReached(Mode::Frames, 60, 60), "and applies it at the age");
}

} // namespace

int main() {
	TestTick();
	TestStockAgesUnchanged();
	TestFrameAgesScaleWithRefresh();
	TestAgeReached();
	std::printf("GcClockTests: all passed\n");
	return 0;
}
