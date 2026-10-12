#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERGCGATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERGCGATE_H_

// Pure gate of the buffer cache's "aggressive" garbage collection (K42), unit tested in
// tests/BufferGcGateTests.cpp.
//
// Aggressive collection downloads GPU-modified buffers before freeing them, and every download
// batch is a full GPU drain. Stock decides it from the whole-device memory usage, which the
// texture cache shares and can pin above the critical mark with images it cannot evict (tiled,
// GPU-modified ones). The buffer cache then runs aggressive on every collection although freeing
// its own buffers cannot bring the device usage down. The gate here also asks that the buffer
// cache itself holds a material share of the critical budget before it pays for the drains.
//
// Idea from Reece Ketley's fork (ReeceKetley/KytyPS5@7c8f2bb94, GPL-2.0), re-implemented with a
// flag and a pure function.

#include <cstdint>

namespace Libs::Graphics::BufferGcGate {

// Share (percent) of the critical budget the buffer cache must hold before it may go aggressive.
// 0 keeps the stock behaviour (device usage alone decides).
constexpr uint32_t DefaultOwnSharePercent = 25;

[[nodiscard]] constexpr bool Aggressive(uint64_t device_used, uint64_t critical, uint64_t own_bytes,
                                        uint32_t own_share_percent) {
	if (device_used < critical) {
		return false;
	}
	if (own_share_percent == 0) {
		return true;
	}
	// Byte counts stay far below 2^50, so neither product can overflow 64 bits.
	return own_bytes * 100 >= critical * own_share_percent;
}

} // namespace Libs::Graphics::BufferGcGate

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERGCGATE_H_ */
