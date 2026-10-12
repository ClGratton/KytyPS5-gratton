#include "graphics/host_gpu/renderer/gpuOpRing.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace Libs::Graphics::GpuOps;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GpuOpRingTests: failed: %s\n", message);
		std::abort();
	}
}

Op MakeOp(Kind kind, uint64_t tick) {
	Op op;
	op.kind = kind;
	op.tick = tick;
	return op;
}

void TestInFlightByTick() {
	Ring ring(16);
	for (uint64_t tick = 1; tick <= 6; tick++) {
		ring.Push(MakeOp(Kind::Draw, tick));
		ring.Push(MakeOp(Kind::Dispatch, tick));
	}
	Check(ring.Count() == 12, "twelve pushes");
	Check(ring.InFlight(6, 100).empty(), "everything completed");
	const auto ops = ring.InFlight(4, 100);
	Check(ops.size() == 4, "ticks 5 and 6 are unfinished: four ops");
	Check(ops.front().tick == 5 && ops.back().tick == 6, "oldest first, newest last");
	Check(ops[0].seq < ops[1].seq && ops[1].seq < ops[2].seq, "sequence numbers grow");
	Check(ring.InFlight(0, 100).size() == 12, "nothing completed: all of them");
}

void TestMaxKeepsNewest() {
	Ring ring(64);
	for (uint64_t i = 1; i <= 20; i++) {
		ring.Push(MakeOp(Kind::Draw, 10));
	}
	const auto ops = ring.InFlight(0, 5);
	Check(ops.size() == 5, "at most max");
	Check(ops.back().seq == 20 && ops.front().seq == 16, "the newest five, in order");
}

void TestWraparound() {
	Ring ring(8);
	for (uint64_t i = 1; i <= 30; i++) {
		ring.Push(MakeOp(Kind::Draw, i)); // one op per submission
	}
	const auto ops = ring.InFlight(0, 100);
	Check(ops.size() == 8, "only the capacity survives");
	Check(ops.front().seq == 23 && ops.back().seq == 30, "the newest eight");
	Op op;
	Check(ring.Find(30, op) && op.seq == 30, "newest is found");
	Check(ring.Find(23, op) && op.seq == 23, "oldest kept is found");
	Check(!ring.Find(22, op), "overwritten op is gone");
	Check(!ring.Find(31, op) && !ring.Find(0, op), "future and zero are not found");
	const auto done = ring.InFlight(27, 100);
	Check(done.size() == 3 && done.front().tick == 28, "after wrap the tick filter still applies");
}

void TestFormat() {
	Op draw = MakeOp(Kind::MeshDraw, 7);
	draw.seq    = 3;
	draw.shader0 = 0xabc;
	draw.n[0]   = 5; // prim
	draw.n[1]   = 1; // indexed
	draw.n[2]   = 1; // restart
	draw.n[3]   = 1234;
	draw.n[7]   = 17;
	const auto text = Format(draw);
	Check(text.find("mesh-draw") != std::string::npos, "mesh draw named");
	Check(text.find("prim=5") != std::string::npos && text.find("restart=1") != std::string::npos,
	      "primitive and restart shown");
	Check(text.find("count=1234") != std::string::npos && text.find("groups=17") != std::string::npos,
	      "counts shown");
	Op indirect = MakeOp(Kind::DispatchIndirect, 9);
	indirect.addr = 0x1000;
	indirect.n[0] = 0xdeadbeef;
	const auto itext = Format(indirect);
	Check(itext.find("dispatch-indirect") != std::string::npos &&
	          itext.find("guest-view=3735928559x") != std::string::npos,
	      "indirect args shown");
	Ring ring(4);
	ring.Push(draw);
	const auto report = ring.Report(0, 10);
	Check(report.find("1 op(s)") != std::string::npos && report.find("gpu-op: ") != std::string::npos,
	      "report lists the op");
}

} // namespace

int main() {
	TestInFlightByTick();
	TestMaxKeepsNewest();
	TestWraparound();
	TestFormat();
	std::printf("GpuOpRingTests: all passed\n");
	return 0;
}
