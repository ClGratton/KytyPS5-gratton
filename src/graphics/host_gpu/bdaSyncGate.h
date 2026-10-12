#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCGATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCGATE_H_

// Decides when RenderContext::PrepareBda can skip its walk over every buffer of every mapped range
// (K15), unit tested in tests/BdaSyncGateTests.cpp.
//
// A shader that addresses memory through raw pointers needs every buffer to be current, so before
// each such draw PrepareBda synchronizes (uploads the CPU-modified pages of) all of them. A full
// pass leaves the pages clean and write-protected, so a second pass has nothing to do unless
// pages became CPU-modified, a buffer was registered or unregistered, or the mapped ranges
// changed in between. Each of those bumps a generation; the pass is skipped while all three match
// the generations read before the last full pass.
//
// Idea from ogeidexe's fork (ogeidexe/KytyPS5@4a24f1d2d, GPL-2.0), re-implemented with a flag and
// a testable gate.

#include <atomic>
#include <cstdint>

namespace Libs::Graphics::BdaSync {

// Bumped whenever pages become CPU-modified (MemoryTracker also bumps it when a tracking region is
// created, because a new region starts out fully CPU-modified). Bumped after the dirty bits are
// set, so a pass that read the counter earlier either uploads those pages or sees the new value
// the next time.
inline std::atomic<uint64_t> g_cpu_dirty_generation {0};

inline void NoteCpuModified() noexcept {
	g_cpu_dirty_generation.fetch_add(1, std::memory_order_release);
}

struct Generations {
	uint64_t cpu_dirty = 0;
	uint64_t buffers   = 0;
	uint64_t mapped    = 0;

	[[nodiscard]] constexpr bool operator==(const Generations& other) const noexcept {
		return cpu_dirty == other.cpu_dirty && buffers == other.buffers && mapped == other.mapped;
	}
};

class SkipGate {
public:
	// True when the pass can be left out: the feature is enabled, a full pass has run, and nothing
	// changed since the generations it started from.
	[[nodiscard]] bool CanSkip(bool enabled, const Generations& now) const noexcept {
		return enabled && m_valid && now == m_last;
	}

	// Call after a full pass with the generations read BEFORE it started.
	void RecordFullPass(const Generations& before_pass) noexcept {
		m_valid = true;
		m_last  = before_pass;
	}

	void Invalidate() noexcept {
		m_valid = false;
	}

private:
	bool        m_valid = false;
	Generations m_last {};
};

} // namespace Libs::Graphics::BdaSync

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_BDASYNCGATE_H_ */
