#include "graphics/host_gpu/renderer/image/transitNoop.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

// TransitIsNoop against a copy of Image::GetBarriers' decision (image.cpp): on random image states and random
// destinations, whenever the predicate says "nothing to do", GetBarriers must record no barrier and leave
// the image-wide state and the per-subresource states exactly as they were. The reference keeps the structure
// of the original on purpose, so that a change of GetBarriers that breaks the equivalence is seen here.

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "TransitNoopTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint32_t kTransferWrite = 1u << 0;
constexpr uint32_t kShaderWrite   = 1u << 1;
constexpr uint32_t kMemoryWrite   = 1u << 2;
constexpr uint32_t kShaderRead    = 1u << 3;
constexpr uint32_t kTransferRead  = 1u << 4;
constexpr uint32_t kWriteAccess   = kTransferWrite | kShaderWrite | kMemoryWrite;

struct State {
	uint32_t pl_stage    = 0;
	uint32_t access_mask = 0;
	int      layout      = 0;
	bool operator==(const State& o) const {
		return pl_stage == o.pl_stage && access_mask == o.access_mask && layout == o.layout;
	}
};

struct Model {
	State              state;
	std::vector<State> subresource_states;
	uint32_t           levels    = 1;
	uint32_t           layers    = 1;
	bool               is_volume = false;
};

struct Range {
	uint32_t base_level, level_count, base_layer, layer_count;
};

// Image::GetBarriers, with the barrier replaced by a counter.
size_t GetBarriers(Model& image, int destination_layout, uint32_t destination_access, uint32_t destination_stage,
                   const Range* range_in) {
	auto& state              = image.state;
	auto& subresource_states = image.subresource_states;
	Range range {};
	if (range_in) range = *range_in;
	if (range_in && image.is_volume) {
		range.base_layer  = 0;
		range.layer_count = 1;
	}
	const bool partial = range_in && (range.base_level != 0 || range.level_count != image.levels ||
	                                  range.base_layer != 0 || range.layer_count != image.layers);
	const bool has_subresource_states = !subresource_states.empty();
	size_t     barriers               = 0;
	if (partial || has_subresource_states) {
		if (!has_subresource_states) subresource_states.resize(image.levels * image.layers, state);
		const uint32_t base_level  = partial ? range.base_level : 0;
		const uint32_t level_count = partial ? range.level_count : image.levels;
		const uint32_t base_layer  = partial ? range.base_layer : 0;
		const uint32_t layer_count = partial ? range.layer_count : image.layers;
		for (uint32_t level = base_level; level < base_level + level_count; level++) {
			for (uint32_t layer = base_layer; layer < base_layer + layer_count; layer++) {
				const auto index = level * image.layers + layer;
				Check(index < subresource_states.size(), "model: subresource index in range");
				auto&      s              = subresource_states[index];
				const bool repeated_write = (s.access_mask & kWriteAccess) != 0;
				if (s.layout != destination_layout || s.access_mask != destination_access || repeated_write) {
					barriers++;
					s = {destination_stage, destination_access, destination_layout};
				}
			}
		}
		if (!partial) subresource_states.clear();
	} else {
		const bool repeated_write = (state.access_mask & kWriteAccess) != 0;
		if (state.layout == destination_layout && state.access_mask == destination_access && !repeated_write) {
			return 0;
		}
		barriers++;
	}
	state = {destination_stage, destination_access, destination_layout};
	return barriers;
}

bool Predicate(const Model& image, int layout, uint32_t access, const Range* range) {
	TransitRange r;
	if (range) r = {range->base_level, range->level_count, range->base_layer, range->layer_count};
	return TransitIsNoop<State, uint32_t>(image.state, image.subresource_states, image.levels, image.layers,
	                                      image.is_volume, range ? &r : nullptr, layout, access, kWriteAccess);
}

void TestPlainCases() {
	Model image;
	image.levels = 4;
	image.layers = 6;
	image.state  = {7, kShaderRead, 5};
	Check(Predicate(image, 5, kShaderRead, nullptr), "same layout and read access, whole image: no-op");
	const Range whole {0, 4, 0, 6};
	Check(Predicate(image, 5, kShaderRead, &whole), "an explicit whole-image range is still whole");
	const Range part {1, 1, 0, 6};
	Check(!Predicate(image, 5, kShaderRead, &part), "a sub-range creates subresource states: not a no-op");
	Check(!Predicate(image, 6, kShaderRead, nullptr), "another layout needs a barrier");
	Check(!Predicate(image, 5, kShaderRead | kTransferRead, nullptr), "another access needs a barrier");
	image.state = {7, kShaderWrite, 5};
	Check(!Predicate(image, 5, kShaderWrite, nullptr), "a write after a write needs a barrier");
	image.state = {7, kShaderRead, 5};
	image.subresource_states.assign(24, image.state);
	Check(!Predicate(image, 5, kShaderRead, nullptr), "per-subresource states are cleared by a whole-image access");
	// A volume has one layer for the purposes of the range.
	Model volume;
	volume.levels    = 3;
	volume.layers    = 1;
	volume.is_volume = true;
	volume.state     = {1, kShaderRead, 2};
	const Range volume_range {0, 3, 4, 9};
	Check(Predicate(volume, 2, kShaderRead, &volume_range), "a volume's layer range is ignored");
	volume.is_volume = false;
	Check(!Predicate(volume, 2, kShaderRead, &volume_range), "an array's layer range is not");
}

void TestRandomAgainstGetBarriers() {
	std::mt19937_64 rng(0x5eed1234);
	size_t          noops = 0, calls = 0;
	for (int trial = 0; trial < 200000; ++trial) {
		Model image;
		image.levels    = 1 + static_cast<uint32_t>(rng() % 4);
		image.layers    = 1 + static_cast<uint32_t>(rng() % 4);
		image.is_volume = rng() % 7 == 0;
		if (image.is_volume) image.layers = 1;
		const uint32_t accesses[] = {kShaderRead, kShaderRead | kShaderWrite, kTransferRead, kTransferWrite,
		                             kMemoryWrite, 0, kShaderRead | kTransferRead};
		auto           pick       = [&]() {
            return State {static_cast<uint32_t>(1 + rng() % 3), accesses[rng() % 7], static_cast<int>(rng() % 3)};
		};
		image.state = pick();
		if (rng() % 4 == 0) {
			image.subresource_states.resize(image.levels * image.layers);
			for (auto& s: image.subresource_states) s = (rng() % 2) ? image.state : pick();
		}
		Range range {};
		bool  has_range = rng() % 2;
		if (has_range) {
			if (rng() % 2) {
				range = {0, image.levels, 0, image.layers};
			} else {
				range.base_level  = static_cast<uint32_t>(rng() % image.levels);
				range.level_count = 1 + static_cast<uint32_t>(rng() % (image.levels - range.base_level));
				range.base_layer  = static_cast<uint32_t>(rng() % image.layers);
				range.layer_count = 1 + static_cast<uint32_t>(rng() % (image.layers - range.base_layer));
			}
		}
		// Half of the destinations repeat the image's own layout and access (a texture sampled by the next
		// draw), the case the predicate exists for; the rest are random so that the mismatches are covered.
		const bool     same   = rng() % 2 == 0;
		const int      layout = same ? image.state.layout : static_cast<int>(rng() % 3);
		const uint32_t access = same ? image.state.access_mask : accesses[rng() % 7];
		const bool     noop   = Predicate(image, layout, access, has_range ? &range : nullptr);
		Model          after  = image;
		const auto     count  = GetBarriers(after, layout, access, 9, has_range ? &range : nullptr);
		calls++;
		if (noop) {
			noops++;
			Check(count == 0, "a predicted no-op records no barrier");
			Check(after.state == image.state, "a predicted no-op leaves the image state alone");
			Check(after.subresource_states.size() == image.subresource_states.size(),
			      "a predicted no-op leaves the subresource states alone (size)");
			for (size_t i = 0; i < image.subresource_states.size(); ++i) {
				Check(after.subresource_states[i] == image.subresource_states[i],
				      "a predicted no-op leaves the subresource states alone");
			}
		}
		// The plain path (nothing partial, no subresource states) is decided exactly: no barrier means no-op.
		const bool partial = has_range && !(range.base_level == 0 && range.level_count == image.levels &&
		                                    (image.is_volume || (range.base_layer == 0 && range.layer_count == image.layers)));
		if (!partial && image.subresource_states.empty()) {
			Check((count == 0) == noop, "on the plain path the predicate is exact");
		}
	}
	Check(noops > 5000 && noops < calls, "the generator exercises both outcomes");
}

} // namespace

int main() {
	TestPlainCases();
	TestRandomAgainstGetBarriers();
	std::puts("TransitNoopTests: all passed");
	return 0;
}
