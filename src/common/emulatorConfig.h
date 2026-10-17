#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/loopGuard.h"

namespace Common::RuntimeToggles {
class Registry;
}

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

// What the on-disk driver VkPipelineCache signature is keyed on (see pipelineCacheFile.h).
//   Revision: stock behaviour. The git revision is part of the signature, so every commit drops
//             the cache, and non-Release / unknown-revision / dirty builds disable it entirely.
//             --pipeline-cache-key revision.
//   Schema:   (default) a schema-version constant plus vendor/device/driverVersion/
//             pipelineCacheUUID. The cache survives emulator revisions and is also enabled in dev
//             (non-Release, dirty) builds. The driver addresses every entry by the whole pipeline
//             creation input, so a build that changes the SPIR-V or the state only misses; a
//             different GPU, driver, layout (schema version) or a damaged file drops the cache.
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
	// --debug-sync-validation: the validation layer's synchronization validation (needs the layer,
	// implies --vulkan-validation); hazards are logged, never fatal. Very slow.
	bool                   debug_sync_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	// Stays false: downloading every GPU-written linear image on submit makes a game that renders
	// into linear memory a GPU drain per frame (Astro Bot: 49 -> 19 fps, 16.7 -> 50 ms frames, same
	// binary). A game whose CPU reads GPU-written linear memory (GTA V vehicle deformation) needs it;
	// the real fix is to download only what the CPU actually touches. docs/defaults.md.
	bool                   readback_linear_images      = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   trophy_enabled              = true;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
	// Default true: VK_EXT_memory_priority + VK_EXT_pageable_device_local_memory (when the device has
	// them) so the driver pages low-priority VRAM out instead of failing an allocation. A safety net
	// under the hardened allocation ladder (reclaim, retry, host fallback), which stays on either way.
	bool                   vram_paging                 = true;
	bool                   vram_stats                  = false;
	bool                   mem_stats                   = false;
	bool                   mem_release_punch           = false;
	// Log-only host RAM residency diagnostics (mincore per GPU-registered range class, CPU faults on
	// clean uploaded pages, reads of never-written guest memory). Costs a few read-probe faults.
	bool                   mem_residency               = false;
	// Hand free pages of the host allocator back to the kernel (malloc_trim) when a lot of freed memory
	// sits in its arenas. See common/heapTrim.h.
	bool                   heap_trim                   = true;
	// Read never-written guest memory with pread instead of through the writable mapping, so the read
	// does not make the zero pages resident (a read of a hole in a shared mapping allocates them).
	bool                   mem_hole_reads              = true;
	// Put the 512 MiB upload staging ring in device-local host-visible memory (ReBAR) instead of system
	// RAM, when the device offers it. The ring is only written sequentially by the CPU.
	bool                   upload_ring_rebar           = true;
	// Do not read and upload a color image whose guest range was never written (it reads as zeros):
	// clear the GPU image instead. Needs no flag to find the range, but a read of the same range
	// through the mapping would make it resident, so it works best with mem_hole_reads.
	bool                   skip_zero_uploads           = true;
	// Debug: when a range is skipped as never written, also read it with pread and log loudly if it
	// holds a non-zero byte.
	bool                   verify_zero_uploads         = false;
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
	PipelineCacheKey       pipeline_cache_key          = PipelineCacheKey::Schema;
	AsyncPipelineMode      async_pipelines             = AsyncPipelineMode::Off;
	// --pipeline-workers: 0 = automatic, min(8, hardware threads / 2), at least 1.
	uint32_t               pipeline_workers            = 0;
	// --driver-cache-checkpoint <seconds>: while pipelines are being created, a background thread
	// writes the driver pipeline cache to disk this often (and not at all while nothing new was
	// compiled), so a crash or hang keeps what the session compiled. 0 = only at shutdown. Default
	// off until measured.
	uint32_t               driver_cache_checkpoint_s   = 0;
	// --pipeline-lookahead: prefetch pipelines from submitted command buffers (needs
	// --async-pipelines stall or skip). Default off.
	bool                   pipeline_lookahead          = false;
	// --debug-hang-watchdog <seconds>: write a thread/pool/lookahead state report when no frame
	// was presented for that long (common/hangWatchdog.h). 0 = off.
	uint32_t               hang_watchdog_seconds       = 0;
	// --debug-gpu-ops <off|ring|checkpoints>: 0 off, 1 host-side ring of the last draws and
	// dispatches (printed when the GPU stops answering), 2 plus VK_NV_device_diagnostic_checkpoints.
	uint32_t               gpu_ops_mode                = 0;
	// --shader-loop-limit <N>: every shader invocation gets N loop-header executions (all loops
	// together); when they are used up the loops are left and the shader is reported loudly. A
	// guard against a GPU hang from a loop that never ends because of an emulation bug (see
	// common/loopGuard.h). On by default; 0 = off.
	uint32_t               shader_loop_limit           = Common::LoopGuard::DefaultLimit;
	// --debug-dump-shader <hex[,hex...]>: write the SPIR-V and the guest binary of just these shader
	// hashes (as printed by --debug-gpu-ops) to --shader-log-folder, without --graphics-debug-dump.
	std::vector<uint64_t>  debug_dump_shaders;
	// --debug-hle-trace: keep a per-thread ring of the last HLE calls and print it in the fatal
	// fault report (common/hleTrace.h). Default off.
	bool                   hle_trace                   = false;
	// --pipeline-lookahead-kinds <all|compute|graphics|none>: debug aid to bisect a misprediction.
	// Bit 0 = prefetch graphics draws, bit 1 = prefetch compute dispatches,
	// bit 2 = programs only (do not request pipelines).
	uint32_t               pipeline_lookahead_kinds    = 3;
	// --lookahead-workers <0-16>: 0 runs the prefetch of every draw on the lookahead thread itself;
	// N > 0 makes that thread a sequencer that decodes the PM4 stream and hands each draw, with a
	// copy of its registers, to N prefetch workers (notes/resolved-draw.md). Default 0.
	uint32_t               lookahead_workers           = 0;
	// --skip-ir-validation: skip the shader IR consistency checks in release translations (about a
	// fifth of translate time; the output is byte-identical). Debug builds always validate and
	// KYTY_IR_VALIDATE forces it on. Default off (= validate, stock behaviour).
	bool                   skip_ir_validation          = false;
	// --spirv-robust-bounds: leave storage-buffer loads to robustBufferAccess2 instead of emitting
	// an `if (index < OpArrayLength)` block around each one (shorter SPIR-V, less driver compile
	// time; an out-of-bounds load reads zero either way). Stores and atomics keep their checks.
	// Needs a device with robustBufferAccess2 and robustStorageBufferAccessSizeAlignment <= 4;
	// otherwise the explicit checks stay.
	// Default on since the measurement in docs/emitter-shape-notes.md; --spirv-robust-bounds false
	// restores the explicit checks.
	bool                   spirv_robust_bounds         = true;
	// --pipelines-unoptimized-first: a pipeline no cache holds is created with
	// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT so its draw can proceed, and the optimized build
	// replaces it from a background worker. Needs --async-pipelines stall|skip; NVIDIA only unless
	// KYTY_PIPELINE_FAST_BUILD=1. Default off.
	bool                   pipelines_unoptimized_first = false;
	// --precompile-on-load: when the guest registers a compute shader (AgcCreateShader) its program
	// and pipeline are built on the worker pool's low-priority queue for the most likely variant,
	// seconds before the first dispatch. Needs --async-pipelines stall|skip. Default off.
	bool                   precompile_on_load          = false;
	// --pipeline-variants-replay: record the compute variants the game uses in
	// _PipelineCache/<TITLE>.variants and, in the next session, build every stored variant of a
	// shader when the guest registers it. Needs --async-pipelines stall|skip. Default off.
	bool                   pipeline_variants_replay    = false;
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
	// --bda-dirty-log: PrepareBda synchronizes only the buffers that overlap a range that became
	// CPU-modified (or got a buffer) since its last pass, from a log kept by the region manager
	// (host_gpu/bdaDirtyLog.h). Exact: the pass it replaces finds nothing else. Default off.
	bool                   bda_dirty_log               = false;
	// --lazy-op-poll: the per-draw poll for finished deferred operations queries the GPU timeline at
	// most every 100 us and never waits for the priority-operations thread (commandScheduler.h). Default off.
	bool                   lazy_op_poll                = false;
	// --backing-read-cache: TryReadBacking remembers the mapping entry of a read per thread, valid while no
	// mapping changed (kernel/memoryAddressSpace.inc), instead of taking the map lock and walking the tree. Default off.
	bool                   backing_read_cache          = false;
	// --gc-clock <runs|frames> (K43): the texture/buffer GC ages in collector runs (stock) or in
	// presented frames (renderer/cache/gcClock.h).
	bool                   gc_clock_frames             = false;
	uint32_t               submit_coalesce_max         = 16;
	// 0 = derive from the vblank frequency (period / 8); otherwise microseconds.
	uint32_t               submit_coalesce_us          = 0;
	// --elide-barriers (K14): leave out a guest global barrier when nothing was recorded since the
	// previous one in the same command buffer. Default off.
	bool                   elide_barriers              = false;
	// --defer-draw-write-barriers: a draw whose shaders write buffers leaves the barrier that makes
	// the writes visible pending (and keeps the render pass open) until something may read them
	// (renderer/pendingWrites.h). Default off.
	bool                   defer_draw_write_barriers   = false;
	// --overlap-dispatches: consecutive direct compute dispatches whose bound resources do not
	// conflict run without a barrier between them (renderer/dispatchHazard.h). Default off.
	bool                   overlap_dispatches          = false;
	// --known-fills (K22): remember ranges last written by one GPU uniform fill, so color-metadata
	// clear discovery needs no GPU->CPU readback for them. Default off.
	bool                   known_fills                 = false;
	// --known-stores (performance, exact): the GPU thread takes the value of a dword a dispatch
	// stored as an immediate (queue headers, flags) from what the dispatch is proven to write
	// (renderer/cache/knownStores.h) instead of draining the GPU when it has to read that dword
	// itself (the SRT walk). Runtime toggle. Default off until measured.
	bool                   known_stores                = false;
	// --known-copies (performance, exact): a compute dispatch proven to copy records from one buffer
	// to another (the color-metadata copy of fast-cleared targets) leaves the destination known when
	// the CPU can read the source without waiting; clear discovery then needs no GPU->CPU readback
	// for the slices it knows. Runtime toggle. Default off until measured.
	bool                   known_copies                = false;
	// --debug-known-verify <seconds>: with --known-stores, once per this many seconds per kind a
	// known value is not used: the GPU is drained, the real bytes are read and compared (loud log and
	// the knowledge dropped on a mismatch; counters in --gpu-phase-stats). 0 = off.
	uint32_t               debug_known_verify_s        = 0;
	// --debug-known-verify-all: verify every known answer (a GPU drain each; a correctness run, not a
	// benchmark).
	bool                   debug_known_verify_all      = false;
	// --image-lookup-memo: FindImage remembers unique matches until the set of registered images
	// changes (renderer/cache/imageLookupMemo.h). Exact; default off until measured.
	bool                   image_lookup_memo           = false;
	// --texture-binding-reuse: a draw keeps a slot's image binding while the descriptor, the shader's
	// image resource and the set of cached images are unchanged. Exact; default off until measured.
	bool                   texture_binding_reuse       = false;
	// --resolved-draw: kyGpu takes the parts of a draw that the lookahead computed ahead of time
	// (read hull, ...; notes/resolved-draw.md) after checking them; --resolved-draw-verify also
	// computes each one inline and stops on a difference. Exact; default off until measured.
	bool                   resolved_draw               = false;
	bool                   resolved_draw_verify        = false;
	bool                   mapping_memo                = false;
	// --draw-key-cache: a most-recently-used memo of the last graphics pipelines found, compared by
	// key before the hash and map lookup. Default off. --debug-draw-key-verify also runs the
	// full lookup on every memo hit and stops on a disagreement.
	bool                   draw_key_cache              = false;
	// --srt-table-walk: refresh a native-eligible shader's flattened SRT from a table through the
	// guest-memory callback instead of walking the value graph. Default off.
	bool                   srt_table_walk              = false;
	bool                   draw_key_verify             = false;
	// --srt-plan-shadow (measurement only): the lookahead thread keeps the materialized SRT of
	// every draw it looks at as a plan, and kyGpu compares each plan with its own result
	// (readiness, lead, validity, byte equality). Nothing is adopted. Needs --pipeline-lookahead.
	bool                   srt_plan_shadow             = false;
	// --srt-plans (performance): kyGpu uses the plan the lookahead materialized for a draw, after a
	// validity check, instead of materializing the draw's SRT itself; it falls back to its own path
	// when the check fails or no plan is there. Needs --pipeline-lookahead. Default off.
	bool                   srt_plans                   = false;
	// --skip-guard: with --async-pipelines skip, learn from the readers of the memory skipped draws
	// should have written (descriptor-table walks, indirect arguments, CP DMA, CPU readbacks,
	// index/vertex data); once one reads such memory, later draws that write it wait for their
	// pipeline instead of being skipped. Loud log lines and a summary (skipDependencies.h). Skip
	// stays an opt-in bridge; this narrows what it may drop. Default off.
	bool                   skip_guard                  = false;
	// --skip-guard-64bit: with --skip-guard, a shader that loads through 64-bit addresses and also
	// reads a skipped draw's output makes that output unsafe too (those loads are invisible to the
	// tracker). Default on; needs --skip-guard.
	bool                   skip_guard_bda              = true;
	// --clean-guest-reads (K20): the GPU thread reads shader metadata and vertex tables through the
	// guest backing, without faulting, when none of the bytes is GPU-written. Default off.
	bool                   clean_guest_reads           = false;
	// --gpu-indirect-draws (K21): indirect draws whose argument block a GPU pass wrote are built on
	// the GPU instead of being read on the CPU after a full drain: a one-thread argument pass, then
	// vkCmdDrawIndexedIndirect (point/line/triangle lists) or vkCmdDrawMeshTasksIndirectEXT
	// (mesh-emulated draws). Default off.
	bool                   gpu_indirect_draws          = false;
	// Runtime request for --gpu-indirect-draws (--toggle-control): the GPU thread copies it into
	// gpu_indirect_draws at a guest flip (LatchGpuIndirectDraws), so the route decision of a draw
	// packet and the mesh-shader variant it builds can never disagree. Not a command-line option;
	// starts equal to the flag.
	bool                   gpu_indirect_draws_request  = false;
	// --readback-queue (K20): synchronous GPU->guest buffer readbacks copy on a second queue of the
	// rendering family and wait only for the work that wrote the data. Default off.
	bool                   readback_queue              = false;
	// --eager-readback: pages a game thread read-faulted on are written back to guest memory right
	// after the GPU writes them (copied in the command buffer about to be submitted), so the next
	// read needs no fault and no GPU drain. Default off. --debug-eager-readback-verify re-reads
	// every page from the GPU before it is unprotected (slow) and reports differences.
	bool                   eager_readback              = false;
	// --poll-before-wait <us>: poll the timeline semaphore for up to this long before blocking in
	// vkWaitSemaphores. 0 (default) blocks at once.
	uint32_t               poll_before_wait_us         = 0;
	// Runtime state of --poll-before-wait (set from poll_before_wait_us != 0 when the configuration
	// is loaded; --toggle-control flips it, common/runtimeToggles.h). Not a command-line option.
	bool                   poll_before_wait_on         = false;
	bool                   eager_readback_verify       = false;
	// --readback-futures: a game thread's read fault on GPU-written memory no longer stops the GPU
	// thread: it copies the bytes out, submits and goes on; the faulting thread alone waits for the
	// copy to land (frame pipelining, notes/frame-pipelining.md). Default off.
	bool                   readback_futures            = false;
	std::filesystem::path  bench_render_csv;
	// --toggle-control <path>: poll this control file and flip the registered runtime-safe flags it
	// names at the next presented frame (common/runtimeToggles.h). Default off.
	std::filesystem::path  toggle_control;
	// --debug-skip-log <path>: one row per dropped / waiting graphics draw (see benchInstrument.h).
	std::filesystem::path  debug_skip_log;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
	// Ordinary boots need a non-null argv[0]; --guest-exec supplies the caller's arguments.
	std::vector<std::string> guest_arguments {""};
};

void Load(const ConfigOptions& cfg);

// Registers the flags that may change at run time (common/runtimeToggles.h).
void RegisterRuntimeToggles(Common::RuntimeToggles::Registry& registry);


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
uint32_t          GetDriverCacheCheckpointSeconds();
bool              GetPipelineLookahead();
// The flag, effective only with --async-pipelines on (the optimized rebuilds run on the pool).
bool              GetPipelinesUnoptimizedFirst();
// Both effective only with --async-pipelines on (the builds run on the pool).
bool              GetPrecompileOnLoad();
bool              GetPipelineVariantsReplay();
uint32_t          GetPipelineLookaheadKinds();
uint32_t          GetLookaheadWorkers();
uint32_t          GetGpuOpsMode();
uint32_t          GetShaderLoopLimit();
bool              DebugDumpShaderMatches(uint64_t shader_hash);
bool              GetLookaheadVerify();
bool              TextureGcEnabled();
bool              TextureGcLog();
bool              SubmitCoalesceEnabled();
bool              BdaSyncSkip();
bool              BdaDirtyLogEnabled();
bool              LazyOpPollEnabled();
bool              BackingReadCacheEnabled();
bool              GcClockFrames();
uint32_t          GetSubmitCoalesceMax();
uint32_t          GetSubmitCoalesceUs();
bool              ElideBarriersEnabled();
bool              DeferDrawWriteBarriers();
bool              OverlapDispatchesEnabled();
bool              KnownFillsEnabled();
bool              KnownStoresEnabled();
bool              KnownCopiesEnabled();
uint32_t          DebugKnownVerifySeconds();
bool              DebugKnownVerifyAll();
bool              ImageLookupMemoEnabled();
bool              TextureBindingReuseEnabled();
bool              ResolvedDrawEnabled();
bool              ResolvedDrawVerify();
bool              CleanGuestReadsEnabled();
bool              DrawKeyCacheEnabled();
bool              SrtPlanShadowEnabled();
bool              SrtPlansEnabled();
bool              SkipGuardEnabled();
bool              SkipGuardRequested();
bool              SkipGuardBdaEnabled();
bool              SrtTableWalkEnabled();
bool              MappingMemoEnabled();
bool              DrawKeyVerifyEnabled();
bool              GpuIndirectDrawsEnabled();
void              LatchGpuIndirectDraws();
bool              ReadbackQueueEnabled();
bool              EagerReadbackEnabled();
uint32_t          PollBeforeWaitUs();
bool              EagerReadbackVerifyEnabled();
bool              ReadbackFuturesEnabled();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();
bool SyncValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool SpirvRobustBoundsRequested();
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
bool MemResidencyEnabled();
bool HeapTrimEnabled();
bool MemHoleReadsEnabled();
bool SkipZeroUploadsEnabled();
bool VerifyZeroUploadsEnabled();
bool UploadRingRebarEnabled();
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
