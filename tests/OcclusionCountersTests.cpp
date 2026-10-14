// Unit test for the ZPASS_DONE counter bookkeeping (graphics/host_gpu/renderer/occlusionCounters.h).
//
// A title brackets draws with two ZPASS_DONE dumps, begin at A and end at A + 8, and reads the
// difference of the two counters as the number of samples that passed. The bookkeeping must
// publish, for every dump, the running total of the samples of all earlier segments, count
// samples only while a query is open, and keep nested or interleaved queries consistent.

#include "graphics/host_gpu/renderer/occlusionCounters.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace Libs::Graphics::OcclusionCounters;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "OcclusionCountersTests: failed: %s\n", message);
		std::abort();
	}
}

// A guest: draws between dumps pass `samples` samples when a query is open (the host counted them).
struct Model {
	Segments                        segments {64};
	uint64_t                        total = 0;
	std::map<uint64_t, uint64_t>    memory; // address -> published DB 0 value
	std::map<uint32_t, uint64_t>    slot_samples;
	uint32_t                        active_slot = 0;
	bool                            active      = false;

	// Records draws of one render pass instance that pass `samples` samples in all.
	void Draw(uint64_t samples) {
		if (segments.Counting()) {
			if (!active) {
				active = segments.AllocateSlot(active_slot);
			}
			if (active) {
				slot_samples[active_slot] += samples;
			}
		}
	}

	void EndPass() { active = false; }

	void Dump(uint64_t address) {
		active = false; // the host query of the segment ends at the dump
		const auto dump = segments.OnDump(address);
		uint64_t   sum  = dump.uncounted ? UncountedSamples : 0;
		for (uint64_t slot = dump.first_slot; slot < dump.end_slot; slot++) {
			sum += slot_samples[static_cast<uint32_t>(slot % segments.Capacity())];
			slot_samples[static_cast<uint32_t>(slot % segments.Capacity())] = 0;
		}
		segments.Release(dump.end_slot);
		total += sum;
		memory[address] = DbValue(0, total);
	}

	uint64_t Read(uint64_t begin_address) const {
		return (memory.at(begin_address + 8) & ValueMask) - (memory.at(begin_address) & ValueMask);
	}
};

} // namespace

int main() {
	// The value layout: bit 63 marks a result ready, the count lives in DB 0 only.
	Check((DbValue(0, 5) & ReadyBit) != 0 && (DbValue(0, 5) & ValueMask) == 5, "DB 0 holds the count");
	Check((DbValue(3, 5) & ReadyBit) != 0 && (DbValue(3, 5) & ValueMask) == 0, "other DBs read zero");
	Check(DumpSize == 15 * 16 + 8, "a dump covers the 16 DB pairs");

	// A query around visible draws reads their samples; draws outside a query are not counted.
	{
		Model m;
		m.Draw(100); // outside any query
		m.Dump(0x1000);
		m.Draw(40);
		m.Draw(2);
		m.Dump(0x1008);
		m.EndPass();
		m.Draw(7); // after the query closed
		Check(m.Read(0x1000) == 42, "a query reads the samples between its dumps");
		Check(!m.segments.Counting(), "counting stops when the query closed");
	}
	// A query around nothing that passed reads zero (occluded).
	{
		Model m;
		m.Dump(0x2000);
		m.Dump(0x2008);
		Check(m.Read(0x2000) == 0, "an occluded object reads zero samples");
	}
	// Nested queries: the outer one sees the inner one's samples too.
	{
		Model m;
		m.Dump(0x3000);
		m.Draw(10);
		m.Dump(0x3100);
		m.Draw(5);
		m.Dump(0x3108);
		m.Draw(20);
		m.Dump(0x3008);
		Check(m.Read(0x3100) == 5, "the inner query reads its own samples");
		Check(m.Read(0x3000) == 35, "the outer query reads all samples in between");
	}
	// Samples of several render pass instances inside one segment add up.
	{
		Model m;
		m.Dump(0x4000);
		m.Draw(3);
		m.EndPass();
		m.Draw(4);
		m.EndPass();
		m.Dump(0x4008);
		Check(m.Read(0x4000) == 7, "a segment spanning render passes adds its slots");
	}
	// A begin without an end stops keeping the counters running after a few frames.
	{
		Segments s(8, 3);
		(void)s.OnDump(0x5000);
		Check(s.Counting(), "an open query keeps counting");
		for (int i = 0; i < 5; i++) {
			s.OnFrame();
		}
		Check(!s.Counting(), "a begin whose end never came is dropped");
	}
	// A full slot ring makes the segment read visible instead of dropping samples silently.
	{
		Segments s(2);
		(void)s.OnDump(0x6000);
		uint32_t slot = 0;
		Check(s.AllocateSlot(slot) && s.AllocateSlot(slot), "two slots fit");
		Check(!s.AllocateSlot(slot), "the ring is full");
		const auto dump = s.OnDump(0x6008);
		Check(dump.uncounted && dump.end_slot - dump.first_slot == 2, "the segment is marked uncounted");
		s.Release(dump.end_slot);
		Check(s.AllocateSlot(slot), "released slots can be used again");
	}
	std::printf("OcclusionCountersTests: all cases passed\n");
	return 0;
}
