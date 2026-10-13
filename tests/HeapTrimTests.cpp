#include "common/heapTrim.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Common::HeapTrim;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HeapTrimTests: failed: %s\n", message);
		std::abort();
	}
}

void TestShouldTrim() {
	const Policy policy;
	// 421 MiB free in a 876 MiB heap (the level-load state that was measured): trim.
	Check(ShouldTrim({876 * MiB, 421 * MiB}, policy, 10000), "large free share trims");
	// Not enough free in absolute terms, however large the share.
	Check(!ShouldTrim({100 * MiB, 60 * MiB}, policy, 10000), "below the absolute floor");
	// Plenty free but a small share of a big heap: the free chunks are scattered, leave them.
	Check(!ShouldTrim({4096 * MiB, 300 * MiB}, policy, 10000), "below the share floor");
	Check(ShouldTrim({4096 * MiB, 820 * MiB}, policy, 10000), "exactly over the share floor");
	// Rate limit.
	Check(!ShouldTrim({876 * MiB, 421 * MiB}, policy, 4999), "too soon after the last trim");
	Check(ShouldTrim({876 * MiB, 421 * MiB}, policy, 5000), "interval elapsed");
	// Degenerate heap.
	Check(!ShouldTrim({0, 0}, policy, 10000), "empty heap");
	Check(!ShouldTrim({0, 100 * MiB}, policy, 10000), "no arenas reported");
	// Thresholds are the policy's, not constants.
	Policy low;
	low.min_free_bytes   = 8 * MiB;
	low.min_free_percent = 50;
	low.min_interval_ms  = 0;
	Check(ShouldTrim({16 * MiB, 8 * MiB}, low, 0), "custom policy trims a small heap");
	Check(!ShouldTrim({16 * MiB, 7 * MiB}, low, 0), "custom floor");
}

} // namespace

int main() {
	TestShouldTrim();
	std::printf("HeapTrimTests: ok\n");
	return 0;
}
