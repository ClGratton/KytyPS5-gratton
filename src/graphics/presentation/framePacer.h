#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMEPACER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMEPACER_H_

// --fps-limit: decides, once per virtual vblank, whether a pending guest flip may be presented.
//
// The presenter runs a virtual vblank at --vblank-frequency and presents at most one flip per
// vblank, so the presented rate is already capped at that frequency. A limit below it skips
// vblanks with an even (Bresenham) spread, e.g. 45 fps on a 60 Hz vblank presents on three of
// every four. The guest keeps seeing every vblank (its vblank events, counter and flip interval
// are unchanged); flips simply wait in the flip queue, which is what throttles the game.
//
// Header-only and free of platform state so tests/FramePacerTests.cpp can run it directly.

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

class FramePacer {
public:
	// limit_fps == 0, or >= vblank_hz, disables limiting (a vblank can never present twice).
	void Configure(uint32_t limit_fps, uint32_t vblank_hz) {
		m_vblank_hz = std::max(vblank_hz, 1u);
		m_step      = (limit_fps == 0 || limit_fps >= m_vblank_hz)
		                  ? 1.0
		                  : static_cast<double>(limit_fps) / static_cast<double>(m_vblank_hz);
		m_active    = m_step < 1.0;
		m_credit    = 0.0;
	}

	[[nodiscard]] bool Active() const noexcept {
		return m_active;
	}

	// Call once per vblank. True when a pending flip may be presented now.
	[[nodiscard]] bool OnVblank() {
		if (!m_active) {
			return true;
		}
		m_credit = std::min(m_credit + m_step, 1.0 + m_step);
		return m_credit >= 1.0 - EPSILON;
	}

	// Call when a flip was presented after OnVblank() returned true.
	void OnPresented() {
		if (m_active) {
			m_credit = std::max(m_credit - 1.0, 0.0);
		}
	}

private:
	// Credit is capped at one flip plus one step: enough to carry the fraction in steady state,
	// but a game resuming after a stall cannot present a burst of flips.
	static constexpr double EPSILON    = 1e-9;

	uint32_t m_vblank_hz = 60;
	double   m_step      = 1.0;
	double   m_credit    = 0.0;
	bool     m_active    = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMEPACER_H_
