#include "graphics/host_gpu/renderer/gpuClock.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics::GpuClock;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GpuClockTests: failed: %s\n", message);
		std::abort();
	}
}

bool Near(double a, double b, double tolerance) {
	return std::fabs(a - b) <= tolerance;
}

void TestReferenceClock() {
	uint64_t value = 0;
	Check(TscToReferenceClock(0, 3000000000ull, value) && value == 0, "zero ticks");
	Check(TscToReferenceClock(1500000000ull, 3000000000ull, value) && value == 50000000ull,
	      "half a second is 50 M reference ticks");
	Check(TscToReferenceClock(3750000001ull, 3000000000ull, value) && value == 125000000ull,
	      "fractional part rounds down");
	Check(!TscToReferenceClock(1, 0, value), "zero frequency is rejected");
	Check(!TscToReferenceClock(UINT64_MAX, 1, value), "overflow is rejected");
}

void TestValidMask() {
	Check(ValidMask(64) == UINT64_MAX, "64 valid bits");
	Check(ValidMask(36) == 0xfffffffffull, "36 valid bits");
	Check(ValidMask(1) == 1u, "1 valid bit");
}

void TestSignedTickDelta() {
	Check(SignedTickDelta(1500, 1000, 64) == 500, "forward, 64 bit");
	Check(SignedTickDelta(900, 1000, 64) == -100, "backward, 64 bit");
	// A 32-bit counter that wrapped between the calibration and the stamp.
	Check(SignedTickDelta(10, 0xfffffff0u, 32) == 26, "wrap forward, 32 bit");
	Check(SignedTickDelta(0xfffffff0u, 10, 32) == -26, "wrap backward, 32 bit");
	// Garbage above the valid bits is ignored.
	Check(SignedTickDelta(0xabcd00000010ull, 0xffff00000000ull, 32) == 16, "upper bits ignored");
	// The exact half is treated as backward (two's complement midpoint).
	Check(SignedTickDelta(0x80000000u, 0, 32) == -2147483648ll, "half range is negative");
	Check(SignedTickDelta(0x7fffffffu, 0, 32) == 2147483647ll, "just under half is positive");
}

void TestDeviceTicksToTsc() {
	// 1 GHz device clock (1 ns per tick), 3 GHz TSC: one device tick is three TSC ticks.
	DeviceCalibration c;
	c.device_ticks  = 1000000;
	c.host_tsc      = 9000000000ull;
	c.ns_per_tick   = 1.0;
	c.valid_bits    = 64;
	c.tsc_frequency = 3000000000ull;
	uint64_t tsc = 0;
	Check(DeviceTicksToTsc(c, 1000000, tsc) && tsc == 9000000000ull, "calibration point maps to itself");
	Check(DeviceTicksToTsc(c, 2000000, tsc) && tsc == 9003000000ull, "1 ms later is 3 M TSC ticks");
	Check(DeviceTicksToTsc(c, 500000, tsc) && tsc == 8998500000ull, "before the calibration point");

	// NVIDIA-style 1 ns period vs AMD-style 10 ns period: same wall time, different ticks.
	DeviceCalibration slow = c;
	slow.ns_per_tick       = 10.0;
	Check(DeviceTicksToTsc(slow, 1000000 + 100000, tsc) && tsc == 9003000000ull,
	      "timestampPeriod scales ticks to ns");

	// A 36-bit counter wrapping after the calibration.
	DeviceCalibration wrap = c;
	wrap.valid_bits        = 36;
	wrap.device_ticks      = ValidMask(36) - 99;
	Check(DeviceTicksToTsc(wrap, 100, tsc) && tsc == 9000000000ull + 200ull * 3u,
	      "wrapped timestamp is 200 ticks after the calibration point");

	// Unusable calibrations and results before TSC 0.
	DeviceCalibration bad = c;
	bad.tsc_frequency     = 0;
	Check(!DeviceTicksToTsc(bad, 1, tsc), "zero TSC frequency");
	bad                   = c;
	bad.ns_per_tick       = 0.0;
	Check(!DeviceTicksToTsc(bad, 1, tsc), "zero timestamp period");
	bad                   = c;
	bad.host_tsc          = 100;
	Check(!DeviceTicksToTsc(bad, 0, tsc), "a time before TSC 0 is rejected");
}

void TestHostSampleToTsc() {
	// The sample was taken 2 ms before "now" on a 3 GHz TSC: 6 M ticks earlier.
	Check(HostSampleToTsc(1000000000ull, 1002000000ull, 50000000000ull, 3000000000ull) ==
	          50000000000ull - 6000000ull,
	      "sample 2 ms ago");
	Check(HostSampleToTsc(5, 5, 777, 3000000000ull) == 777, "sample taken now");
	Check(HostSampleToTsc(20, 10, 777, 3000000000ull) == 777, "a sample from the future is now");
	Check(HostSampleToTsc(0, 1000000000ull, 100, 3000000000ull) == 0, "does not underflow");
}

void TestCalibrationEndToEnd() {
	// Device time of a stamp 5 ms after calibration, converted all the way to the guest clock.
	DeviceCalibration c;
	c.device_ticks  = 0x1000;
	c.ns_per_tick   = 1.0;
	c.valid_bits    = 64;
	c.tsc_frequency = 3000000000ull;
	// The host sample was 1 ms before now (TSC 30 s), the calibration pair is at TSC 30 s - 3 M.
	c.host_tsc = HostSampleToTsc(100000000ull, 101000000ull, 90000000000ull, c.tsc_frequency);
	Check(c.host_tsc == 90000000000ull - 3000000ull, "calibration TSC");
	uint64_t tsc = 0;
	uint64_t ref = 0;
	Check(DeviceTicksToTsc(c, 0x1000 + 5000000, tsc), "stamp 5 ms later converts");
	Check(TscToReferenceClock(tsc, c.tsc_frequency, ref), "to the reference clock");
	// 30 s - 1 ms + 5 ms = 30.004 s -> 3,000,400,000 reference ticks.
	Check(ref == 3000400000ull, "guest clock value");
}

void TestScalerIdentity() {
	TimeScaler s;
	s.AddBoundary(1000);
	Check(s.Apply(5000) == 5000, "scale 1.0 is the identity");
	s.SetScale(2.0);
	TimeScaler empty;
	empty.SetScale(2.0);
	Check(empty.Apply(5000) == 5000, "no boundary yet: unchanged");
	Check(s.Apply(900) == 900, "before the first boundary: unchanged");
	Check(s.Apply(1000) == 1000, "at a boundary: unchanged");
}

void TestScalerStretch() {
	TimeScaler s;
	s.SetScale(1.5);
	s.AddBoundary(1000000);
	Check(s.Apply(1000000 + 200000) == 1000000 + 300000, "offset from the boundary is stretched");
	// Durations inside one frame scale exactly.
	const auto begin = s.Apply(1100000);
	const auto end   = s.Apply(1500000);
	Check(end - begin == 600000, "a 400000 tick duration reads 600000 at scale 1.5");

	s.SetScale(0.5);
	Check(s.Apply(1200000) == 1100000, "a scale below 1 compresses");
	Check(s.Scale() == 0.5, "scale is stored");
}

void TestScalerSnapsBackAtBoundaries() {
	TimeScaler s;
	s.SetScale(2.0);
	s.AddBoundary(1000000);
	s.AddBoundary(1000000 + 1666666);
	// Inside frame 2 the clock is measured from boundary 2, not from boundary 1.
	Check(s.Apply(1000000 + 1666666 + 1000) == 1000000 + 1666666 + 2000, "second frame anchors on boundary 2");
	// The last stamp of frame 1 is ahead of the first stamp of frame 2: not monotonic across a flip.
	Check(s.Apply(1000000 + 1666000) > s.Apply(1000000 + 1666666 + 1), "snaps back to real at a flip");
	// Never more than the capped stretch ahead of the real clock.
	Check(s.Apply(1000000 + 1666666 - 1) - (1000000 + 1666666 - 1) <= TimeScaler::MAX_STRETCH_TICKS,
	      "stretch stays bounded");
}

void TestScalerCapsLongGaps() {
	TimeScaler s;
	s.SetScale(4.0);
	s.AddBoundary(0);
	const uint64_t cap = TimeScaler::MAX_STRETCH_TICKS;
	Check(s.Apply(cap) == cap * 4u, "up to the cap the stretch is full");
	Check(s.Apply(cap + 1000) == cap * 4u + 1000, "beyond the cap time runs at the real rate");
	Check(s.Apply(10 * cap) == cap * 4u + 9 * cap, "a long gap adds only the capped stretch");
}

void TestScalerBoundaryOrderAndLimit() {
	TimeScaler s;
	s.SetScale(2.0);
	s.AddBoundary(5000);
	s.AddBoundary(4000); // older: ignored
	s.AddBoundary(5000); // duplicate: ignored
	Check(s.Apply(5100) == 5200, "late boundaries are ignored");
	for (uint64_t i = 1; i <= 500; i++) {
		s.AddBoundary(5000 + i * 1000);
	}
	// Plenty of boundaries were trimmed; the newest still anchor correctly.
	Check(s.Apply(5000 + 500 * 1000 + 10) == 5000 + 500 * 1000 + 20, "newest boundary still anchors");
	Check(s.Apply(5000 + 480 * 1000 + 10) == 5000 + 480 * 1000 + 20, "recent boundary still anchors");
}

void TestScalerRejectsBadScale() {
	TimeScaler s;
	s.SetScale(std::nan(""));
	Check(s.Scale() == 1.0, "NaN falls back to 1.0");
	s.SetScale(1000.0);
	Check(s.Scale() == TimeScaler::MAX_SCALE, "clamped to the maximum");
	s.SetScale(0.0);
	Check(s.Scale() == TimeScaler::MIN_SCALE, "clamped to the minimum");
	Check(Near(s.Scale(), 0.1, 1e-12), "minimum value");
}

} // namespace

int main() {
	TestReferenceClock();
	TestValidMask();
	TestSignedTickDelta();
	TestDeviceTicksToTsc();
	TestHostSampleToTsc();
	TestCalibrationEndToEnd();
	TestScalerIdentity();
	TestScalerStretch();
	TestScalerSnapsBackAtBoundaries();
	TestScalerCapsLongGaps();
	TestScalerBoundaryOrderAndLimit();
	TestScalerRejectsBadScale();
	std::puts("GpuClockTests: all passed");
	return 0;
}
