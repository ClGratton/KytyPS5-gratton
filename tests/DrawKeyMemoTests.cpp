#include "graphics/host_gpu/renderer/pipeline/drawKeyMemo.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DrawKeyMemoTests: failed: %s\n", message);
		std::abort();
	}
}

// Stand-in for the pipeline key: several scalar fields plus arrays whose unused tail is zero.
struct FakeKey {
	uint64_t                vertex_id = 0;
	uint64_t                pixel_id  = 0;
	uint32_t                color_count = 0;
	std::array<uint32_t, 8> formats {};
	std::array<uint32_t, 32> attributes {};
	uint8_t                 attribute_count = 0;
	bool                    blend           = false;

	bool operator==(const FakeKey&) const = default;
};

FakeKey MakeKey(uint64_t vs, uint64_t ps) {
	FakeKey key;
	key.vertex_id   = vs;
	key.pixel_id    = ps;
	key.color_count = 1;
	key.formats[0]  = 37;
	return key;
}

int g_pipelines[64]; // the "pipeline nodes": addresses are the values

void TestEmptyMissesAndRemembers() {
	DrawKeyMemo<FakeKey, int*> memo;
	const auto                 key = MakeKey(1, 2);
	Check(memo.Find(key) == nullptr, "empty memo misses");
	memo.Remember(key, &g_pipelines[0]);
	auto* hit = memo.Find(key);
	Check(hit != nullptr && *hit == &g_pipelines[0], "remembered key hits and returns its pipeline");
	Check(memo.GetStats().hits == 1 && memo.GetStats().misses == 1, "counters");
}

void TestIdenticalStateReturnsIdenticalPipeline() {
	DrawKeyMemo<FakeKey, int*> memo;
	memo.Remember(MakeKey(1, 2), &g_pipelines[0]);
	memo.Remember(MakeKey(3, 4), &g_pipelines[1]);
	for (int i = 0; i < 100; ++i) {
		auto* a = memo.Find(MakeKey(1, 2)); // a separately built, equal key
		auto* b = memo.Find(MakeKey(3, 4));
		Check(a != nullptr && *a == &g_pipelines[0], "equal key, same pipeline (a)");
		Check(b != nullptr && *b == &g_pipelines[1], "equal key, same pipeline (b)");
	}
}

void TestAnyFieldChangeMisses() {
	DrawKeyMemo<FakeKey, int*> memo;
	const auto                 base = MakeKey(1, 2);
	memo.Remember(base, &g_pipelines[0]);
	auto vary = [&](auto&& mutate, const char* what) {
		FakeKey key = base;
		mutate(key);
		Check(!(key == base), "test bug: the mutation changed nothing");
		Check(memo.Find(key) == nullptr, what);
	};
	vary([](FakeKey& k) { k.vertex_id ^= 1; }, "vertex id");
	vary([](FakeKey& k) { k.pixel_id ^= 1; }, "pixel id");
	vary([](FakeKey& k) { k.color_count = 2; }, "color count");
	vary([](FakeKey& k) { k.formats[7] = 1; }, "last format slot");
	vary([](FakeKey& k) { k.attributes[31] = 1; }, "last attribute slot (beyond the used count)");
	vary([](FakeKey& k) { k.attribute_count = 1; }, "attribute count");
	vary([](FakeKey& k) { k.blend = true; }, "blend");
	Check(memo.Find(base) != nullptr, "the original still hits after the misses");
}

void TestEvictsLeastRecent() {
	DrawKeyMemo<FakeKey, int*, 3> memo;
	memo.Remember(MakeKey(1, 0), &g_pipelines[1]);
	memo.Remember(MakeKey(2, 0), &g_pipelines[2]);
	memo.Remember(MakeKey(3, 0), &g_pipelines[3]);
	Check(memo.Find(MakeKey(1, 0)) != nullptr, "touch 1: now 2 is the least recent");
	memo.Remember(MakeKey(4, 0), &g_pipelines[4]);
	Check(memo.Size() == 3, "bounded");
	Check(memo.Find(MakeKey(2, 0)) == nullptr, "least recent (2) was evicted");
	Check(memo.Find(MakeKey(1, 0)) != nullptr && memo.Find(MakeKey(3, 0)) != nullptr &&
	          memo.Find(MakeKey(4, 0)) != nullptr,
	      "the others stay");
}

void TestClear() {
	DrawKeyMemo<FakeKey, int*> memo;
	memo.Remember(MakeKey(1, 2), &g_pipelines[0]);
	memo.Clear();
	Check(memo.Size() == 0 && memo.Find(MakeKey(1, 2)) == nullptr, "cleared memo misses");
	memo.Remember(MakeKey(1, 2), &g_pipelines[0]);
	Check(memo.Find(MakeKey(1, 2)) != nullptr, "usable after Clear");
}

// The owner's contract: memo-then-map must return exactly what the map alone returns, for any
// stream of keys. Random stream over a small key space (hits, evictions, misses all happen).
void TestAgreesWithTheMapOnARandomStream() {
	std::map<std::array<uint64_t, 2>, int*> truth;
	DrawKeyMemo<FakeKey, int*, 4>           memo;
	std::mt19937                            rng(12345);
	uint64_t                                memo_hits = 0;
	for (int i = 0; i < 20000; ++i) {
		// Mostly the same few pipelines, sometimes a new one: like a draw stream.
		const bool     rare = rng() % 8 == 0;
		const uint64_t vs   = rare ? rng() % 40 : rng() % 3;
		const uint64_t ps   = rare ? rng() % 2 : 0;
		const auto     key = MakeKey(vs, ps);
		const auto     id  = std::array<uint64_t, 2> {vs, ps};
		int*           expected = nullptr;
		if (auto it = truth.find(id); it != truth.end()) {
			expected = it->second;
		}
		int* got = nullptr;
		if (auto* hit = memo.Find(key)) {
			got = *hit;
			++memo_hits;
		} else if (expected != nullptr) {
			got = expected; // the "map lookup"
			memo.Remember(key, got);
		} else {
			got = &g_pipelines[(vs * 2 + ps) % 64]; // a "creation"
			truth[id] = got;
			expected  = got;
			memo.Remember(key, got);
		}
		Check(got == expected, "memo and map agree");
	}
	Check(memo_hits > 10000, "a draw-like stream mostly hits the memo");
}

} // namespace

int main() {
	TestEmptyMissesAndRemembers();
	TestIdenticalStateReturnsIdenticalPipeline();
	TestAnyFieldChangeMisses();
	TestEvictsLeastRecent();
	TestClear();
	TestAgreesWithTheMapOnARandomStream();
	std::puts("DrawKeyMemoTests: ok");
	return 0;
}
