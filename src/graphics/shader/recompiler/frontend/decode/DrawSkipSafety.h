#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DRAWSKIPSAFETY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DRAWSKIPSAFETY_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

// --async-pipelines skip drops the draws whose pipeline is not ready yet. That only loses pixels
// of one frame when the shaders write nothing but their outputs. A shader that stores to a buffer
// or a storage image, does an image atomic, or touches GDS changes memory later frames read
// (counters, linked lists, history buffers), so skipping its draw corrupts frames after the
// current one: such draws always wait. Buffer atomics alone may be skipped, like Bryan's rule.
// Rule and idea: BryanKAdams/KytyPS5@17da0ad52 and @5949de94d (GPL-2.0), re-implemented here on
// the decoded instruction list; the decision needs no translation.
[[nodiscard]] bool InstructionWritesPersistentMemory(const Instruction& inst);
[[nodiscard]] bool ProgramMayBeSkipped(const Program& program);
// Decodes `code` the way the translator does: with `back_code` it is the front half of a merged
// stage, a lone `front_only` (LS) stage ends in the hand-off to the next stage.
[[nodiscard]] bool CodeMayBeSkipped(std::span<const uint32_t> code,
                                    std::span<const uint32_t> back_code = {},
                                    bool front_only = false);

// The verdict per shader, kept by (hash, size): a skipped draw asks again on every frame until its
// pipeline is ready.
class DrawSkipSafetyCache {
public:
	// `newly_unsafe` is set when this call decided that the shader's draws must wait.
	[[nodiscard]] bool MayBeSkipped(std::span<const uint32_t> code, uint64_t hash,
	                                std::span<const uint32_t> back_code = {},
	                                bool front_only = false, bool* newly_unsafe = nullptr);

private:
	std::mutex                         m_mutex;
	std::unordered_map<uint64_t, bool> m_verdicts;
};

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_DRAWSKIPSAFETY_H_ */
