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

} // namespace

int main() {
	TestRecordAndGet();
	TestOverwriteReplaces();
	TestForgetSplits();
	TestForgetEdges();
	TestRecordOverlapSplitsOlder();
	TestUnrelatedForgetIsNoop();
	std::printf("KnownFillsTests: ok\n");
	return 0;
}
