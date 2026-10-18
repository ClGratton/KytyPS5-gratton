#pragma once

// --incremental-draw: a draw keeps what the draw before it resolved, and redoes only what changed.
//
// Consecutive draws bind almost the same textures and buffers (an image slot's descriptor is unchanged in 84 % of the draws that
// bind it), yet every draw used to look each binding up again, touch it, check it for guest writes, find its view, check its
// layout and write its descriptor. A kept binding skips all of that when the inputs it was computed from are unchanged:
//   - the descriptor dwords and the shader's resource record it was decoded for (compared, never trusted),
//   - the texture cache's image-set generation (any image registered, unregistered or re-associated moves it),
//   - the image's own state, read directly each draw (registered, nothing to rebind, no pending guest write, no colour metadata
//     that a bind would materialize), so a state that does move without a generation change makes the binding fall back.
// Anything not proven unchanged takes the ordinary path, which recomputes and re-keeps the binding.
//
// KYTY_DRAW_VERIFY=1 runs the ordinary path next to every kept result and aborts on a difference; KYTY_DRAW_STATS=1 prints how
// often a binding was kept and why one was not.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::IncrementalDraw {

[[nodiscard]] inline bool EnvFlag(const char* name) noexcept {
	const char* text = std::getenv(name);
	return text != nullptr && text[0] != '\0' && text[0] != '0';
}

// Every kept result is compared with a fresh one (EXIT on a difference) and counted.
[[nodiscard]] inline bool VerifyEnabled() noexcept {
	static const bool enabled = EnvFlag("KYTY_DRAW_VERIFY");
	return enabled;
}

[[nodiscard]] inline bool StatsEnabled() noexcept {
	static const bool enabled = EnvFlag("KYTY_DRAW_STATS") || EnvFlag("KYTY_DRAW_VERIFY");
	return enabled;
}

// Counters of the GPU thread (no atomics: only kyGpu counts). Printed every 2^20 image slots.
struct ImageSlotStats {
	uint64_t slots           = 0; // image slots prepared
	uint64_t kept_offered    = 0; // slots that were kept by the draw before
	uint64_t miss_input      = 0; // ... but the descriptor or the shader resource changed
	uint64_t miss_replay     = 0; // ... or the image state ruled out the replay (set moved, colour metadata, ...)
	uint64_t replayed        = 0; // slots that skipped the lookup (first half)
	uint64_t rebind_skipped  = 0; // ... and the rebind (second half)
	uint64_t rebind_fell_back = 0;
	uint64_t keep_set        = 0; // slots kept at the end of a full rebind
	uint64_t keep_refused    = 0; // slots a full rebind could not keep (storage, depth, mips, ...)
	uint64_t transit_skipped = 0; // Image::Transit calls proved to record nothing
	uint64_t transit_called  = 0;
	uint64_t verified        = 0;
};

[[nodiscard]] inline ImageSlotStats& Stats() noexcept {
	static thread_local ImageSlotStats stats;
	return stats;
}

inline void MaybePrint(const ImageSlotStats& s) noexcept {
	if ((s.slots & ((1u << 20) - 1u)) != 0) return;
	std::printf("[incremental-draw] image slots %llu: offered %.1f%% (input miss %.1f%%, replay miss %.1f%%), replayed %.1f%%, rebind skipped %.1f%% "
	            "(fell back %llu), kept %llu refused %llu, transit skipped %.1f%% of %llu, verified %llu\n",
	            static_cast<unsigned long long>(s.slots), 100.0 * s.kept_offered / s.slots, 100.0 * s.miss_input / s.slots,
	            100.0 * s.miss_replay / s.slots, 100.0 * s.replayed / s.slots, 100.0 * s.rebind_skipped / s.slots,
	            static_cast<unsigned long long>(s.rebind_fell_back), static_cast<unsigned long long>(s.keep_set),
	            static_cast<unsigned long long>(s.keep_refused),
	            s.transit_called != 0 ? 100.0 * s.transit_skipped / s.transit_called : 0.0,
	            static_cast<unsigned long long>(s.transit_called), static_cast<unsigned long long>(s.verified));
	std::fflush(stdout);
}

} // namespace Libs::Graphics::IncrementalDraw
