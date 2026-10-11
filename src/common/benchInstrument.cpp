#include "common/benchInstrument.h"

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace Bench {

namespace {

constexpr size_t kPeriodicFlushRows = 32768;

struct FrameRow {
	uint64_t frame_idx;
	int64_t  t_ns;
};

struct State {
	std::mutex            mutex;
	Options               options;
	std::vector<FrameRow> frames;
	bool                  exit_hooks_registered = false;
	bool                  exit_requested        = false;
};

State& GetState() {
	// Intentionally leaked: the flush hooks run during quick_exit()/exit(), possibly after
	// static destruction has begun on another thread.
	static State* state = new State;
	return *state;
}

void FlushLocked(State& state) {
	if (state.frames.empty() || state.options.frames_csv.empty()) {
		state.frames.clear();
		return;
	}
	std::FILE* file = std::fopen(state.options.frames_csv.string().c_str(), "ab");
	if (file == nullptr) {
		std::fprintf(stderr, "bench: cannot open %s for append\n",
		             state.options.frames_csv.string().c_str());
		state.frames.clear();
		return;
	}
	// Header only when the file is new/empty, so repeated appends stay a single valid CSV.
	if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == 0) {
		std::fputs("frame_idx,t_ns\n", file);
	}
	for (const auto& row: state.frames) {
		std::fprintf(file, "%llu,%lld\n", static_cast<unsigned long long>(row.frame_idx),
		             static_cast<long long>(row.t_ns));
	}
	std::fclose(file);
	state.frames.clear();
}

void FlushAtExit() {
	Flush();
}

} // namespace

void Configure(const Options& options) {
	auto&            state = GetState();
	std::lock_guard  lock(state.mutex);
	FlushLocked(state);
	state.options        = options;
	state.exit_requested = false;
	const bool frames    = !options.frames_csv.empty() || options.exit_after_frames != 0;
	if (frames) {
		state.frames.reserve(1u << 16);
	}
	if (!options.frames_csv.empty() && !state.exit_hooks_registered) {
		state.exit_hooks_registered = true;
		// The emulator normally leaves through std::quick_exit(0) (guest threads still run);
		// register for both paths so the buffer is never lost.
		std::atexit(FlushAtExit);
		std::at_quick_exit(FlushAtExit);
	}
	detail::g_frames_enabled.store(frames, std::memory_order_relaxed);
}

bool RecordPresent(uint64_t frame_idx) {
	const auto t_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                      std::chrono::steady_clock::now().time_since_epoch())
	                      .count();
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.frames.push_back({frame_idx, static_cast<int64_t>(t_ns)});
	if (state.options.exit_after_frames != 0 && frame_idx >= state.options.exit_after_frames &&
	    !state.exit_requested) {
		state.exit_requested = true;
		// Persist right away: do not depend on the exit path reaching the hooks.
		FlushLocked(state);
		return true;
	}
	// Bounded loss on a crash/abort (atexit hooks do not run there): ~9 minutes at 60 fps.
	if (state.frames.size() >= kPeriodicFlushRows) {
		FlushLocked(state);
	}
	return false;
}

void Flush() {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	FlushLocked(state);
}

} // namespace Bench
