#include "graphics/host_gpu/renderer/cache/readbackPlan.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::PlanReadback;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ReadbackPlanTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFinishedProducerNeedsNoFlush() {
	const auto plan = PlanReadback(40, 0, 50);
	Check(plan.wait_tick == 40 && !plan.flush_first, "a submitted producer: wait for it, no flush");
}

void TestOpenProducerFlushes() {
	const auto plan = PlanReadback(50, 0, 50);
	Check(plan.wait_tick == 50 && plan.flush_first, "a producer in the open buffer is submitted first");
}

void TestAddressWritesRaiseTheTick() {
	auto plan = PlanReadback(10, 45, 50);
	Check(plan.wait_tick == 45 && !plan.flush_first, "a later raw-address writer is waited for");
	plan = PlanReadback(10, 50, 50);
	Check(plan.wait_tick == 50 && plan.flush_first, "a raw-address writer in the open buffer flushes");
}

void TestNeverWrittenWaitsForNothing() {
	const auto plan = PlanReadback(0, 0, 50);
	Check(plan.wait_tick == 0 && !plan.flush_first, "tick 0 is already signaled");
}

void TestStampFromTheFutureIsClamped() {
	const auto plan = PlanReadback(70, 0, 50);
	Check(plan.wait_tick == 50 && plan.flush_first, "never wait for a tick that cannot be signaled");
}

} // namespace

int main() {
	TestFinishedProducerNeedsNoFlush();
	TestOpenProducerFlushes();
	TestAddressWritesRaiseTheTick();
	TestNeverWrittenWaitsForNothing();
	TestStampFromTheFutureIsClamped();
	std::printf("ReadbackPlanTests: ok\n");
	return 0;
}
