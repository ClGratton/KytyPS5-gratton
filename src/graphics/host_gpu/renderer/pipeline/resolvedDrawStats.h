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

struct Counter {
	std::atomic<uint64_t> used {0};      // the precomputed part was taken
	std::atomic<uint64_t> fallback {0};  // there was one, but it could not be used (checked and refused)
	std::atomic<uint64_t> absent {0};    // the plan carried none (producer had nothing, or flag was off)
	std::atomic<uint64_t> verified {0};  // used parts also computed inline and compared (verify mode)
	void Used() { used.fetch_add(1, std::memory_order_relaxed); }
	void Fallback() { fallback.fetch_add(1, std::memory_order_relaxed); }
	void Absent() { absent.fetch_add(1, std::memory_order_relaxed); }
	void Verified() { verified.fetch_add(1, std::memory_order_relaxed); }
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
};

inline Stats& GetStats() {
	static Stats stats;
	return stats;
}

[[nodiscard]] inline std::string SummaryText() {
	auto& s = GetStats();
	return "ResolvedDraw " + s.hazard_hull.Text("hazard_hull") + "; " + s.permutation.Text("permutation") + "; " + s.color_target.Text("color_target");
}

} // namespace Libs::Graphics::ResolvedDraw

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_RESOLVEDDRAWSTATS_H_
