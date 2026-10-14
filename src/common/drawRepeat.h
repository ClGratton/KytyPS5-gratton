#ifndef KYTY_COMMON_DRAW_REPEAT_H_
#define KYTY_COMMON_DRAW_REPEAT_H_

// KYTY_DRAW_REPEAT=<csv path> (env, debug only, log only, nothing on by default).
//
// Question it answers: how many of the draws of a frame are identical to the same draw of the
// previous frame (same registers, same bindings, same dwords read by the shader's scalar walk)? If
// most are, work that depends only on those inputs (descriptor writes, SRT walks, hazard checks) could
// be reused across frames; if few are, it cannot.
//
// Runs on the lookahead thread only (the shadow command processor sees every draw in submission
// order). Per draw it hashes five groups of inputs:
//   ctx    - the context registers (render targets, viewport, blend, depth, ...)
//   sh     - the shader stage registers and user data (shader bases, root-table pointers, user SGPRs)
//   args   - user config registers and the draw arguments (primitive type, counts, index type)
//   reads  - the dwords the shader's scalar-read walk read from guest memory, with its inputs
//            (from the SRT plan the lookahead builds for the draw)
// A draw is matched to the previous frame's draw by (ordinal of its submission within the frame,
// program, occurrence of that program within the submission). A frame ends at a SuspendPoint item.
// Output: one CSV row per frame, see drawRepeat.cpp.

#include <cstddef>
#include <cstdint>

namespace Common::DrawRepeat {

[[nodiscard]] bool Enabled() noexcept;

enum Kind : uint32_t { Draw = 0, Dispatch = 1 };

struct Inputs {
	Kind     kind    = Draw;
	uint64_t program = 0; // identity of the shaders (code addresses)
	uint64_t ctx     = 0;
	uint64_t sh      = 0;
	uint64_t args    = 0;
};

[[nodiscard]] uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0) noexcept;
[[nodiscard]] inline uint64_t Mix(uint64_t h, uint64_t v) noexcept {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// Lookahead thread. `reset` is a SuspendPoint item (frame boundary), otherwise a submission.
void BeginItem(bool reset) noexcept;
// A plan was stored for the draw being prepared: fold its reads into the draw's signature.
void NoteReads(uint64_t hash) noexcept;
// The draw (or dispatch) has been looked at. Takes the reads noted since the previous call.
void NoteDraw(const Inputs& inputs) noexcept;

} // namespace Common::DrawRepeat

#endif
