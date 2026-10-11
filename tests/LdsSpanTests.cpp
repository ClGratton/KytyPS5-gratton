#include "graphics/shader/recompiler/ir/ldsSpan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "LdsSpanTests: failed: %s\n", message);
		std::abort();
	}
}

void TestAccessSpan() {
	// Offset 0, one dword, wave64: the lane term dominates.
	static_assert(LdsAccessSpanDwords(0, 0, 1, 64) == 65);
	// A pixel shader pattern: slots 256 bytes apart, largest at byte 512, one dword each.
	static_assert(LdsAccessSpanDwords(512, 0, 1, 64) == 129 + 64);
	// The larger of the two immediates (ds_read2 style) counts, and the data size is added.
	static_assert(LdsAccessSpanDwords(16, 64, 2, 32) == (64 + 8 + 3) / 4 + 32);
	// A zero-dword access is treated as one dword.
	static_assert(LdsAccessSpanDwords(0, 0, 0, 64) == LdsAccessSpanDwords(0, 0, 1, 64));
	// Unaligned byte offsets round up to a whole dword.
	static_assert(LdsAccessSpanDwords(5, 0, 1, 0) == 3);
}

void TestPowerOfTwoRounding() {
	Check(ReducedLdsDwords(0) == 0, "an unknown span keeps the full array");
	Check(ReducedLdsDwords(1) == MinReducedLdsDwords, "tiny spans use the minimum");
	Check(ReducedLdsDwords(64) == 64, "64 stays 64");
	Check(ReducedLdsDwords(65) == 128, "65 rounds up to 128");
	Check(ReducedLdsDwords(193) == 256, "the observed 193-dword span uses 1 KiB");
	Check(ReducedLdsDwords(4096) == 4096, "4096 stays");
	Check(ReducedLdsDwords(4097) == 0, "needing more than half the array keeps the full array");
	Check(ReducedLdsDwords(FullLdsDwords) == 0, "a full-size span is not reduced");
	Check(ReducedLdsDwords(100000) == 0, "huge spans are not reduced");
	for (uint32_t span = 1; span <= FullLdsDwords; span++) {
		const auto size = ReducedLdsDwords(span);
		if (size == 0) {
			Check(span > FullLdsDwords / 2, "only spans above half the array stay full");
			continue;
		}
		Check((size & (size - 1)) == 0, "reduced size is a power of two");
		Check(size >= span && size >= MinReducedLdsDwords, "reduced size covers the span");
		Check(size < FullLdsDwords, "reduced size is smaller than the full array");
		Check(size / 2 < span || size == MinReducedLdsDwords, "reduced size is the smallest one");
	}
}

void TestWrap() {
	static_assert(WrapLdsIndex(0, 256) == 0);
	static_assert(WrapLdsIndex(255, 256) == 255);
	static_assert(WrapLdsIndex(256, 256) == 0);
	static_assert(WrapLdsIndex(300, 256) == 44);
	// Indexes the shader used before the reduction keep unique slots when they fit.
	std::set<uint32_t> slots;
	for (uint32_t lane = 0; lane < 64; lane++) {
		for (uint32_t k = 0; k < 3; k++) {
			slots.insert(WrapLdsIndex(lane + k * 64, ReducedLdsDwords(LdsAccessSpanDwords(512, 0, 1, 64))));
		}
	}
	Check(slots.size() == 64 * 3, "the three 256-byte slots of 64 lanes stay distinct after the wrap");
	// A wave base that is a multiple of the array size aliases onto the same slots (private array).
	Check(WrapLdsIndex(4096 + 7, 256) == 7, "a distant wave base wraps onto the same private slots");
}

} // namespace

int main() {
	TestAccessSpan();
	TestPowerOfTwoRounding();
	TestWrap();
	std::printf("LdsSpanTests: ok\n");
	return 0;
}
