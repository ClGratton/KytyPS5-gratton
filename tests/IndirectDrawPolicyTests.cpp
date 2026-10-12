#include "common/indirectDrawPolicy.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Common::IndirectDraw::Decide;
using Common::IndirectDraw::Inputs;
using Common::IndirectDraw::IsFallbackWorthLogging;
using Common::IndirectDraw::kRouteNames;
using Common::IndirectDraw::Route;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "IndirectDrawPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

// The draw the GPU path exists for: indexed, mesh-emulated, 16-bit indices, GPU-written arguments.
constexpr Inputs Eligible {.indexed              = true,
                           .arguments_gpu_data   = true,
                           .mesh_emulated        = true,
                           .list_primitive       = false,
                           .first_instance_ok    = false,
                           .index_element_bytes  = 2,
                           .index_buffer_size    = 4096,
                           .count_in_memory      = false};

void TestEligibleDrawGoesToTheGpu() {
	Check(Decide(Eligible) == Route::Gpu, "indexed mesh draw with GPU-written arguments");
	auto in = Eligible;
	in.index_element_bytes = 4;
	Check(Decide(in) == Route::Gpu, "32-bit indices");
}

void TestPlainListDrawGoesToTheGpu() {
	auto in = Eligible;
	in.mesh_emulated     = false;
	in.list_primitive    = true;
	in.first_instance_ok = true;
	Check(Decide(in) == Route::Gpu, "plain indexed list draw (the Astro Bot case)");
	in.first_instance_ok = false;
	Check(Decide(in) == Route::CpuNoFirstInstance, "without drawIndirectFirstInstance");
	in.first_instance_ok = true;
	in.list_primitive    = false;
	Check(Decide(in) == Route::CpuPrimitive, "strips, rects and quads keep the CPU path");
}

void TestCpuWrittenArgumentsAreTheNormalPath() {
	auto in = Eligible;
	in.arguments_gpu_data = false;
	Check(Decide(in) == Route::CpuNotGpuData, "arguments the CPU wrote need no drain");
	Check(!IsFallbackWorthLogging(Route::CpuNotGpuData), "and are not logged");
	in.mesh_emulated = false;
	Check(Decide(in) == Route::CpuNotGpuData, "the data check comes first");
}

void TestEachUnsupportedCaseFallsBackWithItsOwnReason() {
	auto in = Eligible;
	in.indexed = false;
	Check(Decide(in) == Route::CpuNotIndexed, "non-indexed draw");
	in = Eligible;
	in.index_element_bytes = 1;
	Check(Decide(in) == Route::CpuIndex8, "8-bit indices");
	in.index_element_bytes = 0;
	Check(Decide(in) == Route::CpuIndex8, "unknown index size");
	in = Eligible;
	in.index_buffer_size = 0;
	Check(Decide(in) == Route::CpuNoIndexLimit, "unknown INDEX_BUFFER_SIZE");
	in = Eligible;
	in.count_in_memory = true;
	Check(Decide(in) == Route::CpuCountInMemory, "count read from guest memory");
	for (auto route : {Route::CpuNotIndexed, Route::CpuPrimitive, Route::CpuIndex8,
	                   Route::CpuNoIndexLimit, Route::CpuCountInMemory,
	                   Route::CpuOffsetRegisters}) {
		Check(IsFallbackWorthLogging(route), "fallbacks are logged once");
	}
	Check(!IsFallbackWorthLogging(Route::Gpu), "the GPU route is not a fallback");
}

constexpr uint32_t kNop = 0x280, kGs0 = 0x8c;

void TestPatchedSgprOfAPacketRegister() {
	using Common::IndirectDraw::PatchedSgpr;
	Check(PatchedSgpr(kNop, kNop, kGs0, 32) == Common::IndirectDraw::kNotPatched, "no register");
	Check(PatchedSgpr(kGs0, kNop, kGs0, 32) == 0, "first GS user SGPR");
	Check(PatchedSgpr(kGs0 + 5, kNop, kGs0, 32) == 5, "a GS user SGPR");
	Check(PatchedSgpr(kGs0 + 31, kNop, kGs0, 32) == 31, "last GS user SGPR");
	Check(PatchedSgpr(kGs0 + 32, kNop, kGs0, 32) == Common::IndirectDraw::kUnknownPatch,
	      "past the GS window");
	Check(PatchedSgpr(0x0c, kNop, kGs0, 32) == Common::IndirectDraw::kUnknownPatch,
	      "a PS register cannot be reasoned about");
}

void TestPatchedOffsetsRead() {
	using Common::IndirectDraw::kNotPatched;
	using Common::IndirectDraw::kUnknownPatch;
	using Common::IndirectDraw::PatchedOffsetsRead;
	const std::array<uint32_t, 4> reads {8, 9, 12, 20};
	const int32_t none[3] {kNotPatched, kNotPatched, kNotPatched};
	Check(!PatchedOffsetsRead(none, 8, reads), "nothing patched: nothing read");
	const int32_t consumed[3] {kNotPatched, 4, kNotPatched};
	Check(PatchedOffsetsRead(consumed, 8, reads), "s12 is patched and read");
	const int32_t not_consumed[3] {2, 3, kNotPatched};
	Check(!PatchedOffsetsRead(not_consumed, 8, reads), "s10 and s11 are patched but never read");
	const int32_t other_base[3] {kNotPatched, kNotPatched, 0};
	Check(PatchedOffsetsRead(other_base, 8, reads), "the first user SGPR is s8 in the merged stage");
	Check(!PatchedOffsetsRead(other_base, 0, std::array<uint32_t, 1> {5}),
	      "base 0: s0 is not read");
	const int32_t unknown[3] {kNotPatched, kUnknownPatch, kNotPatched};
	Check(PatchedOffsetsRead(unknown, 8, reads), "an unknown register is assumed read");
}

void TestEveryRouteHasAName() {
	for (std::size_t i = 0; i < kRouteNames.size(); ++i) {
		Check(kRouteNames[i] != nullptr && std::strlen(kRouteNames[i]) > 2, "route name");
		for (std::size_t j = i + 1; j < kRouteNames.size(); ++j) {
			Check(std::strcmp(kRouteNames[i], kRouteNames[j]) != 0, "route names are distinct");
		}
	}
}

} // namespace

int main() {
	TestEligibleDrawGoesToTheGpu();
	TestPlainListDrawGoesToTheGpu();
	TestCpuWrittenArgumentsAreTheNormalPath();
	TestEachUnsupportedCaseFallsBackWithItsOwnReason();
	TestPatchedSgprOfAPacketRegister();
	TestPatchedOffsetsRead();
	TestEveryRouteHasAName();
	std::printf("IndirectDrawPolicyTests: ok\n");
	return 0;
}
