#include "common/gpuPhaseStats.h"

#include "common/logging/log.h"
#include "common/phaseTimers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fmt/format.h>
#include <string>

namespace Common::GpuPhase {

namespace {

constexpr size_t kPhases = static_cast<size_t>(Phase::Count);

constexpr std::array<const char*, kPhases> kNames = {
    "idle", "pm4", "draw", "pipeline", "pipeline_job", "srt", "descriptors",
    "submit", "drain_tick", "drain_flush", "drain_finish", "gc"};

std::atomic<uint32_t> g_interval_s {0};
std::atomic<uint64_t> g_submits {0};
std::atomic<uint64_t> g_frames {0};

int64_t WallNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

bool IsDrain(Phase phase) {
	return phase == Phase::DrainTick || phase == Phase::DrainFlush || phase == Phase::DrainFinish;
}

} // namespace

namespace detail {

struct ThreadState {
	PhaseTimers::ExclusiveClock<kPhases> clock;
	uint32_t                             current_opcode = 0;
	std::array<uint64_t, 256>            packet_ticks {};
	std::array<uint64_t, 256>            packet_count {};
	std::array<uint64_t, 256>            drain_ticks_by_packet {};

	// Window start (previous report).
	int64_t  window_wall_ns = 0;
	uint64_t window_tsc     = 0;
	int64_t  first_wall_ns  = 0;
	uint64_t window_submits = 0;
	uint64_t window_frames  = 0;
	std::array<uint64_t, kPhases> window_total {};
	std::array<uint64_t, kPhases> window_calls {};
	std::array<uint64_t, 256>     window_packet_ticks {};
	std::array<uint64_t, 256>     window_packet_count {};
	std::array<uint64_t, 256>     window_drain_by_packet {};
	std::FILE*                    csv = nullptr;
};

thread_local ThreadState* t_state = nullptr;

uint64_t Now() noexcept {
#if defined(__x86_64__) || defined(__i386__)
	return __builtin_ia32_rdtsc();
#else
	return static_cast<uint64_t>(WallNs());
#endif
}

void Enter(ThreadState* state, Phase phase) noexcept {
	state->clock.Enter(static_cast<size_t>(phase), Now());
}

void Exit(ThreadState* state, Phase phase, uint64_t start) noexcept {
	const auto now = Now();
	state->clock.Exit(now);
	if (IsDrain(phase) && now > start) {
		state->drain_ticks_by_packet[state->current_opcode & 0xffu] += now - start;
	}
}

void SetPacket(ThreadState* state, uint32_t opcode) noexcept {
	state->current_opcode = opcode;
}

void PacketEnd(ThreadState* state, uint32_t opcode, uint64_t start) noexcept {
	const auto now = Now();
	if (now > start) {
		state->packet_ticks[opcode & 0xffu] += now - start;
	}
	++state->packet_count[opcode & 0xffu];
}

} // namespace detail

void Configure(uint32_t interval_seconds) {
	g_interval_s.store(interval_seconds, std::memory_order_relaxed);
}

bool Enabled() noexcept {
	return g_interval_s.load(std::memory_order_relaxed) != 0;
}

void NoteSubmit() noexcept {
	g_submits.fetch_add(1, std::memory_order_relaxed);
}

void NoteFrame() noexcept {
	g_frames.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Submits() noexcept {
	return g_submits.load(std::memory_order_relaxed);
}

uint64_t Frames() noexcept {
	return g_frames.load(std::memory_order_relaxed);
}

void AttachGpuThread() {
	if (!Enabled() || detail::t_state != nullptr) {
		return;
	}
	// Intentionally leaked: the thread can be torn down by quick_exit().
	auto* state             = new detail::ThreadState();
	state->window_wall_ns   = WallNs();
	state->first_wall_ns    = state->window_wall_ns;
	state->window_tsc       = detail::Now();
	state->window_submits   = Submits();
	state->window_frames    = Frames();
	state->csv              = std::fopen("_gpu_phase.csv", "w");
	if (state->csv != nullptr) {
		std::string header = "t_s,window_s,frames,submits";
		for (const auto* name: kNames) {
			header += fmt::format(",{}_pct,{}_calls", name, name);
		}
		header += ",unscoped_pct\n";
		std::fputs(header.c_str(), state->csv);
		std::fflush(state->csv);
	}
	detail::t_state = state;
}

namespace {

void Report(detail::ThreadState& s, bool final_report) {
	const auto wall_now = WallNs();
	const auto tsc_now  = detail::Now();
	const double wall_ns = static_cast<double>(wall_now - s.window_wall_ns);
	const double tsc_d   = static_cast<double>(tsc_now - s.window_tsc);
	if (wall_ns <= 0.0 || tsc_d <= 0.0) {
		return;
	}
	// TSC ticks to nanoseconds from the window itself (no calibration step).
	const double ns_per_tick = wall_ns / tsc_d;
	const auto   frames      = Frames() - s.window_frames;
	const auto   submits     = Submits() - s.window_submits;

	std::array<double, kPhases> pct {};
	std::array<uint64_t, kPhases> calls {};
	double scoped = 0.0;
	for (size_t i = 0; i < kPhases; ++i) {
		const auto total = s.clock.Total(i) - s.window_total[i];
		pct[i]           = static_cast<double>(total) * ns_per_tick * 100.0 / wall_ns;
		calls[i]         = s.clock.Calls(i) - s.window_calls[i];
		scoped += pct[i];
		s.window_total[i] = s.clock.Total(i);
		s.window_calls[i] = s.clock.Calls(i);
	}

	std::string line = fmt::format(
	    "GpuPhase{}: t={:.0f}s window={:.2f}s frames={} submits={} ({:.1f}/frame)",
	    final_report ? " final" : "", static_cast<double>(wall_now - s.first_wall_ns) / 1e9,
	    wall_ns / 1e9, frames, submits,
	    frames != 0 ? static_cast<double>(submits) / static_cast<double>(frames) : 0.0);
	for (size_t i = 0; i < kPhases; ++i) {
		line += fmt::format(" {}={:.1f}%", kNames[i], pct[i]);
	}
	line += fmt::format(" unscoped={:.1f}%\n", std::max(0.0, 100.0 - scoped));

	// Drains: count and time by cause (the phases are exclusive, so these are pure wait time).
	line += fmt::format("GpuPhase drains: tick={}x flush={}x finish={}x", calls[static_cast<size_t>(Phase::DrainTick)],
	                    calls[static_cast<size_t>(Phase::DrainFlush)],
	                    calls[static_cast<size_t>(Phase::DrainFinish)]);
	// Top packets by inclusive handler time, and by drain time they caused.
	auto top = [&](const std::array<uint64_t, 256>& now, std::array<uint64_t, 256>& base) {
		std::array<std::pair<uint64_t, size_t>, 256> order {};
		for (size_t i = 0; i < 256; ++i) {
			order[i] = {now[i] - base[i], i};
			base[i]  = now[i];
		}
		std::partial_sort(order.begin(), order.begin() + 4, order.end(),
		                  [](const auto& a, const auto& b) { return a.first > b.first; });
		return order;
	};
	const auto drain_top = top(s.drain_ticks_by_packet, s.window_drain_by_packet);
	line += " by_packet:";
	for (size_t i = 0; i < 4 && drain_top[i].first != 0; ++i) {
		line += fmt::format(" op0x{:02x}={:.1f}ms", drain_top[i].second,
		                    static_cast<double>(drain_top[i].first) * ns_per_tick / 1e6);
	}
	line += "\n";
	const auto packet_top = top(s.packet_ticks, s.window_packet_ticks);
	std::array<uint64_t, 256> count_base = s.window_packet_count;
	line += "GpuPhase packets (inclusive):";
	for (size_t i = 0; i < 4 && packet_top[i].first != 0; ++i) {
		const auto op = packet_top[i].second;
		line += fmt::format(" op0x{:02x}={:.1f}ms/{}x", op,
		                    static_cast<double>(packet_top[i].first) * ns_per_tick / 1e6,
		                    s.packet_count[op] - count_base[op]);
	}
	line += "\n";
	s.window_packet_count = s.packet_count;
	::Log::WriteToConsoleAndLog(line);

	if (s.csv != nullptr) {
		std::string row = fmt::format("{:.1f},{:.3f},{},{}", static_cast<double>(wall_now - s.first_wall_ns) / 1e9,
		                              wall_ns / 1e9, frames, submits);
		for (size_t i = 0; i < kPhases; ++i) {
			row += fmt::format(",{:.2f},{}", pct[i], calls[i]);
		}
		row += fmt::format(",{:.2f}\n", std::max(0.0, 100.0 - scoped));
		std::fputs(row.c_str(), s.csv);
		std::fflush(s.csv);
	}

	s.window_wall_ns = wall_now;
	s.window_tsc     = tsc_now;
	s.window_submits += submits;
	s.window_frames += frames;
}

} // namespace

void Poll() {
	auto* s = detail::t_state;
	if (s == nullptr) {
		return;
	}
	const auto interval = static_cast<int64_t>(g_interval_s.load(std::memory_order_relaxed)) * 1'000'000'000;
	if (interval != 0 && WallNs() - s->window_wall_ns >= interval) {
		Report(*s, false);
	}
}

void Finish() {
	auto* s = detail::t_state;
	if (s == nullptr) {
		return;
	}
	Report(*s, true);
}

} // namespace Common::GpuPhase
