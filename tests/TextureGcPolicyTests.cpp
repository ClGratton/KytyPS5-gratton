#include "graphics/host_gpu/renderer/cache/textureGcPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Libs::Graphics::TextureGcPolicy;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "TextureGcPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

Candidate CpuOnly() { return {}; }
Candidate GpuClean(bool tiled) {
	Candidate c;
	c.gpu_modified = true;
	c.tiled        = tiled;
	return c;
}

void TestClassify() {
	Check(Classify(CpuOnly(), false, false) == Action::Free, "an image only the CPU wrote is freed");
	Candidate unregistered;
	unregistered.registered = false;
	Check(Classify(unregistered, true, true) == Action::Keep, "an unregistered image is skipped");
	Candidate stencil;
	stencil.has_depth_owner = true;
	Check(Classify(stencil, true, true) == Action::Keep, "a stencil association is skipped");

	// Clean GPU-written images: tiled ones are kept, linear ones are downloaded under pressure.
	Check(Classify(GpuClean(true), true, true) == Action::Keep, "tiled and downloadable: kept");
	Check(Classify(GpuClean(false), false, false) == Action::Keep, "linear, no pressure: kept");
	Check(Classify(GpuClean(false), true, false) == Action::DownloadThenFree,
	      "linear under pressure: download then free");

	// Guest memory is newer than the image: nothing to lose.
	Candidate buffer = GpuClean(true);
	buffer.buffer_modified = true;
	Check(Classify(buffer, false, false) == Action::Free, "buffer-modified: freed");
	Candidate cpu = GpuClean(true);
	cpu.cpu_dirty = true;
	Check(Classify(cpu, false, false) == Action::Free, "definitely CPU-dirty: freed");

	// Regression (desert colour): an image the GPU wrote that is only "maybe" CPU-dirty used to
	// fall through to the free; its contents may be the only valid copy.
	Candidate maybe = GpuClean(false);
	maybe.maybe_cpu_dirty = true;
	Check(Classify(maybe, false, false) == Action::Keep, "maybe dirty, idle: kept");
	Check(Classify(maybe, true, false) == Action::Keep, "maybe dirty, pressured: kept");
	Check(Classify(maybe, true, true) == Action::Free, "maybe dirty, critical pressure: freed");
	Candidate maybe_tiled = maybe;
	maybe_tiled.tiled = true;
	Check(Classify(maybe_tiled, true, false) == Action::Keep, "maybe dirty, tiled: kept");
}

struct Item {
	int       id;
	Candidate c;
};

void TestKeptImagesDoNotUseTheBudget() {
	// The reported starvation: 58 tiled kept images and 2 depth images at the front of the LRU
	// list, then freeable ones. Budget 10 must still free 10.
	std::vector<Item> items;
	for (int i = 0; i < 58; ++i) {
		items.push_back({i, GpuClean(true)});
	}
	for (int i = 58; i < 60; ++i) {
		Candidate depth;
		depth.has_depth_owner = true;
		items.push_back({i, depth});
	}
	for (int i = 60; i < 80; ++i) {
		items.push_back({i, CpuOnly()});
	}
	std::vector<int> ids;
	for (const auto& item: items) {
		ids.push_back(item.id);
	}
	size_t             budget = 10;
	std::vector<int>   freed_ids;
	const size_t       freed = Walk(
        ids, budget, [&](int id) { return Classify(items[id].c, false, false); },
        [&](int id, Action) {
			freed_ids.push_back(id);
			return true;
		});
	Check(freed == 10 && freed_ids.size() == 10, "10 freed despite 60 kept images in front");
	Check(freed_ids.front() == 60 && freed_ids.back() == 69, "oldest freeable first, in order");
	Check(budget == 0, "budget used up");
}

void TestFailedDownloadReturnsTheBudget() {
	std::vector<int> ids {0, 1, 2, 3};
	size_t           budget = 2;
	int              runs   = 0;
	const size_t     freed  = Walk(
        ids, budget, [](int) { return Action::DownloadThenFree; },
        [&](int id, Action) {
			++runs;
			return id >= 2; // the first two downloads fail
		});
	Check(freed == 2 && runs == 4, "failed downloads do not use the budget");
	Check(budget == 0, "budget used by the two real frees");
}

void TestExecuteMayLowerTheBudget() {
	std::vector<int> ids;
	for (int i = 0; i < 20; ++i) {
		ids.push_back(i);
	}
	size_t       budget = 8;
	const size_t freed  = Walk(
        ids, budget, [](int) { return Action::Free; },
        [&](int, Action) {
			budget >>= 1; // memory dropped below a threshold: the collector halves what is left
			return true;
		});
	Check(freed == 3, "budget 8: reserve 1 then halve 7 -> 3, reserve -> 2 -> 1, reserve -> 0");
}

void TestNothingToFree() {
	std::vector<Item> items;
	for (int i = 0; i < 30; ++i) {
		items.push_back({i, GpuClean(true)});
	}
	std::vector<int> ids;
	for (const auto& item: items) {
		ids.push_back(item.id);
	}
	size_t budget = 10;
	const size_t freed = Walk(
	    ids, budget, [&](int id) { return Classify(items[id].c, true, true); },
	    [](int, Action) { return true; });
	Check(freed == 0 && budget == 10, "all kept: nothing freed, budget untouched");
}

} // namespace

int main() {
	TestClassify();
	TestKeptImagesDoNotUseTheBudget();
	TestFailedDownloadReturnsTheBudget();
	TestExecuteMayLowerTheBudget();
	TestNothingToFree();
	std::printf("TextureGcPolicyTests: all passed\n");
	return 0;
}
