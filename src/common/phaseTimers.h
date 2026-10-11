#ifndef KYTY_COMMON_PHASE_TIMERS_H_
#define KYTY_COMMON_PHASE_TIMERS_H_

// Pure exclusive-time accounting for nested scopes (no clock, no threads, no dependencies; see
// tests/PhaseTimersTests.cpp). The runtime wrapper for the GPU thread is common/gpuPhaseStats.h.
//
// Enter(phase, now) / Exit(now) charge the time since the previous event to the phase that was on
// top of the stack, so an inner phase takes its time away from the outer one and the per-phase
// totals add up to the time spent inside the outermost scopes (never more). The stack is bounded:
// deeper scopes are not tracked (their time stays with the deepest tracked phase) but still
// balance Enter/Exit.
//
// Idea from BryanKAdams/BryKytyPS5 (draw-phase timers, drain stats; GPL-2.0), re-implemented.

#include <array>
#include <cstddef>
#include <cstdint>

namespace Common::PhaseTimers {

template<size_t PhaseCount, size_t MaxDepth = 24>
class ExclusiveClock {
public:
	void Enter(size_t phase, uint64_t now) {
		if (m_depth < MaxDepth) {
			if (m_depth > 0) {
				Charge(m_stack[m_depth - 1], now);
			}
			m_stack[m_depth] = phase < PhaseCount ? phase : PhaseCount - 1;
			m_mark           = now;
			++m_calls[m_stack[m_depth]];
		}
		++m_depth;
	}

	void Exit(uint64_t now) {
		if (m_depth == 0) {
			return;
		}
		--m_depth;
		if (m_depth < MaxDepth) {
			Charge(m_stack[m_depth], now);
			m_mark = now;
		}
	}

	[[nodiscard]] uint64_t Total(size_t phase) const { return m_total[phase]; }
	[[nodiscard]] uint64_t Calls(size_t phase) const { return m_calls[phase]; }
	[[nodiscard]] size_t   Depth() const { return m_depth; }
	[[nodiscard]] uint64_t Sum() const {
		uint64_t sum = 0;
		for (const auto value: m_total) {
			sum += value;
		}
		return sum;
	}
	// Phase on top of the stack (valid when Depth() != 0).
	[[nodiscard]] size_t Current() const {
		return m_depth == 0 ? 0 : m_stack[(m_depth < MaxDepth ? m_depth : MaxDepth) - 1];
	}

private:
	void Charge(size_t phase, uint64_t now) {
		if (now > m_mark) {
			m_total[phase] += now - m_mark;
		}
	}

	std::array<size_t, MaxDepth>     m_stack {};
	std::array<uint64_t, PhaseCount> m_total {};
	std::array<uint64_t, PhaseCount> m_calls {};
	size_t                           m_depth = 0;
	uint64_t                         m_mark  = 0;
};

} // namespace Common::PhaseTimers

#endif /* KYTY_COMMON_PHASE_TIMERS_H_ */
