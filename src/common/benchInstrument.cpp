#include "common/benchInstrument.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Bench {

namespace {

constexpr size_t kPeriodicFlushRows = 32768;
constexpr size_t kTimingFlushRows   = 1024;

struct FrameRow {
	uint64_t frame_idx;
	int64_t  t_ns;
};

struct HitCounters {
	std::atomic<uint64_t> count {0};
	std::atomic<int64_t>  total_ns {0};
	std::atomic<int64_t>  max_ns {0};
};

struct State {
	std::mutex                                           mutex;
	Options                                              options;
	std::vector<FrameRow>                                frames;
	std::vector<std::string>                             timing_rows;
	bool                                                 exit_hooks_registered = false;
	bool                                                 exit_requested        = false;
	std::array<HitCounters, static_cast<size_t>(HitSlot::Count)> hits;
};

State& GetState() {
	// Intentionally leaked: the flush hooks run during quick_exit()/exit(), possibly after
	// static destruction has begun on another thread.
	static State* state = new State;
	return *state;
}

// Opens `path` for append and writes `header` when the file is new/empty.
std::FILE* OpenAppend(const std::filesystem::path& path, const char* header) {
	std::FILE* file = std::fopen(path.string().c_str(), "ab");
	if (file == nullptr) {
		std::fprintf(stderr, "bench: cannot open %s for append\n", path.string().c_str());
		return nullptr;
	}
	if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == 0) {
		std::fputs(header, file);
	}
	return file;
}

void FlushFramesLocked(State& state) {
	if (state.frames.empty() || state.options.frames_csv.empty()) {
		state.frames.clear();
		return;
	}
	if (std::FILE* file = OpenAppend(state.options.frames_csv, "frame_idx,t_ns\n")) {
		for (const auto& row: state.frames) {
			std::fprintf(file, "%llu,%lld\n", static_cast<unsigned long long>(row.frame_idx),
			             static_cast<long long>(row.t_ns));
		}
		std::fclose(file);
	}
	state.frames.clear();
}

constexpr const char* kTimingHeader = "t_ns,kind,key_hash,dur_ns,thread,gpu_thread,detail\n";

void FlushTimingLocked(State& state) {
	if (state.timing_rows.empty() || state.options.shader_timing_log.empty()) {
		state.timing_rows.clear();
		return;
	}
	if (std::FILE* file = OpenAppend(state.options.shader_timing_log, kTimingHeader)) {
		for (const auto& row: state.timing_rows) {
			std::fputs(row.c_str(), file);
		}
		std::fclose(file);
	}
	state.timing_rows.clear();
}

uint64_t CurrentThreadId() noexcept {
#if defined(__linux__)
	thread_local const uint64_t tid = static_cast<uint64_t>(::syscall(SYS_gettid));
	return tid;
#else
	thread_local const uint64_t tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
	return tid;
#endif
}

thread_local int64_t t_driver_create_ns = 0;
thread_local uint64_t t_trace_seq        = 0;
thread_local uint32_t t_trace_draws      = 0;
thread_local uint32_t t_trace_dispatches = 0;

void FlushAtExit() {
	Flush();
}

} // namespace

void Configure(const Options& options) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	FlushFramesLocked(state);
	FlushTimingLocked(state);
	state.options        = options;
	state.exit_requested = false;
	const bool frames    = !options.frames_csv.empty() || options.exit_after_frames != 0;
	const bool timing    = !options.shader_timing_log.empty();
	if (frames) {
		state.frames.reserve(1u << 16);
	}
	if ((!options.frames_csv.empty() || timing) && !state.exit_hooks_registered) {
		state.exit_hooks_registered = true;
		// The emulator normally leaves through std::quick_exit(0) (guest threads still run);
		// register for both paths so the buffers are never lost.
		std::atexit(FlushAtExit);
		std::at_quick_exit(FlushAtExit);
	}
	detail::g_frames_enabled.store(frames, std::memory_order_relaxed);
	detail::g_timing_enabled.store(timing, std::memory_order_relaxed);
	detail::g_trace_enabled.store(options.gpu_trace, std::memory_order_relaxed);
}

bool RecordPresent(uint64_t frame_idx) {
	const auto      t_ns  = NowNs();
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.frames.push_back({frame_idx, t_ns});
	if (state.options.exit_after_frames != 0 && frame_idx >= state.options.exit_after_frames &&
	    !state.exit_requested) {
		state.exit_requested = true;
		// Persist right away: do not depend on the exit path reaching the hooks.
		FlushFramesLocked(state);
		return true;
	}
	// Bounded loss on a crash/abort (atexit hooks do not run there): ~9 minutes at 60 fps.
	if (state.frames.size() >= kPeriodicFlushRows) {
		FlushFramesLocked(state);
	}
	return false;
}

int64_t NowNs() noexcept {
	return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                std::chrono::steady_clock::now().time_since_epoch())
	                                .count());
}

uint64_t ThreadId() noexcept {
	return CurrentThreadId();
}

void LogTiming(const char* kind, uint64_t key_hash, int64_t t_start_ns, int64_t dur_ns,
               bool on_gpu_thread, const std::string& detail) {
	LogTimingAs(kind, key_hash, t_start_ns, dur_ns, CurrentThreadId(), on_gpu_thread, detail);
}

void LogTimingAs(const char* kind, uint64_t key_hash, int64_t t_start_ns, int64_t dur_ns,
                 uint64_t thread_id, bool on_gpu_thread, const std::string& detail) {
	auto row = fmt::format("{},{},{:016x},{},{},{},{}\n", t_start_ns, kind, key_hash, dur_ns,
	                       thread_id, on_gpu_thread ? 1 : 0, detail);
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.timing_rows.push_back(std::move(row));
	if (state.timing_rows.size() >= kTimingFlushRows) {
		FlushTimingLocked(state);
	}
}

void AccumulateHit(HitSlot slot, int64_t dur_ns) noexcept {
	auto& counters = GetState().hits[static_cast<size_t>(slot)];
	counters.count.fetch_add(1, std::memory_order_relaxed);
	counters.total_ns.fetch_add(dur_ns, std::memory_order_relaxed);
	auto previous = counters.max_ns.load(std::memory_order_relaxed);
	while (dur_ns > previous &&
	       !counters.max_ns.compare_exchange_weak(previous, dur_ns, std::memory_order_relaxed)) {
	}
}

void SetDriverCreateNs(int64_t ns) noexcept {
	t_driver_create_ns = ns;
}

int64_t TakeDriverCreateNs() noexcept {
	return std::exchange(t_driver_create_ns, 0);
}

void TraceSetSubmission(uint64_t seq) noexcept {
	t_trace_seq        = seq;
	t_trace_draws      = 0;
	t_trace_dispatches = 0;
}

uint64_t TraceSubmission() noexcept {
	return t_trace_seq;
}

uint32_t TraceDraw(const char* what) {
	const bool     cs  = what[0] == 'c';
	const uint32_t idx = cs ? t_trace_dispatches++ : t_trace_draws++;
	LogTiming("gpu_draw", t_trace_seq, NowNs(), 0, true,
	          fmt::format("k={};n={};n_all={}", what, idx, t_trace_draws + t_trace_dispatches - 1));
	return idx;
}

uint32_t TraceDrawCount() noexcept {
	return t_trace_draws;
}

uint32_t TraceDispatchCount() noexcept {
	return t_trace_dispatches;
}

void Flush() {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	FlushFramesLocked(state);
	if (!state.options.shader_timing_log.empty()) {
		static constexpr std::array<const char*, static_cast<size_t>(HitSlot::Count)> names = {
		    "summary_materialize_hit"};
		const auto now = NowNs();
		for (size_t i = 0; i < state.hits.size(); i++) {
			auto&      counters = state.hits[i];
			const auto count    = counters.count.exchange(0, std::memory_order_relaxed);
			const auto total    = counters.total_ns.exchange(0, std::memory_order_relaxed);
			const auto max      = counters.max_ns.exchange(0, std::memory_order_relaxed);
			if (count == 0) {
				continue;
			}
			state.timing_rows.push_back(fmt::format("{},{},{:016x},{},{},{},count={};max_ns={}\n",
			                                        now, names[i], 0, total, CurrentThreadId(), 0,
			                                        count, max));
		}
	}
	FlushTimingLocked(state);
}

} // namespace Bench
