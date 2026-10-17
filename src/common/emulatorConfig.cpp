#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/gpuPhaseStats.h"
#include "common/hangWatchdog.h"
#include "common/hleTrace.h"
#include "common/runtimeToggles.h"

#include <algorithm>
#include <memory>
#include <thread>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	Common::RuntimeToggles::Stop();
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100 ||
	        cfg.pad_speaker_volume > 100);

	*g_config = cfg;
	g_config->gpu_indirect_draws_request = cfg.gpu_indirect_draws;
	g_config->poll_before_wait_on = cfg.poll_before_wait_us != 0;

	Common::Hang::Start(cfg.hang_watchdog_seconds);
	Common::GpuPhase::Configure(cfg.gpu_phase_stats_s != 0 ? cfg.gpu_phase_stats_s
	                                                           : cfg.submit_stats_s,
	                            cfg.gpu_phase_stats_s != 0);
	Common::HleTrace::Enable(cfg.hle_trace);
	Bench::Configure({
	    .frames_csv        = cfg.bench_frames_csv,
	    .exit_after_frames = cfg.bench_exit_after_frames,
	    .shader_timing_log = cfg.shader_timing_log,
	    .gpu_trace         = cfg.bench_gpu_trace,
	    .render_csv        = cfg.bench_render_csv,
	    .skip_log          = cfg.debug_skip_log,
	});
	Common::RuntimeToggles::Start(cfg.toggle_control.empty() ? std::string {} : cfg.toggle_control.string());
}

// The flags a harness may flip while the emulator runs (common/runtimeToggles.h). A flag is listed
// here only after every consumer was checked to read it per call; the readers below use Read().
// Correctness flags are never listed.
void RegisterRuntimeToggles(Common::RuntimeToggles::Registry& registry) {
	registry.Register("submit-coalesce", &g_config->submit_coalesce, false);
	registry.Register("elide-barriers", &g_config->elide_barriers, false);
	// Read per draw; a pending barrier stays correct whichever way the flag flips (the next
	// EndRendering records it, and the draws re-check the overlap while rendering continues).
	registry.Register("defer-draw-write-barriers", &g_config->defer_draw_write_barriers, false);
	// Read per dispatch; a pending group barrier is recorded by the next command whichever way the
	// flag flips.
	registry.Register("overlap-dispatches", &g_config->overlap_dispatches, false);
	registry.Register("bda-sync-skip", &g_config->bda_sync_skip, false);
	// Read per PrepareBda call under the mapped-ranges lock; a pass that finds the log unarmed (the
	// flag was off) runs the full walk and arms it, so a toggle at any time costs one full pass.
	registry.Register("bda-dirty-log", &g_config->bda_dirty_log, false);
	// Read per draw; no state: with the flag off every draw polls as before.
	registry.Register("lazy-op-poll", &g_config->lazy_op_poll, false);
	// Read per guest-backing read; entries are checked against the map epoch on every use, so a toggle
	// never leaves a stale entry usable (they are not consulted while off and are re-read after).
	registry.Register("backing-read-cache", &g_config->backing_read_cache, false);
	// Read per write fault; pages marked dirty ahead are uploaded like any dirty page and the real write is seen
	// by the page protection again after the next upload, so a toggle needs no cleanup.
	registry.Register("write-fault-around", &g_config->write_fault_around, false);
	registry.Register("clean-guest-reads", &g_config->clean_guest_reads, false);
	// Read per call under the texture cache lock; a stale entry cannot exist while off because the
	// generation keeps moving whether or not the memo is consulted.
	registry.Register("image-lookup-memo", &g_config->image_lookup_memo, false);
	// A slot's kept binding is checked against the image-set generation on every use, and
	// ResolveTexture stops recording sources while off, so a toggle never leaves a stale source.
	registry.Register("texture-binding-reuse", &g_config->texture_binding_reuse, false);
	// Every use of a resolved part is checked on kyGpu against the live state; parts built while the
	// flag was off are marked unusable, so a toggle at a frame boundary cannot use a stale part.
	registry.Register("resolved-draw", &g_config->resolved_draw, false);
	registry.Register("mapping-memo", &g_config->mapping_memo, false);
	// Read per refresh; the tables it uses are built with the program whatever the flag says, and a
	// table refresh equals the graph walk (--debug-draw-key-verify), so a toggle only picks the path.
	registry.Register("srt-table-walk", &g_config->srt_table_walk, false);
	// Plan keys resync at the next submission; the adopt path validates every plan, so a change in
	// the middle of a submission costs plans (misses), never a wrong draw.
	registry.Register("srt-plans", &g_config->srt_plans, true);
	// The second queue exists only when the flag was on at start-up.
	registry.Register("readback-queue", &g_config->readback_queue, true);
	// The trim thread starts only when the flag was on at start-up and skips its pass while off.
	registry.Register("heap-trim", &g_config->heap_trim, true);
	// Read per dependent read and per recorded dispatch; knowledge recorded while on is forgotten by
	// the same write tracking whether or not the flag is on, and is only consulted while on.
	registry.Register("known-stores", &g_config->known_stores, false);
	// Not read directly: the GPU thread latches the request at a guest flip (LatchGpuIndirectDraws),
	// so the route of a draw packet and its mesh-shader variant agree. The mesh draw-record ring is
	// created on first use, so the flag can be turned on at run time.
	registry.Register("gpu-indirect-draws", &g_config->gpu_indirect_draws_request, false);
	// Read when a copy dispatch is recorded and when a color-metadata range is looked up; what it
	// recorded is forgotten by the same write tracking as known fills, whether or not it is on.
	registry.Register("known-copies", &g_config->known_copies, false);
	// Polled per wait (MasterSemaphore::Wait), no state: on only when --poll-before-wait <us> != 0
	// was given at start-up (the budget stays the configured one).
	registry.Register("poll-before-wait", &g_config->poll_before_wait_on, true);
	// Hot pages and copies in flight are retired whether or not the flag is on (a page whose eager
	// copy is in flight when the flag goes off is finished by the next submit or read fault), so
	// the flag only decides whether new pages are marked hot and new copies scheduled.
	registry.Register("eager-readback", &g_config->eager_readback, false);
	// Read per game-thread read fault; copies in flight are retired by the same code whether or not
	// the flag is on (they share the eager copies' bookkeeping).
	registry.Register("readback-futures", &g_config->readback_futures, false);
}

PipelineCacheKey GetPipelineCacheKey() {
	return g_config->pipeline_cache_key;
}

AsyncPipelineMode GetAsyncPipelineMode() {
	return g_config->async_pipelines;
}

bool GetPipelineLookahead() {
	return g_config->pipeline_lookahead && g_config->async_pipelines != AsyncPipelineMode::Off;
}

bool TextureGcEnabled() {
	return g_config->texture_gc_enabled;
}

bool TextureGcLog() {
	return g_config->texture_gc_log;
}

bool GcClockFrames() {
	return g_config->gc_clock_frames;
}

bool BdaSyncSkip() {
	return Common::RuntimeToggles::Read(g_config->bda_sync_skip);
}

bool WriteFaultAroundEnabled() {
	return Common::RuntimeToggles::Read(g_config->write_fault_around);
}

bool BackingReadCacheEnabled() {
	return Common::RuntimeToggles::Read(g_config->backing_read_cache);
}

bool LazyOpPollEnabled() {
	return Common::RuntimeToggles::Read(g_config->lazy_op_poll);
}

bool BdaDirtyLogEnabled() {
	return Common::RuntimeToggles::Read(g_config->bda_dirty_log);
}

bool SubmitCoalesceEnabled() {
	return Common::RuntimeToggles::Read(g_config->submit_coalesce);
}

uint32_t GetSubmitCoalesceMax() {
	return std::clamp(g_config->submit_coalesce_max, 1u, 256u);
}

uint32_t GetSubmitCoalesceUs() {
	return g_config->submit_coalesce_us;
}

bool GetPipelinesUnoptimizedFirst() {
	return g_config->pipelines_unoptimized_first && g_config->async_pipelines != AsyncPipelineMode::Off;
}

bool GetPrecompileOnLoad() {
	return g_config->precompile_on_load && g_config->async_pipelines != AsyncPipelineMode::Off;
}

bool GetPipelineVariantsReplay() {
	return g_config->pipeline_variants_replay && g_config->async_pipelines != AsyncPipelineMode::Off;
}

bool ElideBarriersEnabled() {
	return Common::RuntimeToggles::Read(g_config->elide_barriers);
}

bool DeferDrawWriteBarriers() {
	return Common::RuntimeToggles::Read(g_config->defer_draw_write_barriers);
}

bool OverlapDispatchesEnabled() {
	return Common::RuntimeToggles::Read(g_config->overlap_dispatches);
}

bool KnownFillsEnabled() {
	return g_config->known_fills;
}

bool KnownCopiesEnabled() {
	return Common::RuntimeToggles::Read(g_config->known_copies);
}

bool KnownStoresEnabled() {
	return Common::RuntimeToggles::Read(g_config->known_stores);
}

uint32_t DebugKnownVerifySeconds() {
	return g_config->debug_known_verify_s;
}

bool DebugKnownVerifyAll() {
	return g_config->debug_known_verify_all;
}

bool ImageLookupMemoEnabled() {
	return Common::RuntimeToggles::Read(g_config->image_lookup_memo);
}

bool TextureBindingReuseEnabled() {
	return Common::RuntimeToggles::Read(g_config->texture_binding_reuse);
}

bool ResolvedDrawEnabled() {
	return Common::RuntimeToggles::Read(g_config->resolved_draw);
}

bool MappingMemoEnabled() {
	return Common::RuntimeToggles::Read(g_config->mapping_memo);
}

bool ResolvedDrawVerify() {
	return g_config->resolved_draw_verify;
}

bool SrtPlanShadowEnabled() {
	return g_config->srt_plan_shadow && g_config->pipeline_lookahead;
}

bool SrtPlansEnabled() {
	return Common::RuntimeToggles::Read(g_config->srt_plans) && g_config->pipeline_lookahead;
}

bool SkipGuardEnabled() {
	return g_config->skip_guard && g_config->async_pipelines == AsyncPipelineMode::Skip;
}

bool SkipGuardRequested() {
	return g_config->skip_guard;
}

bool SkipGuardBdaEnabled() {
	return g_config->skip_guard_bda;
}

bool DrawKeyCacheEnabled() {
	return g_config->draw_key_cache;
}

bool SrtTableWalkEnabled() {
	return Common::RuntimeToggles::Read(g_config->srt_table_walk) || g_config->draw_key_cache;
}

bool DrawKeyVerifyEnabled() {
	return g_config->draw_key_cache && g_config->draw_key_verify;
}

bool CleanGuestReadsEnabled() {
	return Common::RuntimeToggles::Read(g_config->clean_guest_reads);
}

bool GpuIndirectDrawsEnabled() {
	return Common::RuntimeToggles::Read(g_config->gpu_indirect_draws);
}

void LatchGpuIndirectDraws() {
	Common::RuntimeToggles::Write(g_config->gpu_indirect_draws,
	                              Common::RuntimeToggles::Read(g_config->gpu_indirect_draws_request));
}

uint32_t PollBeforeWaitUs() {
	return g_config != nullptr && Common::RuntimeToggles::Read(g_config->poll_before_wait_on)
	           ? g_config->poll_before_wait_us
	           : 0u;
}

bool EagerReadbackEnabled() {
	return Common::RuntimeToggles::Read(g_config->eager_readback);
}

bool ReadbackFuturesEnabled() {
	return Common::RuntimeToggles::Read(g_config->readback_futures);
}

bool EagerReadbackVerifyEnabled() {
	return (Common::RuntimeToggles::Read(g_config->eager_readback) ||
	        Common::RuntimeToggles::Read(g_config->readback_futures)) &&
	       g_config->eager_readback_verify;
}

bool ReadbackQueueEnabled() {
	return Common::RuntimeToggles::Read(g_config->readback_queue);
}

bool GetLookaheadVerify() {
	return g_config->lookahead_verify && g_config->pipeline_lookahead;
}

bool DebugDumpShaderMatches(uint64_t shader_hash) {
	const auto& list = g_config->debug_dump_shaders;
	return std::find(list.begin(), list.end(), shader_hash) != list.end();
}

uint32_t GetShaderLoopLimit() {
	return g_config->shader_loop_limit;
}

uint32_t GetGpuOpsMode() {
	return g_config->gpu_ops_mode;
}

uint32_t GetPipelineLookaheadKinds() {
	return g_config->pipeline_lookahead_kinds;
}

uint32_t GetLookaheadWorkers() {
	return std::min(g_config->lookahead_workers, 16u);
}

uint32_t GetPipelineWorkerCount() {
	if (g_config->pipeline_workers != 0) {
		return g_config->pipeline_workers;
	}
	const auto hardware = std::thread::hardware_concurrency();
	return std::max(1u, std::min(8u, hardware / 2u));
}

uint32_t GetDriverCacheCheckpointSeconds() {
	return g_config->driver_cache_checkpoint_s;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

const std::string& GetAudioInputDevice() {
	return g_config->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return g_config->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return g_config->controller_speaker_volume;
}

uint32_t GetControllerVibrationIntensity() {
	return g_config->controller_vibration_intensity;
}

bool PadSpeakerEnabled() {
	return g_config->pad_speaker_enabled;
}

uint32_t GetPadSpeakerVolume() {
	return g_config->pad_speaker_volume;
}

bool PadSpeakerOsSinkEnabled() {
	return g_config->pad_speaker_os_sink;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool HideCursorEnabled() {
	return g_config->hide_cursor_enabled;
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

GpuTimestampMode GetGpuTimestampMode() {
	return g_config->gpu_timestamps;
}

double GetGpuTimeScale() {
	return std::clamp(g_config->gpu_time_scale, MIN_GPU_TIME_SCALE, MAX_GPU_TIME_SCALE);
}

uint32_t GetFpsLimit() {
	return std::min(g_config->fps_limit, MAX_FPS_LIMIT);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool SyncValidationEnabled() {
	return g_config->debug_sync_validation_enabled && g_config->vulkan_validation_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool SpirvRobustBoundsRequested() {
	return g_config->spirv_robust_bounds;
}

bool SyncRawImageBuffersEnabled() {
	return g_config->sync_raw_image_buffers;
}

bool TessellationEnabled() {
	return g_config->tessellation_enabled;
}

bool TrophyEnabled() {
	return g_config->trophy_enabled;
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return g_config->skip_notice_screen;
}

bool VramPagingEnabled() {
	return g_config->vram_paging;
}

bool VramStatsEnabled() {
	return g_config->vram_stats;
}

bool MemStatsEnabled() {
	return g_config != nullptr && g_config->mem_stats;
}

bool MemReleasePunchEnabled() {
	return g_config != nullptr && g_config->mem_release_punch;
}

bool MemResidencyEnabled() {
	return g_config != nullptr && g_config->mem_residency;
}

bool UploadRingRebarEnabled() {
	return g_config != nullptr && g_config->upload_ring_rebar;
}

bool HeapTrimEnabled() {
	return g_config != nullptr && Common::RuntimeToggles::Read(g_config->heap_trim);
}

bool MemHoleReadsEnabled() {
	return g_config != nullptr && g_config->mem_hole_reads;
}

bool SkipZeroUploadsEnabled() {
	return g_config != nullptr && g_config->skip_zero_uploads;
}

bool VerifyZeroUploadsEnabled() {
	return g_config != nullptr && g_config->verify_zero_uploads;
}

uint32_t VramAuditSeconds() {
	return g_config->vram_audit_s;
}

uint32_t VramBallastMb() {
	return g_config->vram_ballast_mb;
}

uint32_t GcTextureTriggerMb() {
	return g_config->gc_texture_trigger_mb;
}

uint32_t GcTexturePressureMb() {
	return g_config->gc_texture_pressure_mb;
}

uint32_t GcTextureCriticalMb() {
	return g_config->gc_texture_critical_mb;
}

uint32_t GcBufferTriggerMb() {
	return g_config->gc_buffer_trigger_mb;
}

uint32_t GcBufferCriticalMb() {
	return g_config->gc_buffer_critical_mb;
}

uint32_t GcBufferOwnSharePercent() {
	return g_config->gc_buffer_own_share_pct;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return g_config->guest_arguments;
}

} // namespace Config
