#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCCLOCK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCCLOCK_H_

// Pure clock and age policy of the texture and buffer garbage collectors (K43), unit tested in
// tests/GcClockTests.cpp.
//
// The collectors run once per completed guest submission, and a title can submit a hundred times
// a frame, so "an image idle for 16 collector runs" can mean "not used since earlier in this very
// frame" (nahshongraham97/Mac: 11,000 image creations a second at a menu). With --submit-coalesce
// the runs per frame drop about 15x and the same age silently stretches. In Frames mode the LRU
// clock is the number of presented frames, plus one step per 512 collector runs so a stretch
// without presents (a loading screen) still ages; the ages are seconds scaled by the display
// refresh rate, so they do not depend on how often the game submits.
//
// Idea from nahshongraham97's forks (0fd50e6da, 40cdd7f14, 7573eb224; GPL-2.0), re-implemented.

#include <cstdint>

namespace Libs::Graphics::GcClock {

enum class Mode : uint8_t {
	Runs,   // one tick per collector run (stock)
	Frames, // one tick per presented frame
};

constexpr uint64_t RunsPerFallbackStep = 512;

[[nodiscard]] constexpr uint64_t Tick(Mode mode, uint64_t runs, uint64_t frames) {
	return mode == Mode::Runs ? runs : frames + runs / RunsPerFallbackStep;
}

struct TextureAges {
	uint64_t normal;     // trigger reached, below the pressure mark
	uint64_t pressured;  // above the pressure mark
	uint64_t aggressive; // above the critical mark
};

struct BufferAges {
	uint64_t normal;
	uint64_t aggressive;
};

// Runs mode keeps the stock constants. Frames mode: 1 s, 5 s and 10 s (the 1 : 5 : 10 ratio of the
// stock 16 : 80 : 160) at the display refresh rate; a 600-flip (10 s) age avoided the re-uploads
// of a shorter one in chenxiao07/KytyPS5@45beec32a.
[[nodiscard]] constexpr TextureAges TextureAgesFor(Mode mode, uint32_t refresh_hz) {
	if (mode == Mode::Runs) {
		return {16, 80, 160};
	}
	const uint64_t second = refresh_hz == 0 ? 60 : refresh_hz;
	return {second, 5 * second, 10 * second};
}

// Stock buffers: 160 runs, 80 under critical pressure. Frames mode: 5 s and 2.5 s.
[[nodiscard]] constexpr BufferAges BufferAgesFor(Mode mode, uint32_t refresh_hz) {
	if (mode == Mode::Runs) {
		return {160, 80};
	}
	const uint64_t second = refresh_hz == 0 ? 60 : refresh_hz;
	return {5 * second, 5 * second / 2};
}

// Whether the age threshold can be applied yet: in Frames mode nothing is old enough before `age`
// ticks have passed (Runs mode clamps the age to the tick, as before).
[[nodiscard]] constexpr bool AgeReached(Mode mode, uint64_t tick, uint64_t age) {
	return mode == Mode::Runs || tick >= age;
}

} // namespace Libs::Graphics::GcClock

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCCLOCK_H_ */
