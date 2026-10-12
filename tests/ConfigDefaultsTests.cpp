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

} // namespace

int main() {
	Config::Initialize();

	TestGpuTimestampsDefault();
	TestBufferGcOwnShare();
	TestPipelineCacheKeyDefault();

	Config::Shutdown();
	std::printf("ConfigDefaultsTests: ok\n");
	return 0;
}
