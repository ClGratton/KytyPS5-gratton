#include "graphics/host_gpu/renderer/pollWait.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::PollUntil;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PollWaitTests: failed: %s\n", message);
		std::abort();
	}
}

struct Fake {
	uint64_t clock = 0;
	int      polls = 0;
	int      pauses = 0;
	int      ready_after = 0; // polls before reached() turns true; <0: never
};

bool Run(Fake& fake, uint64_t budget_ns, uint64_t step_ns) {
	return PollUntil(
	    [&] {
		    ++fake.polls;
		    return fake.ready_after >= 0 && fake.polls > fake.ready_after;
	    },
	    [&] {
		    fake.clock += step_ns;
		    return fake.clock;
	    },
	    [&] { ++fake.pauses; }, budget_ns);
}

void TestZeroBudgetNeverPolls() {
	Fake fake;
	fake.ready_after = 0;
	Check(!Run(fake, 0, 100) && fake.polls == 0, "budget 0: no poll, the caller blocks as before");
}

void TestAlreadyReached() {
	Fake fake;
	Check(Run(fake, 50'000, 1'000) && fake.polls == 1 && fake.pauses == 0,
	      "a value that is already there is seen by the first poll");
}

void TestReachedWithinBudget() {
	Fake fake;
	fake.ready_after = 5;
	Check(Run(fake, 50'000, 1'000), "seen within the budget");
	Check(fake.polls == 6 && fake.pauses == 5, "one pause between polls");
}

void TestBudgetExpires() {
	Fake fake;
	fake.ready_after = -1;
	Check(!Run(fake, 10'000, 1'000), "never reached: gives up");
	Check(fake.clock >= 10'000 && fake.clock <= 12'000, "gives up when the budget has elapsed, not much later");
}

} // namespace

int main() {
	TestZeroBudgetNeverPolls();
	TestAlreadyReached();
	TestReachedWithinBudget();
	TestBudgetExpires();
	std::printf("PollWaitTests: ok\n");
	return 0;
}
