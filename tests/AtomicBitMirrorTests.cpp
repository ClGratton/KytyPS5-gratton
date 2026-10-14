#include "common/atomicBitMirror.h"
#include "common/bitArray.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

constexpr size_t kBits = 1024;
using Bits             = Common::BitArray<kBits>;
using Mirror           = Common::AtomicBitMirror<kBits>;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AtomicBitMirrorTests: failed: %s\n", message);
		std::abort();
	}
}

uint64_t Next(uint64_t& state) {
	state += 0x9e3779b97f4a7c15ull;
	uint64_t z = state;
	z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

// Any(start, end) is FirstRangeFrom(start).first < end for every range, after random Set/Unset
// range operations published the way the tracker does it (the changed range only).
void TestAgainstBitArray() {
	Bits   bits;
	Mirror mirror;
	mirror.Reset(bits);
	uint64_t rng = 7;
	for (int step = 0; step < 4000; step++) {
		const size_t a = Next(rng) % kBits;
		const size_t b = a + 1 + Next(rng) % (Next(rng) % 4 == 0 ? kBits - a : 70);
		const size_t end = b > kBits ? kBits : b;
		if (Next(rng) % 2 == 0) {
			bits.SetRange(a, end);
		} else {
			bits.UnsetRange(a, end);
		}
		mirror.Publish(bits, a, end);
		Check(mirror.Matches(bits), "mirror differs from the bits after Publish");
		for (int q = 0; q < 12; q++) {
			const size_t qs = Next(rng) % kBits;
			const size_t qe = qs + 1 + Next(rng) % (kBits - qs);
			Check(mirror.Any(qs, qe) == (bits.FirstRangeFrom(qs).first < qe),
			      "Any differs from FirstRangeFrom");
		}
	}
	Check(!mirror.Any(5, 5) && !mirror.Any(10, 5) && !mirror.Any(0, kBits + 1), "degenerate ranges answer false");
	bits.Fill();
	mirror.Reset(bits);
	Check(mirror.Any(0, 1) && mirror.Any(1023, 1024) && mirror.Matches(bits), "Reset of a full array");
}

// Visibility: bit i is set and published before the counter says i + 1 bits exist, so a reader that
// read the counter c must find bit c - 1 (and the whole prefix) set.
void TestConcurrentReaders() {
	Bits                  bits;
	Mirror                mirror;
	std::atomic<uint64_t> published {0};
	std::atomic<bool>     failed {false};
	mirror.Reset(bits);
	std::thread writer([&] {
		for (size_t i = 0; i < kBits; i++) {
			bits.Set(i);
			mirror.Publish(bits, i, i + 1);
			published.store(i + 1, std::memory_order_release);
		}
	});
	std::thread reader([&] {
		uint64_t c = 0;
		while (c < kBits) {
			c = published.load(std::memory_order_acquire);
			if (c != 0 && (!mirror.Any(c - 1, c) || !mirror.Any(0, c))) {
				failed.store(true);
				return;
			}
		}
	});
	writer.join();
	reader.join();
	Check(!failed.load(), "a published bit was not visible to a reader that saw the counter");
	Check(mirror.Matches(bits), "final state differs");
}

} // namespace

int main() {
	TestAgainstBitArray();
	TestConcurrentReaders();
	std::puts("AtomicBitMirrorTests: ok");
	return 0;
}
