#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SUBMITCOALESCER_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SUBMITCOALESCER_H_

// Pure policy for --submit-coalesce (K13): when a RELEASE_MEM end-of-pipe event that asks for a
// submit may leave the open command buffer unsubmitted. Unit tested in
// tests/SubmitCoalescerTests.cpp; the command processor owns one instance per queue.
//
// Titles raise hundreds of end-of-pipe events a frame (Astro Bot: ~340 command buffers, ~47 us per
// vkQueueSubmit). The label value is written to guest memory when the packet is parsed, so a
// submit per event never made it visible sooner. Only an event's interrupt waits for the submit,
// and that wait is bounded here: by the number of events held back and by their age. The end of a
// submission slice and every drain submit regardless (graphicsRun.cpp), so a held event is never
// stuck behind a wait. The age bound comes from the vblank period: a fixed 5000 us collapsed
// pacing in BryanKAdams/BryKytyPS5 (the game waits on the delayed interrupts), 2000 us was best.
//
// Ported from ogeidexe/KytyPS5@abbbce369 (count + age bound, ~16 events / 250 us) and
// BryanKAdams/BryKytyPS5@b63206f4c (the interval data); GPL-2.0, re-implemented.

#include <cstdint>

namespace Libs::Graphics {

struct SubmitCoalescerConfig {
	uint32_t max_events = 16; // flush when this many events are held back
	uint64_t max_age    = 0;  // flush when the oldest held event is this old (clock units)
};

// The age bound from the vblank frequency: a fraction (1/divisor) of the frame period, in the
// units of `clock_hz`, never zero. divisor 8 gives 2083 us at 60 Hz.
[[nodiscard]] constexpr uint64_t CoalesceMaxAge(uint32_t vblank_hz, uint64_t clock_hz,
                                                uint32_t divisor) {
	if (vblank_hz == 0 || divisor == 0) {
		return 1;
	}
	const uint64_t age = clock_hz / vblank_hz / divisor;
	return age == 0 ? 1 : age;
}

class SubmitCoalescer {
public:
	enum class Decision : uint8_t {
		Defer,         // leave the buffer open
		FlushBatch,    // submit: enough events are held
		FlushDeadline, // submit: the oldest held event is too old
	};

	explicit SubmitCoalescer(SubmitCoalescerConfig config = {}): m_config(config) {}

	// An event wants a submit at time `now`.
	[[nodiscard]] Decision OnEvent(uint64_t now) {
		if (m_pending == 0) {
			m_since = now;
		}
		++m_pending;
		if (m_pending >= m_config.max_events) {
			return Decision::FlushBatch;
		}
		if (now - m_since >= m_config.max_age) {
			return Decision::FlushDeadline;
		}
		return Decision::Defer;
	}

	// Polled between packets: a held event must not outlive the age bound even if no further
	// event arrives.
	[[nodiscard]] bool Due(uint64_t now) const {
		return m_pending != 0 && now - m_since >= m_config.max_age;
	}

	// Anything submitted (by us or by any other flush) clears what was held.
	// Returns how many events had been held back.
	uint32_t Flushed() {
		const auto held = m_pending;
		m_pending       = 0;
		return held;
	}

	[[nodiscard]] uint32_t Pending() const { return m_pending; }

private:
	SubmitCoalescerConfig m_config;
	uint32_t              m_pending = 0;
	uint64_t              m_since   = 0;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_SUBMITCOALESCER_H_ */
