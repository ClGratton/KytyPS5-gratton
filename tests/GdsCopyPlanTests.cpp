#include "graphics/host_gpu/renderer/cache/gdsCopyPlan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Libs::Graphics::GdsCopyChunk;
using Libs::Graphics::PlanGdsCopy;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GdsCopyPlanTests: failed: %s\n", message);
		std::abort();
	}
}

// Executes the plan with non-overlapping copies only (as vkCmdCopyBuffer requires) and compares
// with memmove over a pattern buffer.
void CheckAgainstMemmove(uint64_t destination, uint64_t source, uint64_t size) {
	std::vector<uint8_t> expected(512);
	for (size_t i = 0; i < expected.size(); i++) {
		expected[i] = static_cast<uint8_t>(i * 7u + 1u);
	}
	auto actual = expected;
	std::memmove(expected.data() + destination, expected.data() + source, size);
	for (const auto& chunk: PlanGdsCopy(destination, source, size)) {
		Check(chunk.size != 0, "empty chunk");
		Check(chunk.source + chunk.size <= actual.size() &&
		          chunk.destination + chunk.size <= actual.size(),
		      "chunk runs past the buffer");
		Check(chunk.source + chunk.size <= chunk.destination ||
		          chunk.destination + chunk.size <= chunk.source,
		      "a chunk overlaps its own destination");
		std::memcpy(actual.data() + chunk.destination, actual.data() + chunk.source, chunk.size);
	}
	Check(actual == expected, "planned copy differs from memmove");
}

void TestDisjointIsOneChunk() {
	const auto plan = PlanGdsCopy(64, 0, 32);
	Check(plan.size() == 1u && plan[0].source == 0u && plan[0].destination == 64u &&
	          plan[0].size == 32u,
	      "disjoint ranges must be one copy");
	CheckAgainstMemmove(64, 0, 32);
	CheckAgainstMemmove(0, 64, 32);
	CheckAgainstMemmove(32, 0, 32); // adjacent
}

void TestOverlapMatchesMemmove() {
	for (uint64_t size = 1; size <= 96; size += 5) {
		for (uint64_t distance = 1; distance <= 96; distance += 3) {
			CheckAgainstMemmove(100 + distance, 100, size);
			CheckAgainstMemmove(100, 100 + distance, size);
		}
	}
}

void TestDegenerate() {
	Check(PlanGdsCopy(8, 8, 64).empty(), "copying onto itself must plan nothing");
	Check(PlanGdsCopy(0, 4, 0).empty(), "zero bytes must plan nothing");
}

} // namespace

int main() {
	TestDisjointIsOneChunk();
	TestOverlapMatchesMemmove();
	TestDegenerate();
	std::puts("GdsCopyPlanTests: all cases passed");
	return 0;
}
