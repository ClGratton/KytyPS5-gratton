#ifndef KYTY_COMMON_BENCH_INSTRUMENT_H_
#define KYTY_COMMON_BENCH_INSTRUMENT_H_

// Observation-only benchmark instrumentation (phase 1, shaders.md / emulator-base harness spec).
//
// Everything here is off by default. With no --bench-* flag the only cost on the hot path is one
// relaxed atomic<bool> load at the instrumented call site.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Bench {

struct Options {
	// --bench-frames-csv <path>: append "frame_idx,t_ns" for every successful Present.
	std::filesystem::path frames_csv;
	// --bench-exit-after-frames <N>: request a clean exit once N frames were presented (0 = off).
	uint64_t exit_after_frames = 0;
	// --shader-timing-log <path>: append one CSV row per program translation, per pipeline
	// creation (misses only) and per slow MaterializeResources / S_SWAP_PC refresh, plus
	// aggregate rows for the fast (hit) path. See docs/phase1-notes.md for the row format.
	std::filesystem::path shader_timing_log;
	// --bench-gpu-trace: with --shader-timing-log, also log the guest submit / GPU-thread execute
	// timeline (rows gpu_submit, gpu_start, gpu_end, gpu_draw, gpu_miss). Observation only.
	bool gpu_trace = false;
	// --bench-render-csv <path>: one row per guest flip with the render-pass sizes recorded since
	// the previous flip, and the size of the buffer that was flipped.
	std::filesystem::path render_csv;
	// --debug-registration-log: with the timing log, also log shader registrations with their
	// context registers and the first use of every graphics pipeline (rows reg_shader, gfx_prog,
	// gfx_req, gfx_draw). Observation only.
	bool registration_log = false;
};

// Called once from Config::Load(). Replaces any previous state (the buffer is flushed first).
void Configure(const Options& options);

namespace detail {
inline std::atomic<bool> g_frames_enabled {false};
} // namespace detail

[[nodiscard]] inline bool FramesEnabled() noexcept {
	return detail::g_frames_enabled.load(std::memory_order_relaxed);
}

// Records one presented frame. `frame_idx` is the 1-based number of frames presented so far
// (the value of presented_frames after the increment). The timestamp is std::chrono::steady_clock
// nanoseconds since the clock's epoch, taken inside this call.
// Returns true when --bench-exit-after-frames was reached by this frame (the caller must then
// request the emulator's normal clean exit). Call only when FramesEnabled().
[[nodiscard]] bool RecordPresent(uint64_t frame_idx);

namespace detail {
inline std::atomic<bool> g_render_enabled {false};
} // namespace detail

[[nodiscard]] inline bool RenderEnabled() noexcept {
	return detail::g_render_enabled.load(std::memory_order_relaxed);
}

// Counts one dynamic-rendering pass of width x height (any thread). Call only when RenderEnabled().
void RecordRenderPass(uint32_t width, uint32_t height);

// Closes the current frame group at a guest flip (GPU thread): writes
// "flip_idx,t_ns,flip_w,flip_h,sizes" where sizes is "WxH:count;..." ordered by pixel count
// (largest area first) and resets the group. Call only when RenderEnabled().
void RecordGuestFlip(uint32_t flip_width, uint32_t flip_height);

// Writes buffered rows to disk and clears the buffers. Thread-safe, safe at exit. Also emits the
// aggregate "summary_*" timing rows (values are deltas since the previous Flush).
void Flush();

// ---- Shader / pipeline timing (--shader-timing-log) -----------------------------------------

namespace detail {
inline std::atomic<bool> g_timing_enabled {false};
} // namespace detail

[[nodiscard]] inline bool TimingEnabled() noexcept {
	return detail::g_timing_enabled.load(std::memory_order_relaxed);
}

namespace detail {
inline std::atomic<bool> g_registration_enabled {false};
} // namespace detail

// --debug-registration-log (implies the timing log is on).
[[nodiscard]] inline bool RegistrationLogEnabled() noexcept {
	return detail::g_registration_enabled.load(std::memory_order_relaxed) && TimingEnabled();
}

// steady_clock nanoseconds since the clock epoch (same clock as the frames CSV).
[[nodiscard]] int64_t NowNs() noexcept;

// Appends one row. `t_start_ns` is NowNs() taken when the timed step began, so rows can be
// joined with the frames CSV. `detail` is ';'-separated key=value text without commas.
// Call only when TimingEnabled().
void LogTiming(const char* kind, uint64_t key_hash, int64_t t_start_ns, int64_t dur_ns,
               bool on_gpu_thread, const std::string& detail);

// OS thread id as written in the `thread` column.
[[nodiscard]] uint64_t ThreadId() noexcept;

// Same as LogTiming, but with an explicit `thread` column. Used when a row is recorded on one
// thread and written later by another (async pipeline jobs, docs/phase2-notes.md).
void LogTimingAs(const char* kind, uint64_t key_hash, int64_t t_start_ns, int64_t dur_ns,
                 uint64_t thread_id, bool on_gpu_thread, const std::string& detail);

// ---- GPU command trace (--bench-gpu-trace, docs/lookahead-notes.md) -----------------------------
namespace detail {
inline std::atomic<bool> g_trace_enabled {false};
} // namespace detail

[[nodiscard]] inline bool TraceEnabled() noexcept {
	return detail::g_trace_enabled.load(std::memory_order_relaxed) && TimingEnabled();
}
// GPU thread: the submission being executed (0 = none) and the draw/dispatch counter inside it.
void TraceSetSubmission(uint64_t seq) noexcept;
[[nodiscard]] uint64_t TraceSubmission() noexcept;
// Counts one draw/dispatch of the current submission and logs a gpu_draw row; returns its index.
uint32_t TraceDraw(const char* what);
[[nodiscard]] uint32_t TraceDrawCount() noexcept;
[[nodiscard]] uint32_t TraceDispatchCount() noexcept;

// Fast-path (cache hit) steps run on every draw, so they are aggregated, not logged per call.
enum class HitSlot : uint32_t { Materialize = 0, SwapPcRefresh = 1, Count };
// Calls at or above this duration are additionally logged as individual rows (miss=0).
inline constexpr int64_t kSlowHitNs = 200'000;
void AccumulateHit(HitSlot slot, int64_t dur_ns) noexcept;

// Hand-off of the driver-call duration (vkCreate*Pipelines) from shaders.cpp to the caller that
// logs the whole pipeline creation. Thread-local, so it is safe with worker threads.
void SetDriverCreateNs(int64_t ns) noexcept;
[[nodiscard]] int64_t TakeDriverCreateNs() noexcept;

} // namespace Bench

#endif // KYTY_COMMON_BENCH_INSTRUMENT_H_
