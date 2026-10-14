#include "graphics/host_gpu/renderer/cache/knownFills.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::KnownFills;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "KnownFillsTests: failed: %s\n", message);
		std::abort();
	}
}

void TestRecordAndGet() {
	KnownFills f;
	uint32_t   v = 0;
	Check(!f.TryGet(0x1000, 0x100, &v), "empty: unknown");
	f.Record(0x1000, 0x1000, 0xabababab);
	Check(f.TryGet(0x1000, 0x1000, &v) && v == 0xabababab, "the whole fill");
	Check(f.TryGet(0x1800, 0x100, &v) && v == 0xabababab, "a part inside");
	Check(!f.TryGet(0x0fff, 2, &v), "starting before the fill");
	Check(!f.TryGet(0x1800, 0x1000, &v), "running past the fill");
	Check(!f.TryGet(0x2000, 4, &v), "after the fill");
	Check(!f.TryGet(0x1000, 0, &v), "empty range is never known");
}

void TestOverwriteReplaces() {
	KnownFills f;
	uint32_t   v = 0;
	f.Record(0x1000, 0x1000, 1);
	f.Record(0x1000, 0x1000, 2);
	Check(f.TryGet(0x1000, 0x1000, &v) && v == 2, "a newer fill replaces the older");
	Check(f.Count() == 1, "one range");
}

void TestForgetSplits() {
	KnownFills f;
	uint32_t   v = 0;
	f.Record(0x1000, 0x3000, 7);
	f.Forget(0x2000, 0x1000); // the middle page was written by something else
	Check(f.TryGet(0x1000, 0x1000, &v) && v == 7, "head stays known");
	Check(f.TryGet(0x3000, 0x1000, &v) && v == 7, "tail stays known");
	Check(!f.TryGet(0x2000, 4, &v), "the written part is unknown");
	Check(!f.TryGet(0x1000, 0x3000, &v), "the whole range is no longer one fill");
	Check(f.Count() == 2, "split in two");
}

void TestForgetEdges() {
	KnownFills f;
	uint32_t   v = 0;
	f.Record(0x1000, 0x1000, 9);
	f.Forget(0x0800, 0x0900); // overlaps the head by 0x100 bytes
	Check(!f.TryGet(0x1000, 4, &v) && !f.TryGet(0x10ff, 1, &v), "overlapped head forgotten");
	Check(f.TryGet(0x1100, 0xf00, &v) && v == 9, "rest known");
	f.Forget(0x1f00, 0x1000); // overlaps the tail
	Check(f.TryGet(0x1100, 0xe00, &v), "tail trimmed");
	Check(!f.TryGet(0x1f00, 1, &v), "tail forgotten");
	f.Forget(0, ~0ull >> 1);
	Check(f.Count() == 0, "forgetting everything");
}

void TestRecordOverlapSplitsOlder() {
	KnownFills f;
	uint32_t   v = 0;
	f.Record(0x1000, 0x3000, 1);
	f.Record(0x2000, 0x1000, 2);
	Check(f.TryGet(0x1000, 0x1000, &v) && v == 1, "older head");
	Check(f.TryGet(0x2000, 0x1000, &v) && v == 2, "newer middle");
	Check(f.TryGet(0x3000, 0x1000, &v) && v == 1, "older tail");
	Check(!f.TryGet(0x1000, 0x2000, &v), "a range over two fills is not known");
}

void TestUnrelatedForgetIsNoop() {
	KnownFills f;
	uint32_t   v = 0;
	f.Record(0x1000, 0x1000, 5);
	f.Forget(0x3000, 0x1000);
	f.Forget(0x0000, 0x1000);
	Check(f.TryGet(0x1000, 0x1000, &v) && v == 5, "disjoint writes keep the fill");
}

void TestReadDwords() {
	KnownFills f;
	uint32_t   out[8] = {};
	Check(!f.TryReadDwords(0x1000, 4, out), "empty: unknown");
	f.Record(0x1018, 4, 0);     // one stored dword
	f.Record(0x1000, 4, 0xc0);  // another one, apart
	Check(f.TryReadDwords(0x1018, 4, out) && out[0] == 0, "one known dword");
	Check(f.TryReadDwords(0x1000, 4, out) && out[0] == 0xc0, "the other known dword");
	Check(!f.TryReadDwords(0x1000, 8, out), "the dword after a known one is not known");
	Check(!f.TryReadDwords(0x1014, 8, out), "a range that starts unknown and ends known");
	f.Record(0x1004, 0x14, 5); // 0x1004..0x1018 filled
	Check(f.TryReadDwords(0x1000, 0x1c, out), "adjacent fills make one known run");
	Check(out[0] == 0xc0 && out[1] == 5 && out[5] == 5 && out[6] == 0, "values of the run, in order");
	Check(!f.TryReadDwords(0x1000, 0x20, out), "past the last known dword");
	Check(!f.TryReadDwords(0x1002, 4, out), "unaligned address");
	Check(!f.TryReadDwords(0x1000, 3, out), "unaligned size");
	Check(!f.TryReadDwords(0x1000, 0, out), "empty range");
	f.Forget(0x1018, 4); // another writer touched the stored dword
	Check(!f.TryReadDwords(0x1018, 4, out), "a forgotten dword is unknown again");
	Check(f.TryReadDwords(0x1000, 0x18, out), "the rest stays known");
	KnownFills g;
	g.Record(0x2000, 6, 9); // a fill that ends inside a dword
	Check(!g.TryReadDwords(0x2000, 8, out) && !g.TryReadDwords(0x2004, 4, out),
	      "a dword that straddles the end of a fill is not known");
	KnownFills h;
	h.Record(0x3002, 8, 9); // a fill that starts inside a dword
	Check(!h.TryReadDwords(0x3004, 4, out), "a fill with a misaligned start never answers");
}

void TestCollectDwords() {
	KnownFills f;
	f.Record(0x1000, 4, 1);
	f.Record(0x1018, 8, 2);
	f.Record(0x2000, 4, 3);
	std::vector<std::pair<uint64_t, uint32_t>> out;
	f.CollectDwords(0x1000, 0x20, out, 100);
	Check(out.size() == 3 && out[0] == std::make_pair<uint64_t, uint32_t>(0x1000, 1) &&
	          out[1] == std::make_pair<uint64_t, uint32_t>(0x1018, 2) && out[2].first == 0x101c,
	      "the known dwords of a range, in order");
	out.clear();
	f.CollectDwords(0x101c, 4, out, 100);
	Check(out.size() == 1 && out[0].first == 0x101c, "a range that starts inside a fill");
	out.clear();
	f.CollectDwords(0x1000, 0x2000, out, 2);
	Check(out.size() == 2, "the limit");
	out.clear();
	f.CollectDwords(0x3000, 0x100, out, 100);
	Check(out.empty(), "nothing known there");
}

} // namespace

int main() {
	TestCollectDwords();
	TestReadDwords();
	TestRecordAndGet();
	TestOverwriteReplaces();
	TestForgetSplits();
	TestForgetEdges();
	TestRecordOverlapSplitsOlder();
	TestUnrelatedForgetIsNoop();
	std::printf("KnownFillsTests: ok\n");
	return 0;
}
