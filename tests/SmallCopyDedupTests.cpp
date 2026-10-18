#include "graphics/host_gpu/renderer/cache/smallCopyDedup.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Libs::Graphics::SmallCopyDedup;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SmallCopyDedupTests: failed: %s\n", message);
		std::abort();
	}
}

// What BufferCache does on a miss: stage, write the ring from the staged bytes, commit.
void Store(SmallCopyDedup& dedup, uint64_t address, const std::vector<uint8_t>& guest, uint64_t offset, uint64_t tick,
           uint64_t lap, std::vector<uint8_t>& ring) {
	const auto* kept = dedup.Stage(address, guest.data(), static_cast<uint32_t>(guest.size()));
	for (size_t i = 0; i < guest.size(); i++) ring[offset + i] = kept[i];
	dedup.Commit(address, static_cast<uint32_t>(guest.size()), offset, tick, lap);
}

void TestHitNeedsEqualBytes() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(4096);
	std::vector<uint8_t> a(64, 7);
	Check(!dedup.Find(0x1000, a.data(), 64, 5, 0).has_value(), "nothing is kept at first");
	Store(dedup, 0x1000, a, 128, 5, 0, ring);
	const auto hit = dedup.Find(0x1000, a.data(), 64, 5, 0);
	Check(hit.has_value() && *hit == 128, "an equal range in the same tick and lap returns the ring offset");
	a[63] = 8;
	Check(!dedup.Find(0x1000, a.data(), 64, 5, 0).has_value(), "one changed byte is a miss");
	a[63] = 7;
	Check(dedup.Find(0x1000, a.data(), 64, 5, 0).has_value(), "and equal again is a hit");
}

void TestKeyTickAndLap() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(4096);
	std::vector<uint8_t> a(32, 1);
	Store(dedup, 0x2000, a, 0, 9, 3, ring);
	Check(!dedup.Find(0x2010, a.data(), 32, 9, 3).has_value(), "another address is a miss even with equal bytes");
	Check(!dedup.Find(0x2000, a.data(), 16, 9, 3).has_value(), "another size is a miss");
	Check(!dedup.Find(0x2000, a.data(), 32, 10, 3).has_value(), "a later tick is a miss: the ring fence covers only the first");
	Check(!dedup.Find(0x2000, a.data(), 32, 9, 4).has_value(), "a later ring lap is a miss: the offset may be overwritten");
	Check(dedup.Find(0x2000, a.data(), 32, 9, 3).has_value(), "the original key still hits");
}

void TestRingHoldsTheKeptBytes() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(256);
	std::vector<uint8_t> a(16, 3);
	Store(dedup, 0x3000, a, 32, 1, 0, ring);
	for (size_t i = 0; i < 16; i++) Check(ring[32 + i] == 3, "the ring was written from the kept bytes");
	Check(dedup.GetStats().stores == 1, "one store");
}

void TestStagedButNotCommittedHoldsNothing() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(256);
	std::vector<uint8_t> a(16, 3);
	Store(dedup, 0x4000, a, 0, 1, 0, ring);
	std::vector<uint8_t> b(16, 4);
	(void)dedup.Stage(0x4000, b.data(), 16); // the ring map failed: no Commit
	Check(!dedup.Find(0x4000, a.data(), 16, 1, 0).has_value(), "a restaged slot no longer answers for the old copy");
	Check(!dedup.Find(0x4000, b.data(), 16, 1, 0).has_value(), "and not for the new one before its commit");
}

void TestStats() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(256);
	std::vector<uint8_t> a(48, 9);
	(void)dedup.Find(0x5000, a.data(), 48, 1, 0);
	Store(dedup, 0x5000, a, 64, 1, 0, ring);
	(void)dedup.Find(0x5000, a.data(), 48, 1, 0);
	const auto& s = dedup.GetStats();
	Check(s.lookups == 2 && s.hits == 1 && s.stores == 1 && s.bytes_saved == 48, "the counters add up");
	dedup.Clear();
	Check(!dedup.Find(0x5000, a.data(), 48, 1, 0).has_value(), "cleared");
}

// Four ranges that land in one set all stay; the fifth takes the oldest entry's place.
void TestSetAssociativity() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(4096);
	std::vector<uint8_t> a(16, 5);
	std::vector<uint64_t> same;
	const auto            set = SmallCopyDedup::SetOf(0x10000, 16);
	for (uint64_t address = 0x10000; same.size() < 5; address += 16) {
		if (SmallCopyDedup::SetOf(address, 16) == set) same.push_back(address);
	}
	for (size_t i = 0; i < 4; i++) Store(dedup, same[i], a, i * 16, 1 + i, 0, ring);
	for (size_t i = 0; i < 4; i++) {
		const auto hit = dedup.Find(same[i], a.data(), 16, 1 + i, 0);
		Check(hit.has_value() && *hit == i * 16, "four colliding ranges are all kept");
	}
	Store(dedup, same[4], a, 64, 5, 0, ring);
	Check(dedup.Find(same[4], a.data(), 16, 5, 0).has_value(), "the fifth is kept");
	Check(!dedup.Find(same[0], a.data(), 16, 1, 0).has_value(), "and the oldest was the one replaced");
	Check(dedup.Find(same[3], a.data(), 16, 4, 0).has_value(), "the others stay");
}

void TestMissReasons() {
	SmallCopyDedup       dedup;
	std::vector<uint8_t> ring(256);
	std::vector<uint8_t> a(16, 1);
	(void)dedup.Find(0x7000, a.data(), 16, 1, 0); // absent
	Store(dedup, 0x7000, a, 0, 1, 0, ring);
	(void)dedup.Find(0x7000, a.data(), 16, 2, 0); // stale: another tick
	a[0] = 9;
	(void)dedup.Find(0x7000, a.data(), 16, 1, 0); // content
	const auto& s = dedup.GetStats();
	Check(s.miss_absent == 1 && s.miss_stale == 1 && s.miss_content == 1, "the three miss reasons are counted apart");
}

} // namespace

int main() {
	TestHitNeedsEqualBytes();
	TestKeyTickAndLap();
	TestRingHoldsTheKeptBytes();
	TestStagedButNotCommittedHoldsNothing();
	TestStats();
	TestSetAssociativity();
	TestMissReasons();
	std::printf("SmallCopyDedupTests: ok\n");
	return 0;
}
