#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCLOCK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCLOCK_H_

// The arithmetic behind the guest-visible GPU clock. Header-only and free of Vulkan and global
// state, so tests/GpuClockTests.cpp can exercise it directly.
//
//  - TscToReferenceClock: host TSC ticks -> the 100 MHz GPU reference clock the guest reads.
//  - DeviceCalibration, DeviceTicksToTsc, HostSampleToTsc: a Vulkan timestamp (device ticks) ->
//    host TSC, from one calibration pair taken with VK_EXT_calibrated_timestamps.
//  - TimeScaler: --gpu-time-scale, a generic bias on the GPU durations a guest observes.
//
// Idea for the scale model: BryanKAdams/BryKytyPS5 (GPL-2.0) b44c4edeb, --gpu-timestamp-scale.
// Idea for GPU-true timestamps: ogeidexe/KytyPS5 (GPL-2.0) 1936f6169. Both re-implemented here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace Libs::Graphics::GpuClock {

// Frequency of the GPU reference clock (EVENT_WRITE_EOP / COPY_DATA clock sources).
constexpr uint64_t REFERENCE_FREQUENCY = 100000000;

// Converts host ticks to the reference clock; false on a zero frequency or 64-bit overflow.
[[nodiscard]] inline bool TscToReferenceClock(uint64_t host_ticks, uint64_t host_frequency,
                                              uint64_t& value) {
	if (host_frequency == 0) {
		return false;
	}
	const auto     whole_seconds = host_ticks / host_frequency;
	const auto     remainder     = host_ticks % host_frequency;
	constexpr auto MAX_VALUE     = std::numeric_limits<uint64_t>::max();
	if (whole_seconds > MAX_VALUE / REFERENCE_FREQUENCY ||
	    remainder > MAX_VALUE / REFERENCE_FREQUENCY) {
		return false;
	}
	const auto whole_value      = whole_seconds * REFERENCE_FREQUENCY;
	const auto fractional_value = (remainder * REFERENCE_FREQUENCY) / host_frequency;
	if (whole_value > MAX_VALUE - fractional_value) {
		return false;
	}
	value = whole_value + fractional_value;
	return true;
}

// Mask of the valid low bits of a device timestamp (VkQueueFamilyProperties::timestampValidBits).
[[nodiscard]] constexpr uint64_t ValidMask(uint32_t valid_bits) {
	return valid_bits >= 64 ? ~uint64_t {0} : ((uint64_t {1} << valid_bits) - 1u);
}

// A device timestamp and the host TSC at the same instant.
struct DeviceCalibration {
	uint64_t device_ticks  = 0; // raw query value (masked on use)
	uint64_t host_tsc      = 0;
	double   ns_per_tick   = 1.0; // VkPhysicalDeviceLimits::timestampPeriod
	uint32_t valid_bits    = 64;
	uint64_t tsc_frequency = 0;
};

// ticks - base on a counter that wraps at valid_bits, as a signed distance (the shorter way
// round), so a timestamp slightly before the calibration point maps to an earlier time.
[[nodiscard]] inline int64_t SignedTickDelta(uint64_t ticks, uint64_t base, uint32_t valid_bits) {
	const auto mask  = ValidMask(valid_bits);
	const auto delta = (ticks - base) & mask;
	if (valid_bits >= 64) {
		return static_cast<int64_t>(delta);
	}
	const auto half = mask / 2u + 1u;
	return delta >= half ? static_cast<int64_t>(delta) - static_cast<int64_t>(mask + 1u)
	                     : static_cast<int64_t>(delta);
}

// Host TSC at a device timestamp. False if the calibration is unusable or the result would be
// before TSC 0.
[[nodiscard]] inline bool DeviceTicksToTsc(const DeviceCalibration& c, uint64_t ticks,
                                           uint64_t& tsc) {
	if (c.tsc_frequency == 0 || !(c.ns_per_tick > 0.0) || c.valid_bits == 0) {
		return false;
	}
	const auto   delta_ticks = SignedTickDelta(ticks, c.device_ticks, c.valid_bits);
	const double delta_tsc =
	    static_cast<double>(delta_ticks) * c.ns_per_tick * static_cast<double>(c.tsc_frequency) /
	    1e9;
	const double value = static_cast<double>(c.host_tsc) + delta_tsc;
	if (!(value >= 0.0) || value >= 18446744073709551615.0) {
		return false;
	}
	tsc = static_cast<uint64_t>(value);
	return true;
}

// Host TSC at the moment of a host-clock sample (nanoseconds) taken a little before now_ns /
// now_tsc were read. A sample from the future (clock skew) is treated as "now".
[[nodiscard]] inline uint64_t HostSampleToTsc(uint64_t sample_ns, uint64_t now_ns,
                                              uint64_t now_tsc, uint64_t tsc_frequency) {
	const auto ago_ns = now_ns > sample_ns ? now_ns - sample_ns : uint64_t {0};
	const auto ago_tsc =
	    static_cast<uint64_t>(static_cast<double>(ago_ns) * static_cast<double>(tsc_frequency) /
	                          1e9);
	return now_tsc > ago_tsc ? now_tsc - ago_tsc : uint64_t {0};
}

// --gpu-time-scale: the GPU durations a guest observes are multiplied by `scale`, so a game that
// sizes its dynamic resolution from them renders lower (> 1) or higher (< 1). Times are stretched
// from the latest frame boundary (a guest flip) before them and snap back to the real clock at
// each boundary, so a stamp is never more than one frame's stretch from the real clock and
// durations inside a frame scale exactly. Durations across a boundary are not scaled. The
// stretch of one frame is capped (a long gap without flips, such as a loading screen, must not
// push the clock far ahead).
//
// All times are in one domain (reference clock ticks), either parse time or GPU execution time;
// use one scaler per domain.
class TimeScaler {
public:
	static constexpr double   MIN_SCALE         = 0.1;
	static constexpr double   MAX_SCALE         = 16.0;
	static constexpr uint64_t MAX_STRETCH_TICKS = REFERENCE_FREQUENCY / 10; // 100 ms
	static constexpr size_t   MAX_BOUNDARIES    = 64;

	explicit TimeScaler(double scale = 1.0) {
		SetScale(scale);
	}

	void SetScale(double scale) {
		std::lock_guard lock(m_mutex);
		m_scale = std::isfinite(scale) ? std::clamp(scale, MIN_SCALE, MAX_SCALE) : 1.0;
	}

	[[nodiscard]] double Scale() const {
		std::lock_guard lock(m_mutex);
		return m_scale;
	}

	// A frame boundary at time t. Boundaries arrive in increasing order; an older one is ignored.
	void AddBoundary(uint64_t t) {
		std::lock_guard lock(m_mutex);
		if (!m_boundaries.empty() && t <= m_boundaries.back()) {
			return;
		}
		if (m_boundaries.size() >= MAX_BOUNDARIES) {
			m_boundaries.erase(m_boundaries.begin(), m_boundaries.begin() + MAX_BOUNDARIES / 2);
		}
		m_boundaries.push_back(t);
	}

	[[nodiscard]] uint64_t Apply(uint64_t t) const {
		std::lock_guard lock(m_mutex);
		if (m_scale == 1.0 || m_boundaries.empty()) {
			return t;
		}
		const auto it = std::upper_bound(m_boundaries.begin(), m_boundaries.end(), t);
		if (it == m_boundaries.begin()) {
			return t; // before the first known boundary: nothing to stretch from
		}
		const auto   anchor  = *(it - 1);
		const auto   elapsed = t - anchor;
		const auto   stretch = std::min(elapsed, MAX_STRETCH_TICKS);
		const double scaled  = static_cast<double>(stretch) * m_scale;
		return anchor + static_cast<uint64_t>(scaled) + (elapsed - stretch);
	}

private:
	mutable std::mutex    m_mutex;
	double                m_scale = 1.0;
	std::vector<uint64_t> m_boundaries;
};

} // namespace Libs::Graphics::GpuClock

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCLOCK_H_
