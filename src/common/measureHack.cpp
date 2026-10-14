#include "common/measureHack.h"

#include "common/emulatorConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace Common::MeasureHack {

namespace {

struct Counters {
	std::atomic<uint64_t> calls {0};
	std::atomic<int64_t>  last_verify_ns {0};
	std::atomic<uint64_t> skipped {0};
	std::atomic<uint64_t> checks {0};
	std::atomic<uint64_t> mismatches {0};
};

Counters g_counters[static_cast<size_t>(Kind::Count)];

const char* Name(Kind kind) {
	return kind == Kind::Srt ? "srt" : "meta";
}

} // namespace

bool SkipNow(Kind kind) noexcept {
	auto&      c      = g_counters[static_cast<size_t>(kind)];
	// A verify drain waits for the whole backlog of the GPU (about 9 ms here), so it is rationed by time,
	// not by call count: the stale reads of one frame can number in the hundreds (a count-based sample
	// then drains every frame and the "ceiling" is gone). At most one verify per kind every N seconds.
	const auto seconds = std::max<uint32_t>(Config::DebugSkipDrainVerify(), 1u);
	const auto now_ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                         std::chrono::steady_clock::now().time_since_epoch())
	                         .count();
	c.calls.fetch_add(1, std::memory_order_relaxed);
	auto       last   = c.last_verify_ns.load(std::memory_order_relaxed);
	const bool verify = (last == 0 || now_ns - last >= static_cast<int64_t>(seconds) * 1'000'000'000) &&
	                    c.last_verify_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed);
	if (!verify) {
		c.skipped.fetch_add(1, std::memory_order_relaxed);
	}
	return !verify;
}

void NoteVerify(Kind kind, uint64_t address, const void* stale, const void* now,
                uint64_t size) noexcept {
	auto&      c        = g_counters[static_cast<size_t>(kind)];
	const auto checks   = c.checks.fetch_add(1, std::memory_order_relaxed) + 1;
	const bool mismatch = std::memcmp(stale, now, size) != 0;
	if (mismatch) {
		c.mismatches.fetch_add(1, std::memory_order_relaxed);
	}
	if (mismatch || (checks & 3u) == 1u) {
		if (std::FILE* f = std::fopen("_skip_drain.log", "a")) {
			if (mismatch) {
				const auto* a = static_cast<const uint8_t*>(stale);
				const auto* b = static_cast<const uint8_t*>(now);
				std::fprintf(f, "MISMATCH %s addr=%llx size=%llu stale0=%02x now0=%02x\n", Name(kind),
				             static_cast<unsigned long long>(address),
				             static_cast<unsigned long long>(size), a[0], b[0]);
			}
			std::fprintf(f, "SUMMARY %s calls=%llu skipped=%llu checks=%llu mismatches=%llu\n",
			             Name(kind), static_cast<unsigned long long>(c.calls.load()),
			             static_cast<unsigned long long>(c.skipped.load()),
			             static_cast<unsigned long long>(c.checks.load()),
			             static_cast<unsigned long long>(c.mismatches.load()));
			std::fclose(f);
		}
	}
}

} // namespace Common::MeasureHack
