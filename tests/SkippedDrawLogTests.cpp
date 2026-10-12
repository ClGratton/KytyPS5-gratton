#include "graphics/host_gpu/renderer/skippedDrawLog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::SkippedDrawLog::ShouldLog;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SkippedDrawLogTests: failed: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	Check(!ShouldLog(0), "occurrence 0 does not exist");
	Check(ShouldLog(1), "the first skip is always reported");
	Check(ShouldLog(2) && !ShouldLog(3) && ShouldLog(4) && !ShouldLog(5) && !ShouldLog(7) &&
	          ShouldLog(8),
	      "then every power of two");
	uint64_t lines = 0;
	for (uint64_t i = 1; i <= 3'600ull * 60; i++) { // an hour of one skipped draw per frame at 60 Hz
		lines += ShouldLog(i) ? 1 : 0;
	}
	Check(lines == 18, "an hour of per-frame skips is 18 lines");
	Check(ShouldLog(1ull << 62) && !ShouldLog((1ull << 62) + 1), "no overflow at large counts");
	std::printf("SkippedDrawLogTests: ok\n");
	return 0;
}
