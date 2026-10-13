// Defaults of the emulator options (docs/defaults.md). A default is part of the emulator's
// behaviour: these tests pin the values the emulator starts with when no flag is given, and that
// the opt-out flags still reach the config.

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/cache/bufferGcGate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ConfigDefaultsTests: failed: %s\n", message);
		std::abort();
	}
}

// Loads `options` as main() does after parsing the command line.
void Apply(const Config::ConfigOptions& options) {
	Config::Load(options);
}

// Correctness: the guest reads GPU timestamps from the host GPU's execution, not from the time the
// command processor parsed the packet (an accurate emulation; Legacy is the opt-out).
void TestGpuTimestampsDefault() {
	const Config::ConfigOptions options;
	Check(options.gpu_timestamps == Config::GpuTimestampMode::Gpu, "gpu_timestamps defaults to gpu");
	Apply(options);
	Check(Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Gpu,
	      "an unconfigured emulator reads real GPU timestamps");

	Config::ConfigOptions legacy;
	legacy.gpu_timestamps = Config::GpuTimestampMode::Legacy;
	Apply(legacy);
	Check(Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Legacy,
	      "--gpu-timestamps legacy still selects the parse-time clock");
	Check(Config::GetGpuTimeScale() == 1.0, "the time scale stays off by default");
}

// K42: the buffer GC goes aggressive (dirty-buffer downloads, a full GPU drain per batch) only when
// the buffer cache itself holds a share of the critical budget. Measured and left opt-in: with
// --vram-paging (the default) it keeps more buffers resident and Astro Bot under a forced 5.7 GB
// budget falls from 35 to 12.5 fps. The share is of the budget-derived mark, never a byte count.
void TestBufferGcOwnShare() {
	namespace Gate = Libs::Graphics::BufferGcGate;
	constexpr uint64_t MiB = 1024ull * 1024;
	const Config::ConfigOptions options;
	Check(options.gc_buffer_own_share_pct == 0, "gc_buffer_own_share_pct defaults to 0 (the old rule)");
	Check(Config::RECOMMENDED_GC_BUFFER_OWN_SHARE_PCT == Gate::DefaultOwnSharePercent,
	      "the recommended share is the gate's default share");
	Apply(options);
	Check(Config::GcBufferOwnSharePercent() == 0, "an unconfigured emulator uses the old rule");
	Check(Gate::Aggressive(5000 * MiB, 3912 * MiB, 0, Config::GcBufferOwnSharePercent()),
	      "share 0: device usage alone makes the buffer GC aggressive");

	Config::ConfigOptions gated;
	gated.gc_buffer_own_share_pct = Config::RECOMMENDED_GC_BUFFER_OWN_SHARE_PCT;
	Apply(gated);
	Check(Config::GcBufferOwnSharePercent() == 25, "--gc-buffer-own-share 25 reaches the config");
	for (const uint64_t critical: {2048 * MiB, 3912 * MiB, 12000 * MiB}) {
		const uint64_t device = critical + 1500 * MiB;
		Check(!Gate::Aggressive(device, critical, critical / 10, Config::GcBufferOwnSharePercent()),
		      "device pinned by the texture cache, small buffer cache: not aggressive");
		Check(Gate::Aggressive(device, critical, critical / 4, Config::GcBufferOwnSharePercent()),
		      "a quarter of the budget in buffers: aggressive");
		Check(!Gate::Aggressive(critical - 1, critical, critical, Config::GcBufferOwnSharePercent()),
		      "below the critical mark is never aggressive");
	}
}

// Correctness of the first-play experience: the driver pipeline cache survives emulator builds.
// The invalidation rules (device, driver, schema version, damage, size cap) are pinned in
// PipelineCacheFileTests.
void TestPipelineCacheKeyDefault() {
	const Config::ConfigOptions options;
	Check(options.pipeline_cache_key == Config::PipelineCacheKey::Schema,
	      "pipeline_cache_key defaults to schema");
	Apply(options);
	Check(Config::GetPipelineCacheKey() == Config::PipelineCacheKey::Schema,
	      "an unconfigured emulator keys the pipeline cache on the schema");

	Config::ConfigOptions stock;
	stock.pipeline_cache_key = Config::PipelineCacheKey::Revision;
	Apply(stock);
	Check(Config::GetPipelineCacheKey() == Config::PipelineCacheKey::Revision,
	      "--pipeline-cache-key revision still opts out");
}

// --vram-paging used to switch on the VRAM stats log (every 5 s, and each pipeline compile).
// Stats are diagnostics and have their own flag.
void TestVramStatsAreIndependentOfPaging() {
	Config::ConfigOptions options;
	options.vram_paging = true;
	options.vram_stats  = false;
	Apply(options);
	Check(Config::VramPagingEnabled() && !Config::VramStatsEnabled(),
	      "paging alone does not turn the stats log on");
	options.vram_paging = false;
	options.vram_stats  = true;
	Apply(options);
	Check(!Config::VramPagingEnabled() && Config::VramStatsEnabled(), "stats alone work");
}

// Safety net: with memory priorities and pageable device-local memory the driver pages
// low-priority VRAM out under pressure instead of failing an image allocation. It only
// does something on a device with both extensions (the device setup logs when it does not); the
// allocation ladder underneath (reclaim, retry, host fallback) works either way.
void TestVramPagingDefault() {
	const Config::ConfigOptions options;
	Check(options.vram_paging, "vram_paging defaults to on");
	Check(!options.vram_stats, "the VRAM stats log stays off by default");
	Apply(options);
	Check(Config::VramPagingEnabled(), "an unconfigured emulator requests VRAM paging");
	Check(!Config::VramStatsEnabled(), "and does not log VRAM stats");

	Config::ConfigOptions off;
	off.vram_paging = false;
	Apply(off);
	Check(!Config::VramPagingEnabled(), "--vram-paging off still opts out");
}

// Measured, not assumed: readback of every GPU-written linear image on submit cost Astro Bot
// 60 % of its frame rate (49 -> 19 fps), so it stays opt-in until it downloads only what the CPU
// reads. A title that needs it (GTA V) passes --readback-linear-images true.
void TestReadbackLinearImagesStaysOff() {
	const Config::ConfigOptions options;
	Check(!options.readback_linear_images, "readback_linear_images defaults to off");
	Apply(options);
	Check(!Config::ReadbackLinearImagesEnabled(), "an unconfigured emulator does not read linear images back");

	Config::ConfigOptions on;
	on.readback_linear_images = true;
	Apply(on);
	Check(Config::ReadbackLinearImagesEnabled(), "--readback-linear-images true still enables it");
}

} // namespace

// --generic-compute is a performance option: off, and it leaves the short binaries to the dispatch.
void TestGenericComputeDefaults() {
	const Config::ConfigOptions options;
	Check(!options.generic_compute, "--generic-compute defaults to off");
	Check(options.generic_compute_min_dwords == 512, "generic compute skips binaries under 512 dwords");
	Apply(options);
	Check(!Config::GetGenericCompute() && Config::GetGenericComputeMinDwords() == 512,
	      "an unconfigured emulator builds no generic variants");
}

int main() {
	Config::Initialize();

	TestGpuTimestampsDefault();
	TestBufferGcOwnShare();
	TestReadbackLinearImagesStaysOff();
	TestPipelineCacheKeyDefault();
	TestVramStatsAreIndependentOfPaging();
	TestVramPagingDefault();
	TestGenericComputeDefaults();

	Config::Shutdown();
	std::printf("ConfigDefaultsTests: ok\n");
	return 0;
}
