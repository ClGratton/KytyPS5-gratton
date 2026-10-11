#include "common/benchInstrument.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
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
	// Render passes of the current guest frame (key = width << 32 | height) and finished rows.
	std::map<uint64_t, uint32_t> passes;
	std::vector<std::string>     render_rows;
	uint64_t                     flip_index = 0;
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

void FlushRenderLocked(State& state) {
	if (state.render_rows.empty() || state.options.render_csv.empty()) {
		state.render_rows.clear();
		return;
	}
	std::FILE* file = std::fopen(state.options.render_csv.string().c_str(), "ab");
	if (file == nullptr) {
		std::fprintf(stderr, "bench: cannot open %s for append\n",
		             state.options.render_csv.string().c_str());
		state.render_rows.clear();
		return;
	}
	if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == 0) {
		std::fputs("flip_idx,t_ns,flip_w,flip_h,passes\n", file);
	}
	for (const auto& row: state.render_rows) {
		std::fputs(row.c_str(), file);
	}
	std::fclose(file);
	state.render_rows.clear();
}

void FlushAtExit() {
	Flush();
}

} // namespace

void Configure(const Options& options) {
	auto&            state = GetState();
	std::lock_guard  lock(state.mutex);
	FlushLocked(state);
	FlushRenderLocked(state);
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
	state.passes.clear();
	state.flip_index = 0;
	if (!options.render_csv.empty()) {
		state.render_rows.reserve(1u << 14);
		if (!state.exit_hooks_registered) {
			state.exit_hooks_registered = true;
			std::atexit(FlushAtExit);
			std::at_quick_exit(FlushAtExit);
		}
	}
	detail::g_render_enabled.store(!options.render_csv.empty(), std::memory_order_relaxed);
}

void RecordRenderPass(uint32_t width, uint32_t height) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.passes[(static_cast<uint64_t>(width) << 32u) | height]++;
}

void RecordGuestFlip(uint32_t flip_width, uint32_t flip_height) {
	const auto t_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                      std::chrono::steady_clock::now().time_since_epoch())
	                      .count();
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	std::vector<std::pair<uint64_t, uint32_t>> sizes(state.passes.begin(), state.passes.end());
	std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
		const auto area_a = (a.first >> 32u) * (a.first & 0xffffffffu);
		const auto area_b = (b.first >> 32u) * (b.first & 0xffffffffu);
		return area_a != area_b ? area_a > area_b : a.first > b.first;
	});
	std::string row = std::to_string(++state.flip_index) + "," + std::to_string(t_ns) + "," +
	                  std::to_string(flip_width) + "," + std::to_string(flip_height) + ",";
	for (size_t i = 0; i < sizes.size(); i++) {
		row += (i == 0 ? "" : ";") + std::to_string(sizes[i].first >> 32u) + "x" +
		       std::to_string(sizes[i].first & 0xffffffffu) + ":" + std::to_string(sizes[i].second);
	}
	row += "\n";
	state.render_rows.push_back(std::move(row));
	state.passes.clear();
	if (state.render_rows.size() >= kPeriodicFlushRows / 8) {
		FlushRenderLocked(state);
	}
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
	FlushRenderLocked(state);
}

} // namespace Bench
