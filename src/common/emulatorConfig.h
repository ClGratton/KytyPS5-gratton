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
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   vr_enabled                  = false;
	bool                   amd_cpu_enabled             = false;
	uint32_t               vblank_frequency            = 60;
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
	bool                   tessellation_enabled        = false;
	bool                   playgo_hack_enabled         = false;
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
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     VrEnabled();
bool     AmdCpuEnabled();
uint32_t GetVblankFrequency();
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

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool TessellationEnabled();
bool PlayGoHackEnabled();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
