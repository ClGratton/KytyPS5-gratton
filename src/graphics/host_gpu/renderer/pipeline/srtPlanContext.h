#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLANCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLANCONTEXT_H_

// --srt-plan-shadow: the draw event a thread is processing, and the key of a plan (see srtPlan.h).
// Kept apart from srtPlan.h so the command processor can include it without the shader headers.

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::SrtPlan {

// (--srt-plan-shadow or --srt-plans) and --pipeline-lookahead are on: the lookahead keeps plans and
// the command processors number the draws.
[[nodiscard]] bool Enabled();

// seq: submission sequence number (GuestGpu::Enqueue, shared by the lookahead copy and the real
// submission); ordinal: draw/dispatch packet count inside the submission (1-based); call: index of
// the ProgramCache::Get() call inside that draw (pixel stage first, then vertex stages).
struct Key {
	uint64_t seq     = 0;
	uint32_t ordinal = 0;
	uint32_t call    = 0;
	bool     operator==(const Key&) const = default;
};

struct KeyHash {
	std::size_t operator()(const Key& key) const noexcept {
		uint64_t h = key.seq * 0x9e3779b97f4a7c15ull;
		h ^= (static_cast<uint64_t>(key.ordinal) << 20u) ^ (static_cast<uint64_t>(key.call) << 52u);
		h ^= h >> 29u;
		h *= 0xbf58476d1ce4e5b9ull;
		h ^= h >> 32u;
		return static_cast<std::size_t>(h);
	}
};

// Order of events in the lookahead's processing order (submissions are processed in sequence).
struct Position {
	uint64_t seq     = 0;
	uint32_t ordinal = 0;
	auto     operator<=>(const Position&) const = default;
};

inline Position PositionOf(const Key& key) {
	return {key.seq, key.ordinal};
}

// The draw event the calling thread is processing (set by the command processor, shadow or real).
struct Context {
	uint64_t seq     = 0;
	uint32_t ordinal = 0;
	uint32_t sub     = 0; // nested draw of an indirect multi-draw (the shadow reports only the first)
	uint32_t call    = 0; // Get() calls made so far in this event
	bool     active  = false;
};

inline thread_local Context t_context;

// The key of the next Get() call of the current event, or false outside a draw event.
inline bool NextCall(Key& key, bool& nested) noexcept {
	auto& context = t_context;
	if (!context.active) return false;
	key    = {context.seq, context.ordinal, context.call++};
	nested = context.sub != 0;
	return true;
}

class EventScope {
public:
	EventScope(uint64_t seq, uint32_t ordinal, uint32_t sub) noexcept: m_previous(t_context) {
		t_context = {seq, ordinal, sub, 0, true};
	}
	~EventScope() { t_context = m_previous; }
	EventScope(const EventScope&)            = delete;
	EventScope& operator=(const EventScope&) = delete;

private:
	Context m_previous;
};

} // namespace Libs::Graphics::SrtPlan

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLANCONTEXT_H_
