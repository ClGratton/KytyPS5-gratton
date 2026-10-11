#include "common/phaseTimers.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Clock = Common::PhaseTimers::ExclusiveClock<4, 3>;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PhaseTimersTests: failed: %s\n", message);
		std::abort();
	}
}

void TestFlat() {
	Clock c;
	c.Enter(1, 100);
	c.Exit(150);
	c.Enter(1, 200);
	c.Exit(210);
	Check(c.Total(1) == 60 && c.Calls(1) == 2, "two sibling scopes of one phase add up");
	Check(c.Total(0) == 0 && c.Sum() == 60, "time outside the scopes is not charged");
	Check(c.Depth() == 0, "balanced");
}

void TestNestedIsExclusive() {
	Clock c;
	c.Enter(0, 0);   // outer, e.g. PM4
	c.Enter(1, 10);  // inner, e.g. draw
	c.Enter(2, 15);  // innermost, e.g. pipeline
	c.Exit(35);      // pipeline: 20
	c.Exit(40);      // draw: 5 + 5
	c.Exit(100);     // outer: 10 + 60
	Check(c.Total(2) == 20, "innermost");
	Check(c.Total(1) == 10, "middle is exclusive of its child");
	Check(c.Total(0) == 70, "outer is exclusive of its children");
	Check(c.Sum() == 100, "totals add up to the outermost scope's wall time");
}

void TestDepthLimitBalances() {
	Clock c; // MaxDepth 3
	c.Enter(0, 0);
	c.Enter(1, 10);
	c.Enter(2, 20);
	c.Enter(3, 30); // too deep: ignored, its time stays with phase 2
	c.Enter(3, 35); // ignored too
	c.Exit(40);
	c.Exit(50);
	Check(c.Depth() == 3, "ignored scopes still balance");
	c.Exit(60); // phase 2 ends: 20..60 minus nothing = 40
	c.Exit(70); // phase 1: 10 + 10 = 20
	c.Exit(80); // phase 0: 10 + 10 = 20
	Check(c.Total(2) == 40 && c.Total(1) == 20 && c.Total(0) == 20, "deep time stays with the deepest tracked phase");
	Check(c.Calls(3) == 0, "ignored scopes are not counted");
	Check(c.Sum() == 80 && c.Depth() == 0, "sum equals wall time");
}

void TestMisuse() {
	Clock c;
	c.Exit(5); // unbalanced exit is harmless
	Check(c.Depth() == 0 && c.Sum() == 0, "exit with an empty stack does nothing");
	c.Enter(99, 10); // out-of-range phase is clamped to the last one
	c.Exit(20);
	Check(c.Total(3) == 10, "out-of-range phase goes to the last slot");
	c.Enter(0, 100);
	c.Exit(50); // clock going backwards never makes a negative charge
	Check(c.Total(0) == 0, "no negative time");
}

void TestCurrent() {
	Clock c;
	c.Enter(2, 0);
	c.Enter(1, 1);
	Check(c.Current() == 1, "top of the stack");
	c.Exit(2);
	Check(c.Current() == 2, "after exit");
}

} // namespace

int main() {
	TestFlat();
	TestNestedIsExclusive();
	TestDepthLimitBalances();
	TestMisuse();
	TestCurrent();
	std::printf("PhaseTimersTests: all passed\n");
	return 0;
}
