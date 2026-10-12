#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class LogDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

// What the on-disk driver VkPipelineCache signature is keyed on (see pipelineCache.cpp).
//   Revision: stock behaviour. The git revision is part of the signature, so every commit drops
//             the cache, and non-Release / unknown-revision / dirty builds disable it entirely.
//   Schema:   a schema-version constant plus vendor/device/driverVersion/pipelineCacheUUID. The
//             cache survives emulator revisions and is also enabled in dev (non-Release, dirty)
//             builds. Opt-in via --pipeline-cache-key schema.
enum class PipelineCacheKey { Revision, Schema };

// Where shader translation and Vulkan pipeline creation run (--async-pipelines, shaders.md C1a).
//   Off:   stock behaviour. Everything is synchronous on the GPU thread.
//   Stall: a miss submits a self-contained job to the worker pool and the draw/dispatch waits for
//          it. Same result as Off, but the work runs on a worker (foundation for lookahead).
//   Skip:  a graphics draw whose program or pipeline is not ready is skipped (KytyPlus
//          behaviour); compute dispatches always wait.
enum class AsyncPipelineMode { Off, Stall, Skip };
// Where the 64-bit GPU timestamps a guest reads (EVENT_WRITE_EOP / COPY_DATA clock sources) come
// from: Legacy = the clock when the emulator parses the packet, Gpu (default) = the host GPU's own
// timestamp of that point in the command stream, mapped onto the guest GPU clock. Gpu falls back to
// the parse-time value when the device cannot calibrate timestamps (logged once).
enum class GpuTimestampMode { Legacy, Gpu };

// The share --gc-buffer-own-share is meant to be set to (K42); not the default, see below.
constexpr uint32_t RECOMMENDED_GC_BUFFER_OWN_SHARE_PCT = 25;

constexpr double MIN_GPU_TIME_SCALE = 0.1;
constexpr double MAX_GPU_TIME_SCALE = 16.0;
constexpr uint32_t MAX_FPS_LIMIT    = 1000;

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id != USER_ID_EVERYONE && user_id != USER_ID_SYSTEM;
}

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	std::string            audio_input_device;
	std::optional<ControllerColor> controller_color;
	uint32_t               controller_speaker_volume      = 50;
	uint32_t               controller_vibration_intensity = 100;
	// Whether the game's controller-speaker audio goes to the DualSense (otherwise it plays on the
	// normal output), its hardware level (0-100; the raw report volume) and whether a Bluetooth pad's speaker is
	// exposed as a controllable PipeWire/PulseAudio sink on Linux.
	bool                   pad_speaker_enabled            = true;
	uint32_t               pad_speaker_volume             = 15;
	bool                   pad_speaker_os_sink            = true;
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hide_cursor_enabled         = false;
	bool                   splash_enabled              = true;
	bool                   vr_enabled                  = false;
	bool                   amd_cpu_enabled             = false;
	uint32_t               vblank_frequency            = 60;
	// Default Gpu: guest-visible GPU time follows the host GPU's own execution (accurate emulation);
	// --gpu-timestamps legacy restores the command-processor clock. docs/defaults.md.
	GpuTimestampMode       gpu_timestamps              = GpuTimestampMode::Gpu;
	// Multiplier on the GPU durations a guest observes (1.0 = off); see --gpu-time-scale.
	double                 gpu_time_scale              = 1.0;
	// Presented frames per second cap (0 = off); see --fps-limit.
	uint32_t               fps_limit                   = 0;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	LogDirection           printf_direction            = LogDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   trophy_enabled              = true;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
	bool                   vram_paging                 = false;
	bool                   vram_stats                  = false;
	bool                   mem_stats                   = false;
	bool                   mem_release_punch           = false;
	uint32_t               vram_ballast_mb             = 0;
	uint32_t               vram_audit_s                = 0;
	// GC thresholds in MiB; 0 keeps the budget-derived stock value.
	uint32_t               gc_texture_trigger_mb       = 0;
	uint32_t               gc_texture_pressure_mb      = 0;
	uint32_t               gc_texture_critical_mb      = 0;
	uint32_t               gc_buffer_trigger_mb        = 0;
	uint32_t               gc_buffer_critical_mb       = 0;
	// --gc-buffer-own-share <pct> (K42): the buffer GC only goes aggressive (downloads dirty buffers,
	// a full drain each) when the buffer cache itself holds this share of the critical budget, a
	// share of the budget-derived mark so it scales with the card. 0 (default) = the old rule: whole-
	// device usage alone decides, which the texture cache can pin above the mark. Stays 0 because
	// with --vram-paging (default) the gate keeps ~370 MiB more buffers resident and, at a forced
	// 5.7 GB budget, Astro Bot drops from 35 to 12.5 fps (docs/defaults.md); 25 is for cards where
	// the old rule's drains are the problem and paging is off.
	uint32_t               gc_buffer_own_share_pct     = 0;
	// Observation-only benchmark instrumentation (see common/benchInstrument.h). Off by default.
	std::filesystem::path  bench_frames_csv;
	uint64_t               bench_exit_after_frames     = 0;
	std::filesystem::path  shader_timing_log;
	bool                   bench_gpu_trace             = false;
	PipelineCacheKey       pipeline_cache_key          = PipelineCacheKey::Revision;
	AsyncPipelineMode      async_pipelines             = AsyncPipelineMode::Off;
	// --pipeline-workers: 0 = automatic, min(8, hardware threads / 2), at least 1.
	uint32_t               pipeline_workers            = 0;
	// --pipeline-lookahead: prefetch pipelines from submitted command buffers (needs
	// --async-pipelines stall or skip). Default off.
	bool                   pipeline_lookahead          = false;
	// --debug-hang-watchdog <seconds>: write a thread/pool/lookahead state report when no frame
	// was presented for that long (common/hangWatchdog.h). 0 = off.
	uint32_t               hang_watchdog_seconds       = 0;
	// --pipeline-lookahead-kinds <all|compute|graphics|none>: debug aid to bisect a misprediction.
	// Bit 0 = prefetch graphics draws, bit 1 = prefetch compute dispatches,
	// bit 2 = programs only (do not request pipelines).
	uint32_t               pipeline_lookahead_kinds    = 3;
	// --skip-ir-validation: skip the shader IR consistency checks in release translations (about a
	// fifth of translate time; the output is byte-identical). Debug builds always validate and
	// KYTY_IR_VALIDATE forces it on. Default off (= validate, stock behaviour).
	bool                   skip_ir_validation          = false;
	// --pipelines-unoptimized-first: a pipeline no cache holds is created with
	// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT so its draw can proceed, and the optimized build
	// replaces it from a background worker. Needs --async-pipelines stall|skip; NVIDIA only unless
	// KYTY_PIPELINE_FAST_BUILD=1. Default off.
	bool                   pipelines_unoptimized_first = false;
	// --debug-lookahead-verify: the GPU thread recompiles every program a prefetch built before
	// its first use and compares the SPIR-V; mismatches are logged (finds prefetch inputs that
	// differ from the real draw's). Slow, debug only.
	bool                   lookahead_verify            = false;
	// --debug-texture-gc <true|false>: false disables the texture garbage collector (debug:
	// tells whether a rendering difference comes from an evicted GPU-only image).
	bool                   texture_gc_enabled          = true;
	// --debug-texture-gc log: log every free of a GPU-written image by the texture GC (and the
	// out-of-memory reclaim) with the flags that decided it.
	bool                   texture_gc_log              = false;
	// --gpu-phase-stats <seconds>: report where the GPU thread spends its time every N seconds
	// (common/gpuPhaseStats.h). 0 = off.
	uint32_t               gpu_phase_stats_s           = 0;
	// --submit-stats <seconds>: only the frame/submit/coalescing counters, no timers. 0 = off.
	uint32_t               submit_stats_s              = 0;
	// --submit-coalesce (K13): hold back the submit of RELEASE_MEM end-of-pipe events (up to
	// submit_coalesce_max events, or an age derived from the vblank period). Default off.
	bool                   submit_coalesce             = false;
	// --bda-sync-skip (K15): PrepareBda leaves out its walk over all buffers when no page became
	// CPU-modified, no buffer was (un)registered and the mapping did not change since the last full
	// pass (host_gpu/bdaSyncGate.h). Default off.
	bool                   bda_sync_skip               = false;
	// --gc-clock <runs|frames> (K43): the texture/buffer GC ages in collector runs (stock) or in
	// presented frames (renderer/cache/gcClock.h).
	bool                   gc_clock_frames             = false;
	uint32_t               submit_coalesce_max         = 16;
	// 0 = derive from the vblank frequency (period / 8); otherwise microseconds.
	uint32_t               submit_coalesce_us          = 0;
	// --elide-barriers (K14): leave out a guest global barrier when nothing was recorded since the
	// previous one in the same command buffer. Default off.
	bool                   elide_barriers              = false;
	// --known-fills (K22): remember ranges last written by one GPU uniform fill, so color-metadata
	// clear discovery needs no GPU->CPU readback for them. Default off.
	bool                   known_fills                 = false;
	// --clean-guest-reads (K20): the GPU thread reads shader metadata and vertex tables through the
	// guest backing, without faulting, when none of the bytes is GPU-written. Default off.
	bool                   clean_guest_reads           = false;
	// --readback-queue (K20): synchronous GPU->guest buffer readbacks copy on a second queue of the
	// rendering family and wait only for the work that wrote the data. Default off.
	bool                   readback_queue              = false;
	std::filesystem::path  bench_render_csv;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
	// Ordinary boots need a non-null argv[0]; --guest-exec supplies the caller's arguments.
	std::vector<std::string> guest_arguments {""};
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
uint32_t GetControllerSpeakerVolume();
uint32_t GetControllerVibrationIntensity();
bool     PadSpeakerEnabled();
uint32_t GetPadSpeakerVolume();
bool     PadSpeakerOsSinkEnabled();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HideCursorEnabled();
bool     VrEnabled();
bool     AmdCpuEnabled();
uint32_t GetVblankFrequency();
GpuTimestampMode GetGpuTimestampMode();
double   GetGpuTimeScale();
uint32_t GetFpsLimit();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
LogDirection           GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();

PipelineCacheKey GetPipelineCacheKey();
AsyncPipelineMode GetAsyncPipelineMode();
// Resolved worker count (never 0).
uint32_t          GetPipelineWorkerCount();
bool              GetPipelineLookahead();
// The flag, effective only with --async-pipelines on (the optimized rebuilds run on the pool).
bool              GetPipelinesUnoptimizedFirst();
uint32_t          GetPipelineLookaheadKinds();
bool              GetLookaheadVerify();
bool              TextureGcEnabled();
bool              TextureGcLog();
bool              SubmitCoalesceEnabled();
bool              BdaSyncSkip();
bool              GcClockFrames();
uint32_t          GetSubmitCoalesceMax();
uint32_t          GetSubmitCoalesceUs();
bool              ElideBarriersEnabled();
bool              KnownFillsEnabled();
bool              CleanGuestReadsEnabled();
bool              ReadbackQueueEnabled();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool TessellationEnabled();
bool TrophyEnabled();
bool PlayGoHackEnabled();
bool SkipNoticeScreen();
bool VramPagingEnabled();
bool VramStatsEnabled();
// Host-RAM diagnostics for guest memory release (log only) and the opt-in hole punch. Both are
// safe to call before the config exists (they report false).
bool MemStatsEnabled();
bool MemReleasePunchEnabled();
uint32_t VramBallastMb();
uint32_t VramAuditSeconds();
uint32_t GcTextureTriggerMb();
uint32_t GcTexturePressureMb();
uint32_t GcTextureCriticalMb();
uint32_t GcBufferTriggerMb();
uint32_t GcBufferCriticalMb();
uint32_t GcBufferOwnSharePercent();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
