#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESOLVEDDRAWSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESOLVEDDRAWSTATS_H_

// Counters of --resolved-draw (notes/resolved-draw.md): how often kyGpu used a precomputed part of a
// draw, how often it fell back to computing it, and how many used parts the verifier compared.
// Relaxed atomics written by kyGpu only; read for the exit summary.

#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace Libs::Graphics::ResolvedDraw {

// One writer (kyGpu), so a plain relaxed load and store counts exactly and costs a mov instead of the
// locked add of fetch_add (about 20 cycles each, several per draw).
inline void Bump(std::atomic<uint64_t>& counter) {
	counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

struct Counter {
	std::atomic<uint64_t> used {0};      // the precomputed part was taken
	std::atomic<uint64_t> fallback {0};  // there was one, but it could not be used (checked and refused)
	std::atomic<uint64_t> absent {0};    // the plan carried none (producer had nothing, or flag was off)
	std::atomic<uint64_t> verified {0};  // used parts also computed inline and compared (verify mode)
	void Used() { Bump(used); }
	void Fallback() { Bump(fallback); }
	void Absent() { Bump(absent); }
	void Verified() { Bump(verified); }
	[[nodiscard]] std::string Text(const char* name) const {
		return fmt::format("{}: used={} fallback={} absent={} verified={}", name,
		                   used.load(std::memory_order_relaxed), fallback.load(std::memory_order_relaxed),
		                   absent.load(std::memory_order_relaxed), verified.load(std::memory_order_relaxed));
	}
};

struct Stats {
	Counter hazard_hull;
	Counter permutation;
	Counter color_target; // DescribeRenderColorTarget memo
	Counter depth_target; // DescribeRenderDepthTarget memo
};

inline Stats& GetStats() {
	static Stats stats;
	return stats;
}

[[nodiscard]] inline std::string SummaryText() {
	auto& s = GetStats();
	return "ResolvedDraw " + s.hazard_hull.Text("hazard_hull") + "; " + s.permutation.Text("permutation") + "; " + s.color_target.Text("color_target") + "; " + s.depth_target.Text("depth_target");
}

} // namespace Libs::Graphics::ResolvedDraw

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESOLVEDDRAWSTATS_H_
