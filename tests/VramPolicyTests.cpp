#include "graphics/host_gpu/vramPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Libs::Graphics::VramPolicy;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "VramPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

// VkFormat values used below.
constexpr int R8_UNORM         = 9;
constexpr int R16G16B16A16_SFL = 97;
constexpr int R8G8B8A8_UNORM   = 37;
constexpr int BC1_RGBA_UNORM   = 133;
constexpr int BC7_UNORM        = 145;

void TestEstimateImageBytes() {
	// 4K RGBA16F render target: 8 bytes per texel.
	Check(EstimateImageBytes({3840, 2160, 1, 1, 1, 1, R16G16B16A16_SFL}) == 3840ull * 2160 * 8,
	      "RGBA16F 4K size");
	// BC1 is half a byte per texel, other block formats one byte.
	Check(EstimateImageBytes({1024, 1024, 1, 1, 1, 1, BC1_RGBA_UNORM}) == 512ull * 1024, "BC1 size");
	Check(EstimateImageBytes({1024, 1024, 1, 1, 1, 1, BC7_UNORM}) == 1024ull * 1024, "BC7 size");
	Check(EstimateImageBytes({512, 512, 1, 1, 1, 1, R8_UNORM}) == 512ull * 512, "R8 size");
	// A mip chain adds a third, layers and samples multiply.
	const auto base   = EstimateImageBytes({1024, 1024, 1, 1, 1, 1, R8G8B8A8_UNORM});
	const auto mipped = EstimateImageBytes({1024, 1024, 1, 1, 13, 1, R8G8B8A8_UNORM});
	Check(base == 4ull << 20 && mipped == base * 4 / 3, "mip chain adds one third");
	Check(EstimateImageBytes({1024, 1024, 1, 6, 1, 1, R8G8B8A8_UNORM}) == 6 * base, "layers");
	Check(EstimateImageBytes({1024, 1024, 1, 1, 1, 4, R8G8B8A8_UNORM}) == 4 * base, "samples");
	Check(EstimateImageBytes({1024, 1024, 1, 1, 1, 0, R8G8B8A8_UNORM}) == base, "samples 0 counts as 1");
}

void TestDedicatedThreshold() {
	Check(!UseDedicatedAllocation(DedicatedImageMinBytes - 1, 0), "just below the threshold is pooled");
	Check(UseDedicatedAllocation(DedicatedImageMinBytes, 0), "the threshold itself is dedicated");
	Check(UseDedicatedAllocation(1 * GiB, 0), "a large image is dedicated");
	Check(UseDedicatedAllocation(1 * GiB, MaxDedicatedImages - 1), "one slot left");
	Check(!UseDedicatedAllocation(1 * GiB, MaxDedicatedImages), "the cap falls back to pooled blocks");
	Check(!UseDedicatedAllocation(0, 0), "empty image is pooled");
}

void TestImagePriority() {
	const auto depth   = ImagePriority(true, false);
	const auto target  = ImagePriority(false, true);
	const auto texture = ImagePriority(false, false);
	Check(depth.priority == 1.0f && depth.dedicated, "depth: top priority, own allocation");
	Check(target.priority == 0.75f && target.dedicated, "render target: high priority, own allocation");
	Check(texture.priority == 0.5f && !texture.dedicated, "texture: the Vulkan default, pooled");
	Check(depth.priority > target.priority && target.priority > texture.priority,
	      "depth is paged out last, textures first");
	// Depth wins when an image is classified as both.
	Check(ImagePriority(true, true).priority == 1.0f, "depth beats render target");
	for (const auto choice: {depth, target, texture}) {
		Check(choice.priority >= 0.0f && choice.priority <= 1.0f, "priority is in 0..1");
	}
}

void TestGcThresholds() {
	// Pinned values: the 8 GiB and 12 GiB cards the thresholds are tuned against.
	const auto eight = ComputeGcThresholds(static_cast<int64_t>(8 * GiB));
	Check(eight.trigger == 0, "8 GiB: the trigger is 0 (the texture GC always runs)");
	Check(eight.pressure == 8 * GiB - 6 * 8 * GiB / 10, "8 GiB: pressure");
	Check(eight.critical == 8 * GiB - 2 * 8 * GiB / 10, "8 GiB: critical");
	const auto twelve = ComputeGcThresholds(static_cast<int64_t>(12 * GiB));
	Check(twelve.trigger == 2 * GiB, "12 GiB: trigger is half of what exceeds 8 GiB");
	Check(twelve.pressure == 12 * GiB - 6 * 8 * GiB / 10, "12 GiB: pressure");
	Check(twelve.critical == 12 * GiB - 2 * 8 * GiB / 10, "12 GiB: critical");
	// Derived from the budget: ordered, bounded by it and monotonic for any size.
	GcThresholds previous {};
	for (uint64_t mib = 0; mib <= 96 * 1024; mib += 256) {
		const auto t = ComputeGcThresholds(static_cast<int64_t>(mib * MiB));
		Check(t.trigger <= t.pressure && t.pressure <= t.critical, "thresholds are ordered");
		Check(t.pressure >= 3 * GiB / 2 && t.critical >= 3 * GiB, "floors");
		if (mib * MiB >= 4 * GiB) {
			Check(t.pressure < mib * MiB && t.critical < mib * MiB,
			      "above 4 GiB the thresholds stay under the budget");
		}
		Check(t.trigger >= previous.trigger && t.pressure >= previous.pressure &&
		          t.critical >= previous.critical,
		      "thresholds never decrease with the budget");
		previous = t;
	}
	const auto none = ComputeGcThresholds(-5);
	Check(none.trigger == 0, "negative budget clamps the trigger to 0");
}

struct FakeAllocator {
	// Stages that succeed.
	bool normal       = false;
	bool after_reclaim = false;
	bool host         = false;
	std::vector<AllocStage> attempts;
	int                     reclaims = 0;
	bool                    TryAllocate(AllocStage stage) {
        attempts.push_back(stage);
        switch (stage) {
			case AllocStage::Normal: return normal;
			case AllocStage::AfterReclaim: return after_reclaim;
			case AllocStage::HostFallback: return host;
		}
        return false;
	}
};

AllocOutcome Run(FakeAllocator& fake, bool can_reclaim) {
	return RunAllocationLadder([&](AllocStage stage) { return fake.TryAllocate(stage); },
	                           [&] { fake.reclaims++; }, can_reclaim);
}

void TestAllocationLadder() {
	{
		FakeAllocator fake {.normal = true};
		const auto    out = Run(fake, true);
		Check(out.ok && out.stage == AllocStage::Normal && !out.reclaimed, "first try succeeds");
		Check(fake.reclaims == 0 && fake.attempts.size() == 1, "no reclaim when the first try works");
	}
	{
		FakeAllocator fake {.after_reclaim = true};
		const auto    out = Run(fake, true);
		Check(out.ok && out.stage == AllocStage::AfterReclaim && out.reclaimed,
		      "reclaim then retry succeeds");
		Check(fake.reclaims == 1 && fake.attempts.size() == 2, "exactly one reclaim and one retry");
	}
	{
		FakeAllocator fake {.host = true};
		const auto    out = Run(fake, true);
		Check(out.ok && out.stage == AllocStage::HostFallback && out.reclaimed,
		      "the host-memory last resort rescues the allocation");
		Check(fake.attempts.size() == 3 && fake.attempts[0] == AllocStage::Normal &&
		          fake.attempts[1] == AllocStage::AfterReclaim &&
		          fake.attempts[2] == AllocStage::HostFallback,
		      "stages are tried in order");
		Check(fake.reclaims == 1, "reclaim runs once, not before every stage");
	}
	{
		FakeAllocator fake {};
		const auto    out = Run(fake, true);
		Check(!out.ok && out.stage == AllocStage::HostFallback, "everything failing reports failure");
		Check(fake.attempts.size() == 3, "no endless retrying");
	}
	{
		// No reclaim hook, or already inside a reclaim: skip straight to the last resort.
		FakeAllocator fake {.after_reclaim = true, .host = true};
		const auto    out = Run(fake, false);
		Check(out.ok && out.stage == AllocStage::HostFallback && !out.reclaimed,
		      "without a reclaim hook the retry stage is skipped");
		Check(fake.reclaims == 0 && fake.attempts.size() == 2, "no reclaim call, no retry attempt");
	}
}

} // namespace

void TestUploadRingPlan() {
	// A 12 GiB card with ReBAR: the ring goes to VRAM when asked, never when not.
	Check(PlanUploadRing(true, 12 * GiB, 12 * GiB).device_local, "ReBAR on a 12 GiB card");
	Check(!PlanUploadRing(false, 12 * GiB, 12 * GiB).device_local, "ReBAR not wanted");
	// The legacy 256 MiB window cannot hold the ring, and no host-visible device-local heap at all.
	Check(!PlanUploadRing(true, 12 * GiB, 256 * MiB).device_local, "256 MiB BAR window");
	Check(!PlanUploadRing(true, 12 * GiB, 0).device_local, "no host-visible device-local heap");
	// 8 GiB is the smallest card that gives away 1/16 of its VRAM; below that the ring stays in RAM.
	Check(PlanUploadRing(true, 8 * GiB, 8 * GiB).device_local, "8 GiB card");
	Check(!PlanUploadRing(true, 6 * GiB, 6 * GiB).device_local, "6 GiB card keeps the ring in RAM");
	Check(!PlanUploadRing(true, 4 * GiB, 4 * GiB).device_local, "4 GiB card keeps the ring in RAM");
	// A small BAR that is just one ring is refused, two rings is the minimum.
	Check(!PlanUploadRing(true, 12 * GiB, UploadRingBytes).device_local, "BAR as large as the ring");
	Check(PlanUploadRing(true, 12 * GiB, 2 * UploadRingBytes).device_local, "BAR of two rings");
	Check(PlanUploadRing(true, 12 * GiB, 0).size == UploadRingBytes, "ring size is the nominal one");
}

int main() {
	TestUploadRingPlan();
	TestEstimateImageBytes();
	TestDedicatedThreshold();
	TestImagePriority();
	TestGcThresholds();
	TestAllocationLadder();
	std::printf("VramPolicyTests: ok\n");
	return 0;
}
