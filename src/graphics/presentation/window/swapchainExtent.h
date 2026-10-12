#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SWAPCHAINEXTENT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SWAPCHAINEXTENT_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

struct SwapchainExtent {
	uint32_t width  = 0;
	uint32_t height = 0;
};

/**
 * @brief Picks the extent for vkCreateSwapchainKHR from the surface capabilities.
 *
 * A surface whose current extent is 0xFFFFFFFF lets the swapchain choose: the window size,
 * clamped to the surface limits. A minimized window can report a current extent of 0x0 instead,
 * and a swapchain cannot be created with a zero extent (it fails or leaves presentation black
 * until the next out-of-date event). In that case the window size is used, as for the
 * "application chooses" case; the next acquire reports out-of-date and re-creates the swapchain at
 * the restored size.
 */
inline SwapchainExtent ChooseSwapchainExtent(SwapchainExtent current, SwapchainExtent min_extent,
                                             SwapchainExtent max_extent, SwapchainExtent window) {
	const auto choose_from_window = [&] {
		return SwapchainExtent {
		    std::clamp(std::max(window.width, 1u), std::max(min_extent.width, 1u),
		               std::max(max_extent.width, 1u)),
		    std::clamp(std::max(window.height, 1u), std::max(min_extent.height, 1u),
		               std::max(max_extent.height, 1u))};
	};
	if (current.width == UINT32_MAX || current.width == 0u || current.height == 0u) {
		return choose_from_window();
	}
	return current;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_SWAPCHAINEXTENT_H_
