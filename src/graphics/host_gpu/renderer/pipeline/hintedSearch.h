#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HINTEDSEARCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HINTEDSEARCH_H_

// --permutation-hint: a linear search over a growing container that tries the last hit first.
//
// SourceEntry::FindExistingLocked scans every published permutation of a shader in publication order,
// comparing a deep ResourceSpecialization with each, on every draw; consecutive draws of a shader
// mostly hit the same permutation, so the hint turns the common scan into one comparison.
//
// Exact under one precondition the caller guarantees: at most one element satisfies `match` (then the
// first match and the hinted match are the same element). The search itself never changes which
// element a scan returns in that case; tests/HintedSearchTests.cpp checks it against the plain scan on
// sets with unique matches and shows the documented difference when matches repeat.

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace Libs::Graphics {

// `items` is indexable (a deque whose elements never move), `hint` is shared between threads that
// search under a shared lock, so it is an atomic with relaxed ordering (a stale hint only costs a scan).
template <typename Items, typename Match>
auto* FindWithHint(Items& items, std::atomic<size_t>& hint, bool use_hint, Match&& match) {
	using Element = std::remove_reference_t<decltype(items[0])>;
	Element* none = nullptr;
	if (use_hint) {
		const size_t guess = hint.load(std::memory_order_relaxed);
		if (guess < items.size() && match(items[guess])) {
			return &items[guess];
		}
	}
	const size_t count = items.size();
	for (size_t i = 0; i < count; ++i) {
		if (match(items[i])) {
			hint.store(i, std::memory_order_relaxed);
			return &items[i];
		}
	}
	return none;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_HINTEDSEARCH_H_
