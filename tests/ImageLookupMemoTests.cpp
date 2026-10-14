#include "graphics/host_gpu/renderer/cache/imageLookupMemo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <tuple>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ImageLookupMemoTests: failed: %s\n", message);
		std::abort();
	}
}

ImageInfo MakeInfo(uint64_t address) {
	ImageInfo info;
	info.data            = {address, 0x40000};
	info.extent          = {512, 256, 1};
	info.resources       = {5, 1};
	info.samples         = 1;
	info.bytes_per_block = 4;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.pixel_format    = vk::Format::eR8G8B8A8Unorm;
	info.guest_format    = Prospero::BufferFormat::k8_8_8_8UNorm;
	info.type            = Prospero::ImageType::kColor2D;
	return info;
}

void TestRememberAndGeneration() {
	ImageLookupMemo memo;
	const auto      key = ImageLookupKey::Make(MakeInfo(0x1000000), false, 0);
	Common::SlotId  id;
	Check(!memo.Find(key, 1, id), "empty memo misses");
	memo.Record(key, 1, Common::SlotId {7, 3});
	Check(memo.Find(key, 1, id) && id == Common::SlotId({7, 3}), "a recorded key hits with its id");
	Check(!memo.Find(key, 2, id), "a newer generation misses");
	Check(!memo.Find(key, 0, id), "generation 0 never hits");
	memo.Record(key, 2, Common::SlotId {9, 1});
	Check(memo.Find(key, 2, id) && id == Common::SlotId({9, 1}), "re-recording under the new generation hits");
	Check(!memo.Find(key, 1, id), "the old generation no longer hits");
	memo.Clear();
	Check(!memo.Find(key, 2, id), "Clear drops everything");
}

// Every field the lookup reads is part of the key: changing any single one is a different request.
void TestEveryFieldIsInTheKey() {
	const auto base_info = MakeInfo(0x2000000);
	const auto base      = ImageLookupKey::Make(base_info, false, 1);
	Check(base == ImageLookupKey::Make(base_info, false, 1), "same request, same key");
	Check(base.Hash() == ImageLookupKey::Make(base_info, false, 1).Hash(), "same request, same hash");

	const auto differs = [&](ImageInfo info, bool exact, uint8_t binding, const char* what) {
		const auto other = ImageLookupKey::Make(info, exact, binding);
		if (other == base) {
			std::fprintf(stderr, "ImageLookupMemoTests: field not in key: %s\n", what);
			std::abort();
		}
	};
	{ auto i = base_info; i.data.address += 0x4000; differs(i, false, 1, "address"); }
	{ auto i = base_info; i.data.size += 0x4000; differs(i, false, 1, "size"); }
	{ auto i = base_info; i.extent.width += 1; differs(i, false, 1, "width"); }
	{ auto i = base_info; i.extent.height += 1; differs(i, false, 1, "height"); }
	{ auto i = base_info; i.extent.depth += 1; differs(i, false, 1, "depth"); }
	{ auto i = base_info; i.resources.levels += 1; differs(i, false, 1, "levels"); }
	{ auto i = base_info; i.resources.layers += 1; differs(i, false, 1, "layers"); }
	{ auto i = base_info; i.samples = 4; differs(i, false, 1, "samples"); }
	{ auto i = base_info; i.bytes_per_block = 8; differs(i, false, 1, "bytes_per_block"); }
	{ auto i = base_info; i.tile_mode = Prospero::TileMode::kDepth; differs(i, false, 1, "tile_mode"); }
	{ auto i = base_info; i.pixel_format = vk::Format::eB8G8R8A8Unorm; differs(i, false, 1, "pixel_format"); }
	{ auto i = base_info; i.guest_format = Prospero::BufferFormat::k8UInt; differs(i, false, 1, "guest_format"); }
	{ auto i = base_info; i.type = Prospero::ImageType::kColor2DArray; differs(i, false, 1, "type"); }
	{ auto i = base_info; i.bgra16 = true; differs(i, false, 1, "bgra16"); }
	differs(base_info, true, 1, "exact_format");
	differs(base_info, false, 2, "binding_type");
}

// Soundness against a reference: a hit only ever returns what was recorded for that exact key under
// that exact generation, whatever collides in the direct-mapped table.
void TestSoundnessAgainstReference() {
	std::mt19937_64 rng(12345);
	ImageLookupMemo memo;
	using RefKey = std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, bool, uint8_t>;
	std::map<RefKey, std::pair<uint64_t, Common::SlotId>> reference;
	uint64_t generation = 1;
	uint64_t hits       = 0;
	for (int step = 0; step < 400000; ++step) {
		const auto op = rng() % 100;
		if (op < 2) {
			++generation; // an image was registered or unregistered
			continue;
		}
		auto info = MakeInfo(0x1000000 + (rng() % 40) * 0x4000);
		info.extent.width = 64 + static_cast<uint32_t>(rng() % 2) * 64;
		const bool    exact   = (rng() & 1) != 0;
		const uint8_t binding = static_cast<uint8_t>(rng() % 3);
		const auto    key     = ImageLookupKey::Make(info, exact, binding);
		const RefKey  ref {info.data.address, info.data.size, info.extent.width, info.extent.height, exact, binding};
		Common::SlotId id;
		if (memo.Find(key, generation, id)) {
			++hits;
			const auto it = reference.find(ref);
			Check(it != reference.end(), "a hit for a key never recorded");
			Check(it->second.first == generation, "a hit under a generation the key was not recorded in");
			Check(it->second.second == id, "a hit returned another key's id");
		} else {
			const Common::SlotId made {static_cast<uint32_t>(rng() % 100000), 1};
			memo.Record(key, generation, made);
			reference[ref] = {generation, made};
		}
	}
	Check(hits > 1000, "the memo hits on repeated requests");
	Check(memo.GetStats().hits == hits, "hit counter");
}

} // namespace

int main() {
	TestRememberAndGeneration();
	TestEveryFieldIsInTheKey();
	TestSoundnessAgainstReference();
	std::printf("ImageLookupMemoTests: ok\n");
	return 0;
}
