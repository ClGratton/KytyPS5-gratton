#include "graphics/host_gpu/renderer/gpuClock.h"
#include "graphics/presentation/fullSpeed.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Libs::Graphics::FullSpeed::Policy;
using Libs::Graphics::FullSpeed::PolicyParams;
using Libs::Graphics::GpuClock::TimeScaler;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FullSpeedTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t MS = 1000000ull;
constexpr uint64_t T  = 16666667ull; // one 60 Hz frame

// Runs the policy against a model of the emulator: a rendered frame takes `render_ms`, a skipped
// frame `skip_ms` (never less than... nothing: skipped flips complete early), for `seconds`.
// Returns {flips per second, rendered per second}.
struct Rates {
	double flips;
	double rendered;
	double lag_ms;
};

Rates Simulate(Policy& policy, double render_ms, double skip_ms, double seconds, uint64_t interval = T) {
	uint64_t now     = 1000 * MS;
	bool     skipped = false;
	uint64_t flips   = 0;
	uint64_t rendered = 0;
	const uint64_t end = now + static_cast<uint64_t>(seconds * 1e9);
	while (now < end) {
		// The frame that is running now ends at the next flip.
		double cost_ms = skipped ? skip_ms : render_ms;
		// A frame cannot take less than the interval when nothing is skipped (vsync); a skipped
		// frame completes early.
		if (!skipped && cost_ms * 1e6 < static_cast<double>(interval)) {
			cost_ms = static_cast<double>(interval) * 1e-6;
		}
		now += static_cast<uint64_t>(cost_ms * 1e6);
		flips++;
		if (!skipped) {
			rendered++;
		}
		skipped = policy.OnFlip(now, interval);
	}
	return {static_cast<double>(flips) / seconds, static_cast<double>(rendered) / seconds,
	        policy.LagNs() * 1e-6};
}

void TestFastGpuNeverSkips() {
	Policy p;
	const auto r = Simulate(p, 10.0, 3.0, 5.0);
	Check(p.Skipped() == 0, "frames cheaper than the interval are never skipped");
	Check(r.flips > 59.0 && r.flips < 61.0, "at the vsync rate");
}

void TestSlowGpuReachesTargetRate() {
	Policy p;
	const auto r = Simulate(p, 45.0, 6.0, 20.0);
	Check(r.flips > 57.0 && r.flips < 63.0, "45 ms frames still give 60 flips per second");
	Check(r.rendered > 8.0 && r.rendered < 30.0, "some frames are still drawn");
	Check(p.Skipped() > p.Rendered(), "most frames are skipped");
}

void TestUselessSkipRenders() {
	// The guest is the limit (25 ms either way): skipping does not help, so it hardly happens.
	Policy p;
	const auto r = Simulate(p, 25.0, 24.0, 20.0);
	Check(r.flips > 38.0 && r.flips < 42.0, "the limit is the guest, 40 flips per second");
	Check(r.rendered > 0.9 * r.flips, "almost every frame is drawn");
}

void TestMinimumRenderRate() {
	// Skipped frames cost as much as the interval: the lag never repays, the picture still moves.
	Policy p;
	const auto r = Simulate(p, 80.0, 14.0, 20.0);
	Check(r.rendered >= 4.0, "at least the minimum render rate");
}

void TestStallIsForgiven() {
	Policy p;
	Simulate(p, 45.0, 6.0, 3.0);
	uint64_t now = 100000 * MS;
	p.OnFlip(now, T);
	now += 3000 * MS; // a 3 s loading screen
	p.OnFlip(now, T);
	Check(p.LagNs() <= 12.0 * static_cast<double>(T) + 1.0, "lag is capped after a stall");
	bool skipped_in_a_row = true;
	for (int i = 0; i < 30; i++) {
		now += 6 * MS;
		skipped_in_a_row = p.OnFlip(now, T) && skipped_in_a_row;
	}
	Check(p.LagNs() < 12.0 * static_cast<double>(T), "the debt shrinks with skipped frames");
}

void TestThirtyFpsTitle() {
	// A title that flips every second vblank: the interval is 33 ms and frames of 30 ms are on time.
	Policy p;
	const auto r = Simulate(p, 30.0, 4.0, 5.0, 2 * T);
	Check(p.Skipped() == 0, "no skipping when the intended rate is met");
	Check(r.flips > 29.0 && r.flips < 31.0, "30 flips per second");
}

void TestTimeScale() {
	Policy p;
	Check(p.SkippedFrameTimeScale() == 1.0, "no skipped frame, no scale");
	Simulate(p, 45.0, 5.0, 3.0);
	// Run until the current frame is a skipped one.
	uint64_t now = 500000 * MS;
	for (int i = 0; i < 400 && !p.Skipping(); i++) {
		now += 10 * MS;
		p.OnFlip(now, T);
	}
	if (p.Skipping()) {
		const double s = p.SkippedFrameTimeScale();
		Check(s >= 1.0 && s <= 16.0, "scale within the scaler's range");
	}
}

void TestPerBoundaryScale() {
	TimeScaler s; // global scale 1.0
	s.AddBoundary(1000);
	s.AddBoundary(2000, 4.0); // the frame that starts at 2000 is stretched 4x
	s.AddBoundary(3000);
	Check(s.Apply(1500) == 1500, "an ordinary frame is untouched");
	Check(s.Apply(2250) == 3000, "a frame with its own scale stretches from its boundary");
	Check(s.Apply(3250) == 3250, "the next frame is back to the real clock");
	TimeScaler g(0.5);
	g.AddBoundary(1000);
	g.AddBoundary(2000, 2.0);
	Check(g.Apply(1500) == 1250, "a boundary without its own scale uses the global one");
	Check(g.Apply(2500) == 3000, "its own scale replaces the global one");
}

} // namespace

int main() {
	TestFastGpuNeverSkips();
	TestSlowGpuReachesTargetRate();
	TestUselessSkipRenders();
	TestMinimumRenderRate();
	TestStallIsForgiven();
	TestThirtyFpsTitle();
	TestTimeScale();
	TestPerBoundaryScale();
	std::puts("FullSpeedTests: all passed");
	return 0;
}
