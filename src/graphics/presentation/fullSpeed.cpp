#include "graphics/presentation/fullSpeed.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/timer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Libs::Graphics::FullSpeed {

namespace {

struct State {
	Policy                policy;
	std::atomic<bool>     skip {false};
	std::atomic<bool>     ending_skipped {false};
	std::atomic<double>   next_scale {1.0};
	std::atomic<uint64_t> skipped_draws {0};
	std::atomic<uint64_t> flips {0};
	uint64_t              last_log_ns   = 0;
	uint64_t              last_flips    = 0;
	uint64_t              last_rendered = 0;
	uint64_t              last_skipped  = 0;
	uint64_t              last_draws    = 0;
	// Diagnostic, not a feature: KYTY_FULL_SPEED_FORCE=all skips every frame (measures the cost of a
	// skipped frame and the guest's own frame time), =none renders every frame, =N skips N of N+1.
	int64_t               force_every = -2;
};

State& Global() {
	static State state;
	return state;
}

uint64_t NowNs() {
	const auto counter = Common::Timer::QueryPerformanceCounter();
	const auto freq    = Common::Timer::QueryPerformanceFrequency();
	return static_cast<uint64_t>(static_cast<double>(counter) * 1e9 / static_cast<double>(freq));
}

int64_t ForceMode() {
	auto& s = Global();
	if (s.force_every == -2) {
		s.force_every = -1;
		if (const char* env = std::getenv("KYTY_FULL_SPEED_FORCE")) {
			if (std::strcmp(env, "all") == 0) {
				s.force_every = 0;
			} else if (std::strcmp(env, "none") == 0) {
				s.force_every = -3;
			} else {
				s.force_every = std::max<int64_t>(std::atoll(env), 1);
			}
		}
	}
	return s.force_every;
}

} // namespace

bool SkipDraws() noexcept {
	return Global().skip.load(std::memory_order_relaxed);
}

bool EndingFrameSkipped() noexcept {
	return Global().ending_skipped.load(std::memory_order_relaxed);
}

double NextFrameTimeScale() noexcept {
	return Global().next_scale.load(std::memory_order_relaxed);
}

void NoteSkippedDraw() noexcept {
	Global().skipped_draws.fetch_add(1, std::memory_order_relaxed);
}

void OnFlipParsed(uint64_t interval_ns) noexcept {
	auto& s = Global();
	if (!Config::FullSpeedEnabled()) {
		s.skip.store(false, std::memory_order_relaxed);
		s.next_scale.store(1.0, std::memory_order_relaxed);
		return;
	}
	s.ending_skipped.store(s.skip.load(std::memory_order_relaxed), std::memory_order_relaxed);
	const uint64_t now  = NowNs();
	bool           skip = s.policy.OnFlip(now, interval_ns);
	const auto     n    = s.flips.fetch_add(1, std::memory_order_relaxed) + 1;
	switch (const auto force = ForceMode()) {
		case -1: break;
		case -3: skip = false; break;
		case 0: skip = true; break;
		default: skip = (n % static_cast<uint64_t>(force + 1)) != 0; break;
	}
	s.skip.store(skip, std::memory_order_relaxed);
	s.next_scale.store(skip ? s.policy.SkippedFrameTimeScale() : 1.0, std::memory_order_relaxed);
	LogStats();
}

void LogStats() noexcept {
	auto&          s   = Global();
	const uint64_t now = NowNs();
	if (s.last_log_ns == 0) {
		s.last_log_ns = now;
		return;
	}
	if (now - s.last_log_ns < 1000000000ull) {
		return;
	}
	const double   dt      = static_cast<double>(now - s.last_log_ns) * 1e-9;
	const uint64_t flips   = s.flips.load(std::memory_order_relaxed);
	const uint64_t draws   = s.skipped_draws.load(std::memory_order_relaxed);
	const uint64_t rend    = s.policy.Rendered();
	const uint64_t skipped = s.policy.Skipped();
	// One line a second: flips parsed per second, frames decided rendered / skipped, draws not
	// executed, the lag behind real time and the frame time of each kind (parse to parse).
	std::string line = "FULLSPEED: t=" + std::to_string(now / 1000000ull) +
	                   "ms flips/s=" + std::to_string(static_cast<double>(flips - s.last_flips) / dt) +
	                   " rendered/s=" + std::to_string(static_cast<double>(rend - s.last_rendered) / dt) +
	                   " skipped/s=" + std::to_string(static_cast<double>(skipped - s.last_skipped) / dt) +
	                   " skipped_draws/s=" + std::to_string(static_cast<double>(draws - s.last_draws) / dt) +
	                   " lag_ms=" + std::to_string(s.policy.LagNs() * 1e-6) +
	                   " cost_rendered_ms=" + std::to_string(s.policy.CostRenderedNs() * 1e-6) +
	                   " cost_skipped_ms=" + std::to_string(s.policy.CostSkippedNs() * 1e-6) + "\n";
	Log::WriteToConsoleAndLog(line);
	s.last_log_ns   = now;
	s.last_flips    = flips;
	s.last_rendered = rend;
	s.last_skipped  = skipped;
	s.last_draws    = draws;
}

} // namespace Libs::Graphics::FullSpeed
