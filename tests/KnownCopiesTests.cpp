#include "graphics/host_gpu/renderer/cache/knownCopies.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;
using Libs::Graphics::ShaderRecompiler::IR::UniformCopy;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "KnownCopiesTests: failed: %s\n", message);
		std::abort();
	}
}

UniformCopy Copy() {
	UniformCopy c;
	c.valid        = true;
	c.src_resource = 0;
	c.dst_resource = 1;
	c.group_stride = 64;
	return c;
}

CopyDescriptor Desc(uint64_t base, uint32_t records) {
	CopyDescriptor d;
	d.base        = base;
	d.num_records = records;
	d.stride      = 4;
	d.words       = 1;
	d.dst_sel     = kIdentityDstSel;
	d.mapped      = true;
	return d;
}

CopyLaunch Launch(uint32_t groups) {
	CopyLaunch l;
	l.groups[0]  = groups;
	l.groups[1]  = 1;
	l.groups[2]  = 1;
	l.threads[0] = 64;
	l.threads[1] = 1;
	l.threads[2] = 1;
	l.use_plain_mode = true;
	return l;
}

void TestResolve() {
	KnownCopy out;
	Check(ResolveKnownCopy(Copy(), Desc(0x100000, 57344), Desc(0x400000, 57344), Launch(896), out) &&
	          out.dst == 0x400000 && out.src == 0x100000 && out.bytes == 57344 * 4,
	      "a copy of whole buffers");
	Check(ResolveKnownCopy(Copy(), Desc(0x100000, 57344), Desc(0x400000, 57344), Launch(100), out) &&
	          out.bytes == 6400 * 4,
	      "a copy of the first part");
	Check(!ResolveKnownCopy(Copy(), Desc(0x100000, 6000), Desc(0x400000, 57344), Launch(896), out),
	      "invocations past the source: they would load zeros");
	Check(!ResolveKnownCopy(Copy(), Desc(0x100000, 57344), Desc(0x400000, 6000), Launch(896), out),
	      "invocations past the destination");
	Check(!ResolveKnownCopy(Copy(), Desc(0x100000, 57344), Desc(0x120000, 57344), Launch(896), out),
	      "overlapping ranges");
	UniformCopy bad = Copy();
	bad.valid = false;
	Check(!ResolveKnownCopy(bad, Desc(0x10000, 64), Desc(0x40000, 64), Launch(1), out), "not a copy shader");
	auto src = Desc(0x10000, 64), dst = Desc(0x40000, 64);
	dst.words = 2;
	dst.stride = 8;
	Check(!ResolveKnownCopy(Copy(), src, dst, Launch(1), out), "different formats");
	src = Desc(0x10000, 64);
	dst = Desc(0x40000, 64);
	src.words = 0;
	dst.words = 0;
	Check(!ResolveKnownCopy(Copy(), src, dst, Launch(1), out), "a format that is not a plain 32-bit integer");
	src = Desc(0x10000, 64);
	src.stride = 8;
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "stride that is not the record size");
	src = Desc(0x10000, 64);
	src.dst_sel = 0;
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "a component select that is not the identity");
	src = Desc(0x10000, 64);
	src.swizzle = true;
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "swizzled");
	src = Desc(0x10000, 64);
	src.add_tid = true;
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "add-tid");
	src = Desc(0x10000, 64);
	src.mapped = false;
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "partly unmapped");
	src = Desc(0x10002, 64);
	Check(!ResolveKnownCopy(Copy(), src, Desc(0x40000, 64), Launch(1), out), "unaligned");
	auto l = Launch(1);
	l.threads[0] = 32;
	Check(!ResolveKnownCopy(Copy(), Desc(0x10000, 64), Desc(0x40000, 64), l, out), "group size that is not the index stride");
	l = Launch(1);
	l.groups[1] = 2;
	Check(!ResolveKnownCopy(Copy(), Desc(0x10000, 64), Desc(0x40000, 64), l, out), "a 2D dispatch");
	l = Launch(1);
	l.thread_dimensions = true;
	Check(!ResolveKnownCopy(Copy(), Desc(0x10000, 64), Desc(0x40000, 64), l, out), "a dispatch counted in threads");
	l = Launch(1);
	l.use_plain_mode = false;
	Check(!ResolveKnownCopy(Copy(), Desc(0x10000, 64), Desc(0x40000, 64), l, out), "another dispatch initiator");
	l = Launch(0);
	Check(!ResolveKnownCopy(Copy(), Desc(0x10000, 64), Desc(0x40000, 64), l, out), "no groups");
	// Four components: records of 16 bytes.
	src = Desc(0x10000, 64);
	dst = Desc(0x40000, 64);
	src.words = dst.words = 4;
	src.stride = dst.stride = 16;
	Check(ResolveKnownCopy(Copy(), src, dst, Launch(1), out) && out.bytes == 64 * 16, "four 32-bit components");
}

void TestRuns() {
	std::vector<DwordRun> runs;
	const uint32_t uniform[8] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
	                             0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
	Check(CollectRuns(uniform, 4, runs) && runs.size() == 1 && runs[0].offset == 0 && runs[0].bytes == 32 &&
	          runs[0].value == 0xffffffffu,
	      "one run");
	const uint32_t slices[8] = {1, 1, 1, 2, 2, 2, 2, 7};
	Check(CollectRuns(slices, 4, runs) && runs.size() == 3 && runs[1].offset == 12 && runs[1].bytes == 16 &&
	          runs[1].value == 2 && runs[2].offset == 28 && runs[2].bytes == 4,
	      "runs in order");
	Check(!CollectRuns(slices, 2, runs), "more runs than the bound");
	Check(!CollectRuns({}, 2, runs), "no data");
}

} // namespace

int main() {
	TestResolve();
	TestRuns();
	std::printf("KnownCopiesTests: ok\n");
	return 0;
}
