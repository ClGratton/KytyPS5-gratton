#include "common/drainCauses.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace Common::DrainCauses;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "DrainCausesTests: failed: %s\n", message);
		std::abort();
	}
}

void TestAdd() {
	Table t;
	t.Add(Cause::FaultBuffer, true, 100);
	t.Add(Cause::FaultBuffer, false, 50);
	t.Add(Cause::StreamWrap, true, 7);
	Check(t.Get(Cause::FaultBuffer).calls == 2, "two fault drains");
	Check(t.Get(Cause::FaultBuffer).full_calls == 1, "one of them full");
	Check(t.Get(Cause::FaultBuffer).ticks == 150, "ticks add up");
	Check(t.Get(Cause::StreamWrap).calls == 1 && t.Get(Cause::Other).calls == 0, "causes are separate");
	Check(t.TotalTicks() == 157 && t.TotalCalls() == 3, "totals");
}

void TestSince() {
	Table t;
	t.Add(Cause::BufferReadback, true, 10);
	const Table base = t;
	t.Add(Cause::BufferReadback, true, 30);
	t.Add(Cause::FramePool, false, 5);
	const auto delta = t.Since(base);
	Check(delta.Get(Cause::BufferReadback).calls == 1 && delta.Get(Cause::BufferReadback).ticks == 30,
	      "window delta of one cause");
	Check(delta.Get(Cause::FramePool).calls == 1 && delta.Get(Cause::FramePool).full_calls == 0,
	      "new cause in the window");
	Check(delta.TotalTicks() == 35, "window total excludes the base");
	Check(t.TotalTicks() == 45, "the table itself is unchanged");
}

void TestOutOfRangeCauseGoesToOther() {
	Table t;
	t.Add(static_cast<Cause>(200), false, 3);
	Check(t.Get(Cause::Other).calls == 1 && t.Get(Cause::Other).ticks == 3, "unknown cause is 'other'");
}

void TestNames() {
	Check(std::strcmp(kCauseNames[static_cast<size_t>(Cause::Other)], "other") == 0, "other");
	Check(std::strcmp(kCauseNames[static_cast<size_t>(Cause::FramePool)], "frame_pool") == 0,
	      "last cause has a name");
	for (const auto* name: kCauseNames) {
		Check(name != nullptr && name[0] != '\0', "every cause has a name");
	}
}

} // namespace

int main() {
	TestAdd();
	TestSince();
	TestOutOfRangeCauseGoesToOther();
	TestNames();
	std::printf("DrainCausesTests: ok\n");
	return 0;
}
