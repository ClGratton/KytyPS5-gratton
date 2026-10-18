#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_FULLSPEED_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_FULLSPEED_H_

// --full-speed: below the target frame rate the guest still runs at 100 % game speed.
//
// A title advances one fixed simulation step per flip, so when the emulated GPU cannot render
// every frame the game runs in slow motion and no emulated clock can fix that. This mode keeps the
// flip rate at the target rate instead: frames the emulator cannot afford are skipped for DRAWING
// ONLY. The command processor still parses and executes every packet with a side effect
// (dispatches, copies, register state, labels, events, flips); draw packets become no-ops, the
// flip of a skipped frame completes as soon as the GPU is done with it and presents nothing (the
// last rendered image stays on screen). Displayed fps drops, game speed does not.
//
// The policy below is pure (times are passed in) so tests/FullSpeedTests.cpp can run it directly.
// It looks only at generic signals: the times at which flips are parsed and the flip interval.
//
//  lag      = time the command processor parsed the flip - the time that flip was due at the target
//             rate (the due time advances by the flip interval per flip, never runs ahead of the
//             clock, and is capped so a stall such as a loading screen is forgiven, not repaid).
//  decision = the next frame is skipped when lag is more than one flip interval, skipping is
//             measurably cheaper than rendering (EMA of the frame time of skipped frames below
//             kBenefit x rendered), and the minimum render rate is kept.
//
// Rendering whenever lag <= one interval means the simulation never runs ahead of real time; a
// skipped frame costs less than an interval, which repays the debt a rendered frame adds.

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace Libs::Graphics::FullSpeed {

struct PolicyParams {
	double   max_lag_frames  = 12.0; // lag is capped here: a stall is forgiven, not repaid
	double   min_render_hz   = 5.0;  // at least this many rendered frames per second
	double   benefit         = 0.85; // skip only if a skipped frame costs < benefit x a rendered one
	uint32_t probe_every     = 64;   // while skipping is judged useless, test it every Nth frame
	double   ema_alpha       = 0.25;
	double   stall_ns        = 1e9;  // frame times above this are stalls and are not averaged
};

class Policy {
public:
	explicit Policy(PolicyParams params = {}): m_params(params) {}

	// A flip was parsed at now_ns; interval_ns is the intended duration of a frame (flip interval
	// x vblank period). Returns true if the frame that starts now is skipped.
	bool OnFlip(uint64_t now_ns, uint64_t interval_ns) {
		const double interval = static_cast<double>(std::max<uint64_t>(interval_ns, 1));
		if (m_started) {
			const double d = static_cast<double>(now_ns - m_last_flip_ns);
			if (d < m_params.stall_ns) {
				double& ema = m_cur_skipped ? m_cost_skipped : m_cost_rendered;
				ema = ema == 0.0 ? d : ema + m_params.ema_alpha * (d - ema);
			}
			m_due_ns += interval;
		} else {
			m_started = true;
			m_due_ns  = static_cast<double>(now_ns);
		}
		m_last_flip_ns = now_ns;
		const double now = static_cast<double>(now_ns);
		if (m_due_ns > now) {
			m_due_ns = now; // never bank credit: the simulation must not run ahead of real time
		}
		m_lag_ns = now - m_due_ns;
		const double max_lag = m_params.max_lag_frames * interval;
		if (m_lag_ns > max_lag) {
			m_lag_ns = max_lag;
			m_due_ns = now - max_lag;
		}

		bool skip = m_lag_ns > interval && m_cost_rendered > 0.0;
		if (skip && m_cost_skipped > 0.0 && m_cost_skipped >= m_params.benefit * m_cost_rendered) {
			// Skipping did not make frames cheaper (the guest or the display is the limit):
			// render, and re-test now and then because the estimate goes stale.
			skip = (++m_useless_count % std::max<uint32_t>(m_params.probe_every, 1)) == 0;
		}
		const double min_period = 1e9 / std::max(m_params.min_render_hz, 0.1);
		if (skip && now - static_cast<double>(m_last_render_ns) >= min_period) {
			skip = false;
		}
		if (!skip) {
			m_last_render_ns = now_ns;
		}
		m_cur_skipped = skip;
		(skip ? m_skipped : m_rendered)++;
		return skip;
	}

	[[nodiscard]] bool     Skipping() const { return m_cur_skipped; }
	[[nodiscard]] double   LagNs() const { return m_lag_ns; }
	[[nodiscard]] double   CostRenderedNs() const { return m_cost_rendered; }
	[[nodiscard]] double   CostSkippedNs() const { return m_cost_skipped; }
	[[nodiscard]] uint64_t Rendered() const { return m_rendered; }
	[[nodiscard]] uint64_t Skipped() const { return m_skipped; }

	// Time scale for the GPU timestamps a guest reads inside a skipped frame, so the frame reports
	// about the GPU time of a rendered one instead of the near-zero time of a frame without draws.
	[[nodiscard]] double SkippedFrameTimeScale() const {
		if (!m_cur_skipped || m_cost_rendered <= 0.0 || m_cost_skipped <= 0.0) {
			return 1.0;
		}
		return std::clamp(m_cost_rendered / m_cost_skipped, 1.0, 16.0);
	}

private:
	PolicyParams m_params;
	bool         m_started        = false;
	bool         m_cur_skipped    = false;
	uint64_t     m_last_flip_ns   = 0;
	uint64_t     m_last_render_ns = 0;
	double       m_due_ns         = 0.0;
	double       m_lag_ns         = 0.0;
	double       m_cost_rendered  = 0.0;
	double       m_cost_skipped   = 0.0;
	uint32_t     m_useless_count  = 0;
	uint64_t     m_rendered       = 0;
	uint64_t     m_skipped        = 0;
};

// ---- process state (the command-processor thread decides; other threads only read) ----------

// True while the frame the command processor is parsing is a skipped one.
[[nodiscard]] bool SkipDraws() noexcept;
// The frame ending at the flip being prepared was skipped (read before OnFlipParsed).
[[nodiscard]] bool EndingFrameSkipped() noexcept;
// A flip was parsed; decides the next frame. Command-processor thread only.
void OnFlipParsed(uint64_t interval_ns) noexcept;
// Time scale for the guest GPU timestamps of the frame that starts at the flip just parsed.
[[nodiscard]] double NextFrameTimeScale() noexcept;
void                 NoteSkippedDraw() noexcept;
// Statistics line once per second when the mode is on.
void LogStats() noexcept;

} // namespace Libs::Graphics::FullSpeed

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_FULLSPEED_H_
