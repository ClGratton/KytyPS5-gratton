#include "graphics/guest_gpu/command_processor/submitCoalescer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;
using Decision = SubmitCoalescer::Decision;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SubmitCoalescerTests: failed: %s\n", message);
		std::abort();
	}
}

void TestMaxAgeFromVblank() {
	constexpr uint64_t us = 1'000'000; // clock units: microseconds
	Check(CoalesceMaxAge(60, us, 8) == 2083, "60 Hz / 8 is 2083 us");
	Check(CoalesceMaxAge(120, us, 8) == 1041, "the bound shrinks with a faster display");
	Check(CoalesceMaxAge(30, us, 8) == 4166, "and grows with a slower one");
	Check(CoalesceMaxAge(60, us, 8) < 5000, "always under the 5000 us that collapsed pacing");
	Check(CoalesceMaxAge(0, us, 8) == 1 && CoalesceMaxAge(60, us, 0) == 1, "never zero");
	Check(CoalesceMaxAge(360, 100, 64) == 1, "tiny result is clamped to 1");
}

void TestBatchBound() {
	SubmitCoalescer c({4, 1000});
	Check(c.OnEvent(0) == Decision::Defer, "first event is held");
	Check(c.OnEvent(1) == Decision::Defer, "second");
	Check(c.OnEvent(2) == Decision::Defer, "third");
	Check(c.OnEvent(3) == Decision::FlushBatch, "the 4th flushes");
	Check(c.Flushed() == 4, "four events were held");
	Check(c.Pending() == 0, "empty after the flush");
}

void TestAgeBound() {
	SubmitCoalescer c({100, 1000});
	Check(c.OnEvent(10) == Decision::Defer, "held");
	Check(!c.Due(1009), "not due before the bound");
	Check(c.Due(1010), "due at the bound");
	Check(c.OnEvent(1500) == Decision::FlushDeadline, "the age counts from the oldest held event");
}

void TestAgeBoundOnEvent() {
	SubmitCoalescer c({100, 1000});
	Check(c.OnEvent(10) == Decision::Defer, "held");
	Check(c.OnEvent(1011) == Decision::FlushDeadline, "an event arriving after the bound flushes");
}

void TestOtherFlushClearsPending() {
	SubmitCoalescer c({100, 1000});
	(void)c.OnEvent(0);
	(void)c.OnEvent(1);
	Check(c.Flushed() == 2, "an unrelated flush reports what it covered");
	Check(!c.Due(1'000'000), "nothing held, never due");
	Check(c.OnEvent(2'000'000) == Decision::Defer, "the age restarts with the next event");
	Check(!c.Due(2'000'999) && c.Due(2'001'000), "age counts from the first event after the flush");
}

void TestOneEventConfig() {
	SubmitCoalescer c({1, 1000});
	Check(c.OnEvent(0) == Decision::FlushBatch, "max_events 1 restores one submit per event");
}

} // namespace

int main() {
	TestMaxAgeFromVblank();
	TestBatchBound();
	TestAgeBound();
	TestAgeBoundOnEvent();
	TestOtherFlushClearsPending();
	TestOneEventConfig();
	std::printf("SubmitCoalescerTests: all passed\n");
	return 0;
}
