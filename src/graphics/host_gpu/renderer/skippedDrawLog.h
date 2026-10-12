#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPPEDDRAWLOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPPEDDRAWLOG_H_

// A draw the emulator cannot or will not render is a rendering difference the player sees (missing
// geometry), so skipping must be loud, never silent. The first skip of a kind is logged, then the
// 2nd, 4th, 8th, ...: a game that skips a draw every frame produces about 20 lines an hour, and the
// running count in each line shows how often it happens. Unit tested in
// tests/SkippedDrawLogTests.cpp.

#include <cstdint>

namespace Libs::Graphics::SkippedDrawLog {

// `occurrence` counts from 1.
[[nodiscard]] constexpr bool ShouldLog(uint64_t occurrence) {
	return occurrence != 0 && (occurrence & (occurrence - 1)) == 0;
}

} // namespace Libs::Graphics::SkippedDrawLog

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPPEDDRAWLOG_H_ */
