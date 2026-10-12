#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MATERIALIZEPROBE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_MATERIALIZEPROBE_H_

// Diagnostic only: cycle counters around the sections of MaterializeResources(), switched on by the
// renderer when --gpu-phase-stats is on (one relaxed load per section otherwise). The renderer prints
// them with the phase report. Header only so the unit-test targets that compile
// ResourceMaterialization.cpp alone need nothing else.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR::MaterializeProbe {

enum Slot : uint32_t { Total, Setup, Flat, Fill, Buffers, Images, Samplers, Specialize, Count };

inline constexpr std::array<const char*, Count> kNames = {"total",   "setup",  "flat",     "fill",
                                                          "buffers", "images", "samplers", "specialize"};

inline std::atomic<bool>                             g_enabled {false};
inline std::array<std::atomic<uint64_t>, Count>     g_ticks {};
inline std::array<std::atomic<uint64_t>, Count>     g_calls {};

class Timer {
public:
	explicit Timer(Slot slot) noexcept: m_slot(slot) {
		if (g_enabled.load(std::memory_order_relaxed)) {
			m_start = __builtin_ia32_rdtsc();
			m_on    = true;
		}
	}
	~Timer() { Stop(); }
	void Stop() noexcept {
		if (m_on) {
			g_ticks[m_slot].fetch_add(__builtin_ia32_rdtsc() - m_start, std::memory_order_relaxed);
			g_calls[m_slot].fetch_add(1, std::memory_order_relaxed);
			m_on = false;
		}
	}
	Timer(const Timer&)            = delete;
	Timer& operator=(const Timer&) = delete;

private:
	Slot     m_slot;
	uint64_t m_start = 0;
	bool     m_on    = false;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR::MaterializeProbe

#endif
