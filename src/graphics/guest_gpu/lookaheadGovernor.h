#ifndef GRAPHICS_GUEST_GPU_LOOKAHEAD_GOVERNOR_H
#define GRAPHICS_GUEST_GPU_LOOKAHEAD_GOVERNOR_H

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// --lookahead-adaptive: how many of the look-ahead workers need to run.
//
// A worker that runs next to many others takes longer per task (shared caches and cores: measured 8.7,
// 11.5, 13.0 and 14.8 us per task with 1, 2, 3 and 4 workers on the same level), so more workers than the
// GPU thread needs burn host CPU for nothing. The only thing that matters is whether the plans are there
// when the GPU thread asks for them, and the GPU thread's lookups say so: a lookup that finds no plan
// because the look-ahead had not got there yet ("not ready", or the whole submission was started before the
// look-ahead looked at it, "late") or passed the draw without making one ("missing": overloaded workers drop
// tasks as stale) is a miss. The governor drops one worker after a run of quiet windows,
// puts every worker back at the first window with many misses, and remembers the level that failed so it
// does not keep probing it. Scheduling only: every plan is still checked against the real inputs when it is
// adopted.
//
// The thresholds are fractions of the GPU thread's lookups, so they do not depend on the game or the frame
// rate. A miss costs the GPU thread only the work of that one draw's plan (measured: 12.9 % of the draws
// without a plan cost 2 ms of the GPU thread's 40, about 2.6 us per missed draw; 1.2 % cost nothing it could
// be measured at), so a few per cent of misses are cheaper than the host CPU of the extra workers; a
// look-ahead that falls further behind than that is the bottleneck and gets all its workers back.
//
// The state is touched by one thread (the sequencer); `Active()` is what the workers read.
class LookaheadGovernor {
public:
	struct Params {
		int64_t  window_ns      = 500'000'000;     // one decision per window
		uint64_t min_events     = 2000;            // lookups a window needs to count at all
		double   miss_high      = 0.04;            // above this fraction of lookups: all workers back
		double   miss_low       = 0.015;           // at or below: a quiet window
		uint32_t quiet_windows  = 4;               // quiet windows in a row before one worker is dropped
		int64_t  hold_ns        = 20'000'000'000;  // after a restore, no drop for this long
		int64_t  floor_ns       = 600'000'000'000; // a level that failed is not probed again for this long
	};

	explicit LookaheadGovernor(uint32_t max_workers): LookaheadGovernor(max_workers, Params {}) {}
	LookaheadGovernor(uint32_t max_workers, const Params& params)
	    : m_params(params), m_max(std::max<uint32_t>(max_workers, 1u)), m_active(m_max) {}

	[[nodiscard]] uint32_t Active() const { return m_active; }
	[[nodiscard]] uint32_t Max() const { return m_max; }
	// The last window's miss fraction (for the log line when the level changes).
	[[nodiscard]] double LastMissRate() const { return m_last_rate; }

	// All workers, now (the feature was switched off). Starts a fresh window at `now_ns`.
	uint32_t Reset(int64_t now_ns, uint64_t events, uint64_t misses) {
		m_active = m_max;
		m_quiet  = 0;
		StartWindow(now_ns, events, misses);
		return m_active;
	}

	// `events` and `misses` are the cumulative counts of the GPU thread's plan lookups. Returns the number
	// of workers that should run.
	uint32_t Update(int64_t now_ns, uint64_t events, uint64_t misses) {
		if (!m_started) {
			StartWindow(now_ns, events, misses);
			return m_active;
		}
		if (now_ns - m_window_start < m_params.window_ns) {
			return m_active;
		}
		const uint64_t window_events = events - m_events;
		const uint64_t window_misses = misses - m_misses;
		StartWindow(now_ns, events, misses);
		if (window_events < m_params.min_events) {
			m_quiet = 0; // not enough lookups to say anything either way
			return m_active;
		}
		const double rate = static_cast<double>(window_misses) / static_cast<double>(window_events);
		m_last_rate       = rate;
		if (rate > m_params.miss_high) {
			if (m_active < m_max) {
				// The level we tried is too low: everyone back, and do not try it again for a while.
				m_floor       = std::min(m_active + 1, m_max);
				m_floor_until = now_ns + m_params.floor_ns;
				m_active      = m_max;
			}
			m_hold_until = now_ns + m_params.hold_ns;
			m_quiet      = 0;
			return m_active;
		}
		if (rate > m_params.miss_low) {
			m_quiet = 0;
			return m_active;
		}
		if (now_ns < m_hold_until) {
			return m_active;
		}
		if (++m_quiet >= m_params.quiet_windows) {
			m_quiet            = 0;
			const uint32_t low = now_ns < m_floor_until ? m_floor : 1u;
			if (m_active > low) --m_active;
		}
		return m_active;
	}

private:
	void StartWindow(int64_t now_ns, uint64_t events, uint64_t misses) {
		m_started      = true;
		m_window_start = now_ns;
		m_events       = events;
		m_misses       = misses;
	}

	Params   m_params;
	uint32_t m_max;
	uint32_t m_active;
	uint32_t m_floor        = 1;
	uint32_t m_quiet        = 0;
	bool     m_started      = false;
	int64_t  m_window_start = 0;
	int64_t  m_hold_until   = 0;
	int64_t  m_floor_until  = 0;
	uint64_t m_events       = 0;
	uint64_t m_misses       = 0;
	double   m_last_rate    = 0.0;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_LOOKAHEAD_GOVERNOR_H
