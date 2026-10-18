// Defaults of the emulator options (docs/defaults.md). A default is part of the emulator's
// behaviour: these tests pin the values the emulator starts with when no flag is given, and that
// the opt-out flags still reach the config.

#include "common/emulatorConfig.h"
#include "common/runtimeToggles.h"
#include "graphics/host_gpu/renderer/cache/bufferGcGate.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

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
// Correctness: the data of an end-of-pipe packet (the fences a title polls) becomes visible when the GPU has
// finished the work, not when the packet is parsed (an accurate emulation; off is the opt-out).
void TestEopLabelsAtCompletionDefault() {
	const Config::ConfigOptions options;
	Check(options.eop_labels_at_completion, "eop_labels_at_completion defaults to on");
	Apply(options);
	Check(Config::EopLabelsAtCompletionEnabled(), "an unconfigured emulator publishes end-of-pipe labels at completion");

	Config::ConfigOptions off;
	off.eop_labels_at_completion = false;
	Apply(off);
	Check(!Config::EopLabelsAtCompletionEnabled(), "end-of-pipe labels can be published at parse time again");
}

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
// Performance/durability: the driver pipeline cache is written only at shutdown unless asked.
void TestDriverCacheCheckpointDefaultsOff() {
	const Config::ConfigOptions options;
	Check(options.driver_cache_checkpoint_s == 0, "driver_cache_checkpoint_s defaults to off");
	Apply(options);
	Check(Config::GetDriverCacheCheckpointSeconds() == 0,
	      "an unconfigured emulator does not checkpoint the driver pipeline cache");
	Config::ConfigOptions on;
	on.driver_cache_checkpoint_s = 60;
	Apply(on);
	Check(Config::GetDriverCacheCheckpointSeconds() == 60, "the checkpoint period reaches the config");
	Apply(options);
}

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

// Host-RAM features (ram.md), measured on the level route (notes/mem-footprint.md): reading never-written
// guest memory with pread, the ReBAR upload ring (placed by VramPolicy::PlanUploadRing, with a system-RAM
// fallback), clearing never-written images instead of uploading them and trimming the host heap are on.
// --mem-residency only counts, so it stays off, as does the zero-upload verifier.
void TestHostMemoryFeaturesDefaults() {
	const Config::ConfigOptions options;
	Check(options.mem_hole_reads && options.upload_ring_rebar && options.skip_zero_uploads &&
	          options.heap_trim,
	      "the measured host-memory features default to on");
	Check(!options.mem_residency && !options.verify_zero_uploads,
	      "the diagnostics default to off");
	Apply(options);
	Check(Config::MemHoleReadsEnabled() && Config::UploadRingRebarEnabled() &&
	          Config::SkipZeroUploadsEnabled() && Config::HeapTrimEnabled(),
	      "an unconfigured emulator has the host-memory features on");
	Check(!Config::MemResidencyEnabled() && !Config::VerifyZeroUploadsEnabled(),
	      "an unconfigured emulator keeps the diagnostics off");

	Config::ConfigOptions off;
	off.mem_hole_reads = off.upload_ring_rebar = off.skip_zero_uploads = off.heap_trim = false;
	Apply(off);
	Check(!Config::MemHoleReadsEnabled() && !Config::UploadRingRebarEnabled() &&
	          !Config::SkipZeroUploadsEnabled() && !Config::HeapTrimEnabled(),
	      "each host-memory feature can be switched off");
}

// --toggle-control: a control file flips the registered flags through the same accessors the
// emulator reads, at the frame hook, and the applied changes land in <control>.applied.csv.
void TestRuntimeToggleControlFile() {
	namespace RT = Common::RuntimeToggles;
	const auto dir = std::filesystem::temp_directory_path() / "kyty-toggle-test";
	std::filesystem::create_directories(dir);
	const auto control = dir / "toggles.ctl";
	std::filesystem::remove(control);
	std::filesystem::remove(dir / "toggles.ctl.applied.csv");

	Config::ConfigOptions options;
	options.srt_plans = options.pipeline_lookahead = options.submit_coalesce = true;
	options.readback_queue = false;
	options.toggle_control = control;
	Apply(options);
	Check(RT::Active(), "--toggle-control starts the toggles");
	Check(Config::SrtPlansEnabled() && Config::SubmitCoalesceEnabled(), "start values");

	{
		std::ofstream out(dir / "toggles.tmp");
		out << "# seq 1\nsrt-plans=false\nsubmit-coalesce=off\nreadback-queue=true\ndraw-key-cache=true\n";
	}
	std::filesystem::rename(dir / "toggles.tmp", control);
	for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	Check(RT::detail::g_pending.load(), "the watcher saw the control file");
	Check(Config::SrtPlansEnabled(), "nothing changes before the frame boundary");
	RT::OnFrame(42);
	Check(!Config::SrtPlansEnabled() && !Config::SubmitCoalesceEnabled(), "srt-plans and submit-coalesce are off");
	Check(!Config::ReadbackQueueEnabled(), "readback-queue was off at start-up: it cannot be turned on");
	std::ifstream csv(dir / "toggles.ctl.applied.csv");
	std::stringstream text;
	text << csv.rdbuf();
	Check(text.str().find(",42,srt-plans,1,0,applied") != std::string::npos, "the change is logged with its frame");
	Check(text.str().find("readback-queue,0,1,not_armed_at_startup") != std::string::npos, "the refusal is logged");
	Check(text.str().find("draw-key-cache") != std::string::npos &&
	          text.str().find("unknown_or_not_runtime_safe") != std::string::npos,
	      "a flag that is not registered is refused");

	// eager-readback and poll-before-wait: flipped through the accessors the consumers read.
	{
		Config::ConfigOptions eager;
		eager.eager_readback      = true;
		eager.poll_before_wait_us = 100;
		eager.toggle_control      = control;
		Apply(eager);
		Check(Config::EagerReadbackEnabled() && Config::PollBeforeWaitUs() == 100, "start values");
		{
			std::ofstream out(dir / "toggles.tmp");
			out << "# seq 2\neager-readback=off\npoll-before-wait=off\n";
		}
		std::filesystem::rename(dir / "toggles.tmp", control);
		for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		RT::OnFrame(43);
		Check(!Config::EagerReadbackEnabled() && !Config::EagerReadbackVerifyEnabled() &&
		          Config::PollBeforeWaitUs() == 0,
		      "eager-readback and poll-before-wait are off");
		{
			std::ofstream out(dir / "toggles.tmp");
			out << "# seq 3\neager-readback=on\npoll-before-wait=on\n";
		}
		std::filesystem::rename(dir / "toggles.tmp", control);
		for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		RT::OnFrame(44);
		Check(Config::EagerReadbackEnabled() && Config::PollBeforeWaitUs() == 100,
		      "they can be turned on again; the poll budget is the configured one");
		// Off at start-up: poll-before-wait cannot be turned on (it has no budget).
		Config::ConfigOptions no_poll;
		no_poll.toggle_control = control;
		Apply(no_poll);
		{
			std::ofstream out(dir / "toggles.tmp");
			out << "# seq 4\npoll-before-wait=on\n";
		}
		std::filesystem::rename(dir / "toggles.tmp", control);
		for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		RT::OnFrame(45);
		Check(Config::PollBeforeWaitUs() == 0, "poll-before-wait without a budget stays off");
	}

	// readback-futures: flipped at run time in both directions, from either start value.
	{
		Config::ConfigOptions plain;
		plain.readback_futures = false;
		plain.toggle_control   = control;
		Apply(plain);
		Check(!Config::ReadbackFuturesEnabled(), "readback-futures starts off when the config says so");
		for (int seq = 5; seq <= 6; seq++) {
			{
				std::ofstream out(dir / "toggles.tmp");
				out << "# seq " << seq << "\nreadback-futures=" << (seq == 5 ? "on" : "off") << "\n";
			}
			std::filesystem::rename(dir / "toggles.tmp", control);
			for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			RT::OnFrame(50 + seq);
			Check(Config::ReadbackFuturesEnabled() == (seq == 5),
			      "readback-futures follows the control file");
		}
	}

	Apply(Config::ConfigOptions {});
	Check(!RT::Active(), "loading a config without --toggle-control stops it");
	std::filesystem::remove_all(dir);
}

// --known-stores, --known-copies and --gpu-indirect-draws are exact performance features: off by
// default, runtime toggles. gpu-indirect-draws is latched by the GPU thread at a guest flip, so the
// route of a draw packet and the mesh variant it builds agree.
void TestExactChainToggles() {
	namespace RT = Common::RuntimeToggles;
	const Config::ConfigOptions defaults;
	Check(!defaults.known_stores && !defaults.known_copies && !defaults.gpu_indirect_draws &&
	          defaults.debug_known_verify_s == 0,
	      "the exact drain cuts default to off, the verifier too");
	const auto dir = std::filesystem::temp_directory_path() / "kyty-toggle-test-chain";
	std::filesystem::create_directories(dir);
	const auto control = dir / "toggles.ctl";
	std::filesystem::remove(control);
	std::filesystem::remove(dir / "toggles.ctl.applied.csv");
	Config::ConfigOptions options;
	options.toggle_control = control;
	Apply(options);
	Check(!Config::KnownStoresEnabled() && !Config::KnownCopiesEnabled() && !Config::GpuIndirectDrawsEnabled(),
	      "off at start-up");
	{
		std::ofstream out(dir / "toggles.tmp");
		out << "# seq 1\nknown-stores=on\nknown-copies=on\ngpu-indirect-draws=on\n";
	}
	std::filesystem::rename(dir / "toggles.tmp", control);
	for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	RT::OnFrame(7);
	Check(Config::KnownStoresEnabled() && Config::KnownCopiesEnabled(), "the two drain cuts are on after the frame boundary");
	Check(!Config::GpuIndirectDrawsEnabled(), "gpu-indirect-draws waits for the GPU thread to latch it");
	Config::LatchGpuIndirectDraws();
	Check(Config::GpuIndirectDrawsEnabled(), "latched at a guest flip");
	{
		std::ofstream out(dir / "toggles.tmp");
		out << "# seq 2\nknown-stores=off\ngpu-indirect-draws=off\n";
	}
	std::filesystem::rename(dir / "toggles.tmp", control);
	for (int i = 0; i < 100 && !RT::detail::g_pending.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	RT::OnFrame(8);
	Check(!Config::KnownStoresEnabled() && Config::KnownCopiesEnabled(), "each one flips on its own");
	Check(Config::GpuIndirectDrawsEnabled(), "still latched on until the next flip");
	Config::LatchGpuIndirectDraws();
	Check(!Config::GpuIndirectDrawsEnabled(), "latched off");
	Apply(Config::ConfigOptions {});
	std::filesystem::remove_all(dir);
}

// --skip-guard is a narrowing of the opt-in skip mode: off by default, and it only acts with skip.
void TestSkipGuardDefaults() {
	const Config::ConfigOptions options;
	Check(!options.skip_guard && options.skip_guard_bda, "skip guard is off, its 64-bit rule is on");
	Apply(options);
	Check(!Config::SkipGuardEnabled(), "an unconfigured emulator has the skip guard off");

	Config::ConfigOptions on;
	on.skip_guard = true;
	Apply(on);
	Check(!Config::SkipGuardEnabled() && Config::SkipGuardRequested(),
	      "the guard does nothing without --async-pipelines skip");
	on.async_pipelines = Config::AsyncPipelineMode::Stall;
	Apply(on);
	Check(!Config::SkipGuardEnabled(), "the guard does nothing in stall mode");
	on.async_pipelines = Config::AsyncPipelineMode::Skip;
	Apply(on);
	Check(Config::SkipGuardEnabled() && Config::SkipGuardBdaEnabled(), "skip + guard enables it");
	on.skip_guard_bda = false;
	Apply(on);
	Check(Config::SkipGuardEnabled() && !Config::SkipGuardBdaEnabled(), "the 64-bit rule can be off");
}

// gratton-play-31 (docs/defaults.md): every exact performance flag with a measured gain starts on, and keeps its opt-out. The flags
// that stay off are the ones with an open correctness question (the exact chain, --occlusion-queries), --submit-coalesce (gratton-play-32:
// it causes one-frame lighting flashes and disappearing meshes) and the ones whose gain was not measured.
void TestPerformanceFlagsDefaultOn() {
	const Config::ConfigOptions o;
	Check(o.async_pipelines == Config::AsyncPipelineMode::Stall && o.pipeline_lookahead, "async pipelines stall + look-ahead");
	Check(!o.submit_coalesce && o.gc_clock_frames, "the frame GC clock on, submit coalescing off (it causes lighting flashes)");
	Check(o.eop_labels_at_completion, "end-of-pipe labels at completion (correctness)");
	Check(o.elide_barriers && o.known_fills && o.clean_guest_reads && o.defer_draw_write_barriers && o.overlap_dispatches,
	      "the drain and barrier cuts");
	Check(o.srt_plans && o.srt_table_walk && o.resolved_draw && o.mapping_memo && o.readback_futures, "the draw path");
	Check(o.image_lookup_memo && o.texture_binding_reuse && o.draw_state_memo && o.push_descriptor_delta && o.draw_key_cache,
	      "the memo flags");
	Check(o.bda_dirty_log && o.lazy_op_poll && o.backing_read_cache && o.write_fault_around && o.plan_prefetch &&
	          o.tracker_lockfree && o.tracker_sleeping_locks && o.image_presence && o.image_pool,
	      "the perf-stack");
	Check(o.scalable_read_locks && o.submit_thread && o.small_copy_dedup && o.async_upload_copies && o.texture_lock_batch,
	      "the threading and copy cuts");
	Check(o.ro_depth_access && o.compute_image_copy, "the GPU-time cuts");
	Check(o.plan_pool && o.lookahead_adaptive && o.desc_commit_diet && o.skip_ir_validation, "the host-CPU and diet cuts");
	Check(o.lookahead_workers == 4 && !o.lookahead_workers_explicit, "four look-ahead workers by default");

	Check(!o.known_stores && !o.known_copies && !o.gpu_indirect_draws, "the exact chain stays off");
	Check(!o.cp_direct_stores && !o.bda_sync_skip && !o.readback_queue && !o.eager_readback, "flags without a measured gain stay off");

	// The opt-outs reach the config and the accessors.
	Apply(o);
	Check(Config::SubmitThreadEnabled() && Config::GetPipelineLookahead(), "an unconfigured emulator runs the recommended set");
	Config::ConfigOptions off;
	off.submit_thread = off.pipeline_lookahead = false;
	off.async_pipelines = Config::AsyncPipelineMode::Off;
	Apply(off);
	Check(!Config::SubmitThreadEnabled() && !Config::GetPipelineLookahead(), "the opt-outs still switch them off");

	// A count given on the command line is used as given; the default scales with the host.
	Config::ConfigOptions workers;
	workers.lookahead_workers          = 12;
	workers.lookahead_workers_explicit = true;
	Apply(workers);
	Check(Config::GetLookaheadWorkers() == 12, "an explicit --lookahead-workers count is not scaled");
	const auto hardware = std::thread::hardware_concurrency();
	Apply(o);
	Check(Config::GetLookaheadWorkers() == (hardware == 0 ? 4u : std::min(4u, hardware / 4u)),
	      "the default count is a quarter of the hardware threads, at most 4");
	Apply(Config::ConfigOptions {});
}

int main() {
	Config::Initialize();

	TestPerformanceFlagsDefaultOn();

	TestSkipGuardDefaults();
	TestGpuTimestampsDefault();
	TestEopLabelsAtCompletionDefault();
	TestBufferGcOwnShare();
	TestReadbackLinearImagesStaysOff();
	TestPipelineCacheKeyDefault();
	TestDriverCacheCheckpointDefaultsOff();
	TestVramStatsAreIndependentOfPaging();
	TestVramPagingDefault();
	TestHostMemoryFeaturesDefaults();
	TestRuntimeToggleControlFile();
	TestExactChainToggles();

	Config::Shutdown();
	std::printf("ConfigDefaultsTests: ok\n");
	return 0;
}
