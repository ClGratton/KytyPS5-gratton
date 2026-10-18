#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TRANSITNOOP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TRANSITNOOP_H_

// --desc-commit-diet: CommitBindings transits every image a draw binds, and most of them are already
// in the layout and access the draw wants (a texture sampled by one draw is sampled by the next).
// Image::Transit then builds the destination stage, calls GetBarriers, finds nothing to do and
// returns. This is the decision "Image::GetBarriers returns no barrier AND changes no state" as a pure
// function of the image's recorded state, so the caller can leave the call out.
//
// Only the plain case qualifies: the whole image is addressed (no sub-range, or one that covers every
// level and layer), no per-subresource states exist, and the image-wide state already has the
// destination layout and access with no write bit in it (a write needs a barrier before the next
// access, even to the same layout). Everything else keeps the full call: a sub-range creates or walks
// the per-subresource states and GetBarriers overwrites the image-wide state afterwards, a
// whole-image access with per-subresource states clears them. Exactness is the point: the unit test
// (tests/TransitNoopTests.cpp) runs this against a copy of GetBarriers' decision on random states.
//
// Pure data, no Vulkan types, so that the test does not need a device.

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct TransitRange {
	uint32_t base_level  = 0;
	uint32_t level_count = 1;
	uint32_t base_layer  = 0;
	uint32_t layer_count = 1;
};

// State: has `layout` and `access_mask`. Layout/Access: comparable; Access also supports `&` that
// converts to bool (a flag type or an integer).
template <class State, class Access>
[[nodiscard]] inline bool TransitIsNoop(const State& state, const std::vector<State>& subresource_states,
                                        uint32_t levels, uint32_t layers, bool is_volume,
                                        const TransitRange* range, decltype(State::layout) layout,
                                        Access access, Access write_access) {
	if (!subresource_states.empty()) return false;
	if (range != nullptr) {
		TransitRange r = *range;
		if (is_volume) {
			r.base_layer  = 0;
			r.layer_count = 1;
		}
		const bool partial = r.base_level != 0 || r.level_count != levels || r.base_layer != 0 ||
		                     r.layer_count != layers;
		if (partial) return false;
	}
	return state.layout == layout && state.access_mask == access && !static_cast<bool>(state.access_mask & write_access);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TRANSITNOOP_H_
