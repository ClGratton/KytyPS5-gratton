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
	});
	Common::RuntimeToggles::Start(cfg.toggle_control.empty() ? std::string {} : cfg.toggle_control.string());
}

// The flags a harness may flip while the emulator runs (common/runtimeToggles.h). A flag is listed
// here only after every consumer was checked to read it per call; the readers below use Read().
// Correctness flags are never listed.
void RegisterRuntimeToggles(Common::RuntimeToggles::Registry& registry) {
	registry.Register("submit-coalesce", &g_config->submit_coalesce, false);
	registry.Register("elide-barriers", &g_config->elide_barriers, false);
	registry.Register("bda-sync-skip", &g_config->bda_sync_skip, false);
	registry.Register("clean-guest-reads", &g_config->clean_guest_reads, false);
	// Plan keys resync at the next submission; the adopt path validates every plan, so a change in
	// the middle of a submission costs plans (misses), never a wrong draw.
	registry.Register("srt-plans", &g_config->srt_plans, true);
	// The second queue exists only when the flag was on at start-up.
	registry.Register("readback-queue", &g_config->readback_queue, true);
	// The trim thread starts only when the flag was on at start-up and skips its pass while off.
	registry.Register("heap-trim", &g_config->heap_trim, true);
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

bool KnownFillsEnabled() {
	return g_config->known_fills;
}

bool SrtPlanShadowEnabled() {
	return g_config->srt_plan_shadow && g_config->pipeline_lookahead;
}

bool SrtPlansEnabled() {
	return Common::RuntimeToggles::Read(g_config->srt_plans) && g_config->pipeline_lookahead;
}

bool DrawKeyCacheEnabled() {
	return g_config->draw_key_cache;
}

bool SrtTableWalkEnabled() {
	return g_config->srt_table_walk || g_config->draw_key_cache;
}

bool DrawKeyVerifyEnabled() {
	return g_config->draw_key_cache && g_config->draw_key_verify;
}

bool CleanGuestReadsEnabled() {
	return Common::RuntimeToggles::Read(g_config->clean_guest_reads);
}

bool GpuIndirectDrawsEnabled() {
	return g_config->gpu_indirect_draws;
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

uint32_t GetPipelineWorkerCount() {
	if (g_config->pipeline_workers != 0) {
		return g_config->pipeline_workers;
	}
	const auto hardware = std::thread::hardware_concurrency();
	return std::max(1u, std::min(8u, hardware / 2u));
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
