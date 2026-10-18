#include "graphics/guest_gpu/lookaheadGovernor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::LookaheadGovernor;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LookaheadGovernorTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kS  = 1'000'000'000;

// Feeds the governor one 500 ms window with `lookups` lookups of which `misses` missed; returns the level.
struct Feed {
	LookaheadGovernor& governor;
	int64_t            now      = 0;
	uint64_t           events   = 0;
	uint64_t           misses   = 0;

	uint32_t Window(uint64_t lookups, uint64_t missed) {
		now += 500 * kMs;
		events += lookups;
		misses += missed;
		return governor.Update(now, events, misses);
	}
	uint32_t Quiet(int windows) {
		uint32_t level = governor.Active();
		for (int i = 0; i < windows; ++i) level = Window(2000, 0);
		return level;
	}
};

void TestStartsWithEveryWorker() {
	LookaheadGovernor governor(4);
	Check(governor.Active() == 4, "all workers at the start");
	Feed feed {governor};
	Check(feed.Window(2000, 0) == 4, "the first call only opens a window");
}

void TestDropsOneWorkerPerQuietRunDownToOne() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0); // opens
	Check(feed.Quiet(3) == 4, "three quiet windows are not enough");
	Check(feed.Quiet(1) == 3, "the fourth drops one worker");
	Check(feed.Quiet(4) == 2, "and another after four more");
	Check(feed.Quiet(4) == 1, "and another");
	Check(feed.Quiet(40) == 1, "never below one");
}

void TestWindowsWithTooFewLookupsSayNothing() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0);
	Check(feed.Quiet(3) == 4, "three quiet windows");
	Check(feed.Window(10, 0) == 4, "a window with almost no lookups changes nothing");
	Check(feed.Quiet(3) == 4, "and it broke the run: three more quiet windows are still not enough");
	Check(feed.Quiet(1) == 3, "four in a row drop one");
}

void TestMissesBringEveryWorkerBackAndRememberTheLevel() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0);
	feed.Quiet(4);
	feed.Quiet(4);
	Check(governor.Active() == 2, "down to two");
	Check(feed.Window(2000, 100) == 4, "5 % misses: every worker is back at once");
	// Hold-down: quiet windows do not drop anything for 20 s.
	Check(feed.Quiet(39) == 4, "no drop during the hold-down (19.5 s of quiet)");
	Check(feed.Quiet(5) == 3, "after the hold-down it drops to three (and not to two: that level failed)");
	Check(feed.Quiet(40) == 3, "and stays at three while the failed level is remembered");
}

void TestTheFailedLevelIsProbedAgainLater() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0);
	feed.Quiet(8);
	Check(governor.Active() == 2, "down to two");
	feed.Window(2000, 100);
	feed.Quiet(45); // 20 s hold-down, then four quiet windows
	Check(governor.Active() == 3, "three");
	feed.Quiet(1250); // 625 s: the memory of the failed level has expired
	Check(governor.Active() < 3, "two is probed again after a long quiet");
}

void TestModerateMissesBlockDropsButDoNotRestore() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0);
	feed.Quiet(4);
	Check(governor.Active() == 3, "three");
	// 2.5 % misses: above quiet, below the restore threshold.
	for (int i = 0; i < 40; ++i) feed.Window(2000, 50);
	Check(governor.Active() == 3, "unchanged while misses are between the two thresholds");
}

void TestMissesAtFullStrengthChangeNothing() {
	LookaheadGovernor governor(3);
	Feed              feed {governor};
	feed.Window(2000, 0);
	Check(feed.Window(2000, 100) == 3, "nothing to restore");
	Check(feed.Quiet(20) == 3, "and the hold-down still applies (10 s of quiet)");
}

void TestResetPutsEveryWorkerBack() {
	LookaheadGovernor governor(4);
	Feed              feed {governor};
	feed.Window(2000, 0);
	feed.Quiet(8);
	Check(governor.Active() == 2, "down to two");
	Check(governor.Reset(feed.now, feed.events, feed.misses) == 4, "reset: all workers");
	Check(feed.Quiet(3) == 4, "the quiet run starts over");
	Check(feed.Quiet(1) == 3, "and drops again");
}

void TestOneWorkerNeverMoves() {
	LookaheadGovernor governor(1);
	Feed              feed {governor};
	feed.Window(2000, 0);
	Check(feed.Quiet(40) == 1, "one worker stays one");
	Check(feed.Window(2000, 100) == 1, "also under misses");
}

} // namespace

int main() {
	TestStartsWithEveryWorker();
	TestDropsOneWorkerPerQuietRunDownToOne();
	TestWindowsWithTooFewLookupsSayNothing();
	TestMissesBringEveryWorkerBackAndRememberTheLevel();
	TestTheFailedLevelIsProbedAgainLater();
	TestModerateMissesBlockDropsButDoNotRestore();
	TestMissesAtFullStrengthChangeNothing();
	TestResetPutsEveryWorkerBack();
	TestOneWorkerNeverMoves();
	std::puts("LookaheadGovernorTests: ok");
	return 0;
}
