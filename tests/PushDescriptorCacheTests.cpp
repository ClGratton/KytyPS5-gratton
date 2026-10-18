#include "graphics/host_gpu/renderer/pipeline/pushDescriptorCache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using Libs::Graphics::PushDescriptorCache;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PushDescriptorCacheTests: failed: %s\n", message);
		std::abort();
	}
}

using Words = std::vector<uint64_t>;

void TestUnchangedBindingsAreLeftOut() {
	PushDescriptorCache cache;
	const Words         a {1, 10, 0, 16};
	const Words         b {2, 20, 5, 7};
	cache.Begin(100);
	Check(cache.Changed(0, a), "the first write of a binding is pushed");
	Check(cache.Changed(3, b), "the first write of another binding is pushed");
	cache.Begin(100);
	Check(!cache.Changed(0, a), "an identical write is left out");
	Check(!cache.Changed(3, b), "an identical write of the other binding is left out");
	const Words b2 {2, 20, 6, 7};
	cache.Begin(100);
	Check(!cache.Changed(0, a), "binding 0 still unchanged");
	Check(cache.Changed(3, b2), "a changed binding is pushed");
	cache.Begin(100);
	Check(!cache.Changed(3, b2), "and then remembered");
	const Words shorter {2, 20};
	Check(cache.Changed(3, shorter), "a different length is a change");
}

void TestDisturbance() {
	PushDescriptorCache cache;
	const Words         a {1, 1, 1, 1};
	cache.Begin(100);
	Check(cache.Changed(1, a), "pushed");
	cache.Begin(100);
	Check(!cache.Changed(1, a), "held");
	cache.Begin(200);
	Check(cache.Changed(1, a), "another pipeline layout discards the content");
	cache.Begin(200);
	Check(!cache.Changed(1, a), "held at the new layout");
	cache.Begin(100);
	Check(cache.Changed(1, a), "coming back to the first layout does not bring its content back");
	cache.Invalidate();
	cache.Begin(100);
	Check(cache.Changed(1, a), "an invalidated set holds nothing");
	cache.Begin(100);
	Check(!cache.Changed(1, a), "held again");
}

void TestForgottenBinding() {
	PushDescriptorCache cache;
	const Words         a {1, 1};
	cache.Begin(7);
	Check(cache.Changed(2, a), "pushed");
	cache.Forget(2);
	cache.Begin(7);
	Check(cache.Changed(2, a), "a binding pushed without content is not compared against older content");
}

void TestStats() {
	PushDescriptorCache cache;
	const Words         a {1};
	cache.Begin(1);
	(void)cache.Changed(0, a);
	cache.Begin(1);
	(void)cache.Changed(0, a);
	cache.NoteElided();
	const auto& s = cache.GetStats();
	Check(s.pushes == 2 && s.writes_seen == 2 && s.writes_pushed == 1 && s.pushes_elided == 1, "the counters add up");
}

// BeginUpdate / Put / EndUpdate is Changed() with the words handed over one at a time: the same decisions, the same
// counters, and the same content kept (--desc-commit-diet builds the words in place instead of in a vector).
void TestUpdateMatchesChanged() {
	std::mt19937_64    rng(0xdecaf);
	PushDescriptorCache by_vector;
	PushDescriptorCache by_update;
	uint64_t            layout = 1;
	by_vector.Begin(layout);
	by_update.Begin(layout);
	size_t changed_count = 0, unchanged_count = 0;
	for (int step = 0; step < 100000; ++step) {
		const auto roll = rng() % 40;
		if (roll == 0) {
			layout = 1 + rng() % 3;
			by_vector.Begin(layout);
			by_update.Begin(layout);
		} else if (roll == 1) {
			by_vector.Invalidate();
			by_update.Invalidate();
		} else if (roll == 2) {
			const auto binding = static_cast<uint32_t>(rng() % 6);
			by_vector.Forget(binding);
			by_update.Forget(binding);
		} else {
			const auto binding = static_cast<uint32_t>(rng() % 6);
			Words      words(1 + rng() % 4);
			for (auto& w: words) w = rng() % 3; // few distinct values: many repeats
			const bool expected = by_vector.Changed(binding, words);
			auto       update   = by_update.BeginUpdate(binding, words.size());
			for (const auto w: words) update.Put(w);
			const bool actual = by_update.EndUpdate(update);
			Check(expected == actual, "an update decides like Changed on the same words");
			(expected ? changed_count : unchanged_count)++;
		}
		const auto& a = by_vector.GetStats();
		const auto& b = by_update.GetStats();
		Check(a.writes_seen == b.writes_seen && a.writes_pushed == b.writes_pushed && a.pushes == b.pushes &&
		          a.disturbed == b.disturbed,
		      "the counters of the two ways agree");
	}
	Check(changed_count > 1000 && unchanged_count > 1000, "the generator exercises both outcomes");
}

} // namespace

int main() {
	TestUpdateMatchesChanged();
	TestUnchangedBindingsAreLeftOut();
	TestDisturbance();
	TestForgottenBinding();
	TestStats();
	std::printf("PushDescriptorCacheTests: ok\n");
	return 0;
}
