#ifndef KYTY_COMMON_BENCH_INSTRUMENT_H_
#define KYTY_COMMON_BENCH_INSTRUMENT_H_

// Observation-only benchmark instrumentation (phase 1, shaders.md / emulator-base harness spec).
//
// Everything here is off by default. With no --bench-* flag the only cost on the hot path is one
// relaxed atomic<bool> load at the instrumented call site.

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace Bench {

struct Options {
	// --bench-frames-csv <path>: append "frame_idx,t_ns" for every successful Present.
	std::filesystem::path frames_csv;
	// --bench-exit-after-frames <N>: request a clean exit once N frames were presented (0 = off).
	uint64_t exit_after_frames = 0;
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

// Writes buffered rows to disk and clears the buffer. Idempotent, thread-safe, safe at exit.
void Flush();

} // namespace Bench

#endif // KYTY_COMMON_BENCH_INSTRUMENT_H_
