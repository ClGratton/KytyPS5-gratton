#include "graphics/presentation/window/swapchainExtent.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::ChooseSwapchainExtent;
using Libs::Graphics::SwapchainExtent;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SwapchainExtentTests: failed: %s\n", message);
		std::abort();
	}
}

bool Is(SwapchainExtent extent, uint32_t width, uint32_t height) {
	return extent.width == width && extent.height == height;
}

} // namespace

int main() {
	const SwapchainExtent min_extent {1, 1};
	const SwapchainExtent max_extent {16384, 16384};
	const SwapchainExtent window {2560, 1440};
	// The surface fixes the size: use it.
	Check(Is(ChooseSwapchainExtent({1920, 1080}, min_extent, max_extent, window), 1920, 1080),
	      "a fixed surface extent must be used as is");
	// "Application chooses": the window size, clamped to the limits.
	Check(Is(ChooseSwapchainExtent({UINT32_MAX, UINT32_MAX}, min_extent, max_extent, window), 2560,
	         1440),
	      "an undefined extent must follow the window");
	Check(Is(ChooseSwapchainExtent({UINT32_MAX, UINT32_MAX}, min_extent, {2048, 1024}, window),
	         2048, 1024),
	      "the window size must be clamped to the surface maximum");
	// A minimized window reports 0x0: never create a zero-sized swapchain.
	Check(Is(ChooseSwapchainExtent({0, 0}, min_extent, max_extent, window), 2560, 1440),
	      "a zero extent must fall back to the window size");
	Check(Is(ChooseSwapchainExtent({0, 0}, {0, 0}, {0, 0}, window), 1, 1),
	      "zero limits must still give a non-zero extent");
	Check(Is(ChooseSwapchainExtent({0, 0}, min_extent, max_extent, {0, 0}), 1, 1),
	      "a zero window must still give a non-zero extent");
	Check(Is(ChooseSwapchainExtent({1280, 0}, min_extent, max_extent, window), 2560, 1440),
	      "a zero height alone must fall back too");
	std::puts("SwapchainExtentTests: all cases passed");
	return 0;
}
