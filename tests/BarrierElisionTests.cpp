#include "graphics/guest_gpu/command_processor/barrierElision.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::GlobalBarrierElider;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "BarrierElisionTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFirstBarrierIsAlwaysEmitted() {
	GlobalBarrierElider e;
	Check(!e.CanElide(1, 0), "nothing was emitted yet");
	Check(!e.CanElide(0, 0), "even for command 0 / work 0");
}

void TestRunOfBarriersCollapses() {
	GlobalBarrierElider e;
	e.Emitted(5, 100);
	Check(e.CanElide(5, 100), "same command buffer, no work since: elide");
	Check(e.CanElide(5, 100), "and again, the rule does not consume itself");
}

void TestWorkBreaksTheRun() {
	GlobalBarrierElider e;
	e.Emitted(5, 100);
	Check(!e.CanElide(5, 101), "a recorded command since the barrier: emit");
	e.Emitted(5, 101);
	Check(e.CanElide(5, 101), "the new barrier starts a new run");
}

void TestNewCommandBufferAlwaysGetsTheBarrier() {
	GlobalBarrierElider e;
	e.Emitted(5, 100);
	Check(!e.CanElide(6, 100), "after a submit the next command buffer needs its own barrier");
	e.Emitted(6, 100);
	Check(e.CanElide(6, 100), "then its run collapses");
	Check(!e.CanElide(5, 100), "a different buffer id never matches");
}

void TestReset() {
	GlobalBarrierElider e;
	e.Emitted(5, 100);
	e.Reset();
	Check(!e.CanElide(5, 100), "reset forgets the barrier");
}

} // namespace

int main() {
	TestFirstBarrierIsAlwaysEmitted();
	TestRunOfBarriersCollapses();
	TestWorkBreaksTheRun();
	TestNewCommandBufferAlwaysGetsTheBarrier();
	TestReset();
	std::printf("BarrierElisionTests: ok\n");
	return 0;
}
