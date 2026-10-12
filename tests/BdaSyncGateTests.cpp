#include "graphics/host_gpu/bdaSyncGate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using namespace Libs::Graphics::BdaSync;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "BdaSyncGateTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFirstPassAlwaysRuns() {
	SkipGate gate;
	Check(!gate.CanSkip(true, {}), "no full pass yet: cannot skip");
	Check(!gate.CanSkip(true, {5, 5, 5}), "no full pass yet, any generations");
}

void TestSkipsWhenNothingChanged() {
	SkipGate         gate;
	const Generations g {3, 7, 2};
	gate.RecordFullPass(g);
	Check(gate.CanSkip(true, g), "same generations: skip");
	Check(gate.CanSkip(true, {3, 7, 2}), "equal value, not identity");
}

void TestAnyGenerationChangeForcesAPass() {
	SkipGate gate;
	gate.RecordFullPass({3, 7, 2});
	Check(!gate.CanSkip(true, {4, 7, 2}), "pages became CPU-modified");
	Check(!gate.CanSkip(true, {3, 8, 2}), "a buffer was registered or unregistered");
	Check(!gate.CanSkip(true, {3, 7, 3}), "the mapped ranges changed");
}

void TestDisabledNeverSkips() {
	SkipGate gate;
	gate.RecordFullPass({1, 1, 1});
	Check(!gate.CanSkip(false, {1, 1, 1}), "flag off: stock behaviour, always synchronize");
}

void TestChangeDuringPassIsNotLost() {
	// The generations are read before the pass; a change that lands during the pass makes the
	// next call see different generations and synchronize again.
	SkipGate         gate;
	const Generations before {10, 4, 1};
	gate.RecordFullPass(before);
	const Generations after_concurrent_write {11, 4, 1};
	Check(!gate.CanSkip(true, after_concurrent_write), "write during the pass is picked up");
	gate.RecordFullPass(after_concurrent_write);
	Check(gate.CanSkip(true, after_concurrent_write), "and then settles");
}

void TestInvalidate() {
	SkipGate gate;
	gate.RecordFullPass({1, 2, 3});
	gate.Invalidate();
	Check(!gate.CanSkip(true, {1, 2, 3}), "invalidated gate forces a pass");
}

void TestCpuDirtyCounterIsMonotonicAcrossThreads() {
	const auto start = g_cpu_dirty_generation.load();
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([] {
			for (int i = 0; i < 1000; ++i) {
				NoteCpuModified();
			}
		});
	}
	for (auto& thread: threads) {
		thread.join();
	}
	Check(g_cpu_dirty_generation.load() == start + 4000, "no bump lost");
}

} // namespace

int main() {
	TestFirstPassAlwaysRuns();
	TestSkipsWhenNothingChanged();
	TestAnyGenerationChangeForcesAPass();
	TestDisabledNeverSkips();
	TestChangeDuringPassIsNotLost();
	TestInvalidate();
	TestCpuDirtyCounterIsMonotonicAcrossThreads();
	std::printf("BdaSyncGateTests: all passed\n");
	return 0;
}
