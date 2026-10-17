#include "graphics/host_gpu/renderer/pipeline/pushDescriptorCache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

} // namespace

int main() {
	TestUnchangedBindingsAreLeftOut();
	TestDisturbance();
	TestForgottenBinding();
	TestStats();
	std::printf("PushDescriptorCacheTests: ok\n");
	return 0;
}
