#include "graphics/host_gpu/renderer/indirectDrawArgs.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::IndirectDrawArgs;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "IndirectDrawArgsTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr MeshShape TriangleList {3, 3, 85, 65535, 65535, 4194304};

void TestClampMatchesCpuPath() {
	Check(ClampIndexCount(1000, 600) == 600, "count above INDEX_BUFFER_SIZE is clamped");
	Check(ClampIndexCount(500, 600) == 500, "count inside the buffer is kept");
	Check(ClampIndexCount(1000, 0) == 1000, "unknown buffer size does not clamp");
	Check(ClampIndexCount(0, 600) == 0, "empty draw stays empty");
}

void TestGridOfTriangleList() {
	Check((MeshGrid(3, 1, TriangleList) == MeshCommand {1, 1}), "one triangle");
	Check((MeshGrid(255, 1, TriangleList) == MeshCommand {1, 1}), "85 triangles fill one group");
	Check((MeshGrid(258, 1, TriangleList) == MeshCommand {2, 1}), "86 triangles need two groups");
	Check((MeshGrid(258, 7, TriangleList) == MeshCommand {2, 7}), "instances pass through");
	Check((MeshGrid(2, 1, TriangleList) == MeshCommand {0, 0}), "fewer vertices than a primitive");
	Check((MeshGrid(0, 1, TriangleList) == MeshCommand {0, 0}), "no vertices");
	Check((MeshGrid(300, 0, TriangleList) == MeshCommand {0, 0}), "no instances");
}

void TestGridOfStrip() {
	// Triangle strip: size 3, step 1.
	const MeshShape strip {3, 1, 64, 65535, 65535, 4194304};
	Check((MeshGrid(3, 1, strip) == MeshCommand {1, 1}), "one strip triangle");
	Check((MeshGrid(66, 1, strip) == MeshCommand {1, 1}), "64 strip triangles");
	Check((MeshGrid(67, 1, strip) == MeshCommand {2, 1}), "65 strip triangles");
}

void TestGridClampsToHostLimits() {
	const MeshShape small {3, 3, 1, 10, 4, 24};
	Check((MeshGrid(3 * 100, 1, small) == MeshCommand {10, 1}), "groups clamp to the X limit");
	Check((MeshGrid(3, 100, small) == MeshCommand {1, 4}), "instances clamp to the Y limit");
	Check((MeshGrid(3 * 10, 4, small) == MeshCommand {10, 2}), "total group count limit");
	const MeshShape tiny {3, 3, 1, 10, 4, 5};
	Check((MeshGrid(3 * 10, 4, tiny) == MeshCommand {0, 0}),
	      "a total below one row of groups draws nothing rather than exceed the limit");
}

void TestMeshRecordFollowsCpuPath() {
	// Index count 1000 clamped to 600, 3 instances, first index 5, base vertex -2, first instance 7.
	const IndexedArgs args {1000, 3, 5, 0xfffffffeu, 7};
	const auto        out = BuildMesh(args, 2, 0x0000000912345670ull, 600, TriangleList, true);
	Check(out.record[0] == 600, "record: clamped index count");
	Check(out.record[1] == 0xfffffffeu, "record: base vertex");
	Check(out.record[2] == 7, "record: first instance");
	Check(out.record[3] == 2, "record: index element size");
	const uint64_t address = 0x0000000912345670ull + 5u * 2u;
	Check(out.record[4] == static_cast<uint32_t>(address), "record: index address low");
	Check(out.record[5] == static_cast<uint32_t>(address >> 32u), "record: index address high");
	Check(out.record[6] == 0, "record: base group of a sliced dispatch is 0 (the GPU clamps instead)");
	Check((out.command == MeshCommand {3, 3}), "command: 200 triangles at 85 per group, 3 instances");
}

void TestNativeFetchKeepsOffsetsOutOfTheRecord() {
	const IndexedArgs args {300, 1, 0, 123, 9};
	const auto        out = BuildMesh(args, 4, 0x1000, 0, TriangleList, false);
	Check(out.record[1] == 0 && out.record[2] == 0,
	      "native fetch: the patched user SGPRs carry the offsets, not the record");
	Check(out.record[0] == 300 && out.record[3] == 4, "native fetch: the rest is unchanged");
}

void TestAddressCarriesIntoHighWord() {
	// 0xfffffff0 + 0x60000000 * 4 = 0x2_7ffffff0: the offset itself overflows 32 bits and the sum carries.
	const IndexedArgs args {3, 1, 0x60000000u, 0, 0};
	const auto        out = BuildMesh(args, 4, 0x00000000fffffff0ull, 0, TriangleList, true);
	const uint64_t    address = 0xfffffff0ull + (uint64_t {0x60000000u} * 4u);
	Check(out.record[4] == static_cast<uint32_t>(address), "carry: low word");
	Check(out.record[5] == static_cast<uint32_t>(address >> 32u), "carry: high word");
	Check(out.record[5] == 2, "carry: high word value");
}

void TestPlainCommandFollowsCpuPath() {
	const IndexedArgs args {1000, 3, 5, 0xfffffffeu, 7};
	Check((BuildPlain(args, 192, true) == PlainCommand {192, 3, 5, 0xfffffffeu, 7}),
	      "plain: count clamped, everything else from the arguments");
	Check((BuildPlain(args, 0, true) == PlainCommand {1000, 3, 5, 0xfffffffeu, 7}),
	      "plain: unknown INDEX_BUFFER_SIZE does not clamp");
	Check((BuildPlain(args, 4096, false) == PlainCommand {1000, 3, 5, 0, 0}),
	      "plain, native fetch: the patched user SGPRs carry the offsets, the command carries 0");
	Check((BuildPlain({0, 0, 0, 0, 0}, 192, true) == PlainCommand {0, 0, 0, 0, 0}), "empty draw");
}

} // namespace

int main() {
	TestClampMatchesCpuPath();
	TestGridOfTriangleList();
	TestGridOfStrip();
	TestGridClampsToHostLimits();
	TestMeshRecordFollowsCpuPath();
	TestNativeFetchKeepsOffsetsOutOfTheRecord();
	TestAddressCarriesIntoHighWord();
	TestPlainCommandFollowsCpuPath();
	std::printf("IndirectDrawArgsTests: ok\n");
	return 0;
}
