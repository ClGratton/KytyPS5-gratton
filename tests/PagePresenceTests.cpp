#include "common/pagePresence.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Common::PagePresence;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PagePresenceTests: failed: %s\n", message);
		std::abort();
	}
}

void TestAddRemoveAny() {
	PagePresence p;
	Check(!p.Any(0x500000000, 0x1000), "empty set has no owner");
	Check(p.Add(0x500001800, 0x2000), "add");
	// bytes 0x500001800 .. 0x5000037ff: pages 0x500001000, 0x500002000, 0x500003000
	Check(!p.Any(0x500000000, 0x1000), "page before the range");
	Check(p.Any(0x500000f00, 0x200), "a read that reaches into the first page");
	Check(p.Any(0x500002000, 4), "middle page");
	Check(p.Any(0x500003fff, 1), "last byte of the last page");
	Check(!p.Any(0x500004000, 4), "page after the range");
	Check(p.Count(0x500001000) == 1 && p.Count(0x500003000) == 1 && p.Count(0x500004000) == 0, "counts per page");
	Check(p.Remove(0x500001800, 0x2000), "remove");
	Check(!p.Any(0x500001000, 0x3000), "removed");
}

void TestOverlappingOwnersAndEdges() {
	PagePresence p;
	p.Add(0x600000000, 0x4000); // pages 0..3
	p.Add(0x600002000, 0x4000); // pages 2..5
	Check(p.Count(0x600002000) == 2 && p.Count(0x600000000) == 1 && p.Count(0x600005000) == 1, "two owners on shared pages");
	p.Remove(0x600000000, 0x4000);
	Check(!p.Any(0x600000000, 0x2000) && p.Any(0x600002000, 1), "the other owner keeps its pages");
	p.Remove(0x600002000, 0x4000);
	Check(!p.Any(0x600000000, 0x8000), "all gone");
	p.Remove(0x600000000, 0x1000); // an unmatched removal stays at zero
	Check(p.Count(0x600000000) == 0, "no underflow");
	Check(!p.Add(~uint64_t {0} - 10, 100), "a wrapping range is refused");
	Check(!p.Add(uint64_t {1} << 44, 16), "outside the space is refused");
}

void TestLeafBoundaryAndLongQuery() {
	PagePresence p;
	const uint64_t edge = uint64_t {256} << 20; // a leaf covers 256 MiB: this is the first page of leaf 1
	p.Add(edge - 0x1000, 0x2000);
	Check(p.Any(edge - 4, 4) && p.Any(edge, 4), "an image across a leaf boundary");
	Check(!p.Any(edge + 0x1000, 4), "after it");
	Check(p.Any(0x1000, 65 * 0x1000), "a query longer than the cap is not answered (yes)");
	Check(!p.Any(0x7000000, 0), "empty query");
}

void TestConcurrentReaders() {
	// One writer adds and removes a range while readers ask: a reader never sees "none" while the range is added and
	// stable, and the final state is empty.
	PagePresence       p;
	std::atomic<bool>  stop {false};
	std::atomic<bool>  stable {false};
	std::atomic<uint64_t> wrong {0};
	p.Add(0x700000000, 0x10000);
	stable.store(true);
	std::vector<std::thread> readers;
	for (int t = 0; t < 3; t++) {
		readers.emplace_back([&] {
			while (!stop.load()) {
				if (stable.load() && !p.Any(0x700004000, 8)) {
					wrong.fetch_add(1);
				}
			}
		});
	}
	for (int i = 0; i < 20000; i++) {
		p.Add(0x700000000, 0x10000); // second owner: the count never reaches zero in the middle
		p.Remove(0x700000000, 0x10000);
	}
	stop.store(true);
	for (auto& r: readers) r.join();
	Check(wrong.load() == 0, "readers never miss an owner that stays");
	p.Remove(0x700000000, 0x10000);
	Check(!p.Any(0x700000000, 0x10000), "final state empty");
}

void TestQuery() {
	using Answer = PagePresence::Answer;
	PagePresence p;
	Check(p.Query(0x500000000, 0x1000) == Answer::None, "empty set: none");
	Check(p.Query(0x500000000, 0) == Answer::None, "empty query: none");
	p.Add(0x500001800, 0x2000); // pages 0x500001000 .. 0x500003000
	Check(p.Query(0x500000000, 0x1000) == Answer::None, "page before: none");
	Check(p.Query(0x500000f00, 0x200) == Answer::Some, "reaches the first page: some");
	Check(p.Query(0x500002000, 4) == Answer::Some, "middle page: some");
	Check(p.Query(0x500004000, 4) == Answer::None, "page after: none");
	Check(p.Query(0x500000000, 0x5000) == Answer::Some, "a range around it: some");
	Check(p.Query(0x1000, 65 * 0x1000) == Answer::Unknown, "longer than the cap: unknown");
	Check(p.Query(uint64_t {1} << 44, 16) == Answer::Unknown, "outside the space: unknown");
	Check(p.Query(~uint64_t {0} - 10, 100) == Answer::Unknown, "wrapping range: unknown");
	// Query agrees with Any wherever it answers.
	for (uint64_t address = 0x4ffffe00; address < 0x500005000; address += 0x340) {
		for (uint64_t size: {uint64_t {1}, uint64_t {0x200}, uint64_t {0x1801}, uint64_t {0x3000}}) {
			const auto a = p.Query(address, size);
			Check(a != Answer::Unknown, "answered");
			Check((a == Answer::Some) == p.Any(address, size), "Query equals Any");
		}
	}
	// A saturated page is never trusted.
	PagePresence q;
	for (int i = 0; i < 65535; i++) q.Add(0x800000000, 0x1000);
	Check(q.Query(0x800000000, 8) == Answer::Unknown, "saturated page: unknown");
	Check(q.Query(0x800001000, 8) == Answer::None, "the page after a saturated one is exact");
	for (int i = 0; i < 65535; i++) q.Remove(0x800000000, 0x1000);
	Check(q.Query(0x800000000, 8) == Answer::Unknown, "a saturated page stays unknown (it never reaches zero)");
}

} // namespace

int main() {
	TestAddRemoveAny();
	TestOverlappingOwnersAndEdges();
	TestLeafBoundaryAndLongQuery();
	TestQuery();
	TestConcurrentReaders();
	std::printf("PagePresenceTests: all passed\n");
	return 0;
}
