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
// see Component below.
// A draw is matched to the previous frame's draw by (ordinal of its submission within the frame,
// program, occurrence of that program within the submission). A frame ends at a SuspendPoint item.
// Output: one CSV row per frame, see drawRepeat.cpp. Structures are hashed with their padding bytes masked out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace Common::DrawRepeat {

[[nodiscard]] bool Enabled() noexcept;

enum Kind : uint32_t { Draw = 0, Dispatch = 1 };

// Components of a draw's signature (each hashed on its own, so a difference can be attributed).
enum Component : unsigned {
	CtxAll = 0,   // all context registers
	CtxRt,        // the render targets and the depth target (addresses move when the swap chain rotates)
	ShVsBase,     // code addresses of the ES/LS/HS/GS stages
	ShPsRegs,     // the pixel stage registers (code address, resource words) without pointers
	ShRsrcVs,     // HS/GS resource words
	ShSgprMeta,   // user SGPR counts and types (no values)
	ShUser,       // user-data address words and user SGPR values (root-table pointers, immediates)
	DrawArgs,     // the draw arguments (counts, index type)
	Ucfg,         // user config registers (primitive type, index offsets, GE control, ...)
	ReadsValues,  // the dwords the scalar-read walk read (values only)
	ReadsInputs,  // the walk's inputs (user data dwords, shader base, workgroup counts)
	ComponentCount
};
static constexpr const char* kComponentNames[ComponentCount] = {
    "ctx_all", "ctx_rt", "sh_vs_base", "sh_ps_regs", "sh_rsrc_vs", "sh_sgpr_meta", "sh_user", "draw_args", "ucfg",
    "reads_values", "reads_inputs"};

struct Inputs {
	Kind     kind    = Draw;
	uint64_t program = 0; // identity of the shaders (code addresses)
	uint64_t h[ComponentCount] {};
};

[[nodiscard]] uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0) noexcept;
// Bytes of T that no member initializer writes (padding): found once by default-initialising T over a
// buffer of zeros and over a buffer of ones (clang has no __builtin_clear_padding).
template <class T>
[[nodiscard]] const std::array<unsigned char, sizeof(T)>& PaddingMask() {
	static const auto mask = [] {
		alignas(T) unsigned char zeros[sizeof(T)];
		alignas(T) unsigned char ones[sizeof(T)];
		std::memset(zeros, 0x00, sizeof(T));
		std::memset(ones, 0xff, sizeof(T));
		new (zeros) T;
		new (ones) T;
		std::array<unsigned char, sizeof(T)> m {};
		for (size_t i = 0; i < sizeof(T); i++) {
			m[i] = zeros[i] == ones[i] ? 0xffu : 0x00u;
		}
		return m;
	}();
	return mask;
}

// Hash of an object with its padding bytes masked out (so stale padding never decides an equality).
template <class T>
[[nodiscard]] uint64_t HashObject(const T& value, uint64_t seed = 0) noexcept {
	const auto&            mask = PaddingMask<T>();
	alignas(T) unsigned char buffer[sizeof(T)];
	std::memcpy(buffer, &value, sizeof(T));
	for (size_t i = 0; i < sizeof(T); i++) {
		buffer[i] &= mask[i];
	}
	return HashBytes(buffer, sizeof(T), seed);
}

[[nodiscard]] inline uint64_t Mix(uint64_t h, uint64_t v) noexcept {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// Lookahead thread. `reset` is a SuspendPoint item (frame boundary), otherwise a submission.
void BeginItem(bool reset) noexcept;
// A plan was stored for the draw being prepared: fold its reads into the draw's signature.
void NoteReads(uint64_t values_hash, uint64_t inputs_hash) noexcept;
// The draw (or dispatch) has been looked at. Takes the reads noted since the previous call.
void NoteDraw(const Inputs& inputs) noexcept;

} // namespace Common::DrawRepeat

#endif
