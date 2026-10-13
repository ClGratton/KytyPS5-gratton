#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_

// The renderer side of --debug-skip-log's dependency check (see skipDependencies.h): one global
// tracker, the CSV rows it writes, and the hooks the draw / dispatch / DMA / readback paths call.
// Every hook is a no-op unless the skip log is on AND at least one skipped draw is still waiting for
// its pipeline, so the cost outside a skip-mode debug run is one relaxed atomic load.

#include "common/benchInstrument.h"
#include "graphics/host_gpu/renderer/skipDependencies.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::SkipDeps {

[[nodiscard]] Tracker& Global();

// True while some skipped draw's outputs are still poison.
[[nodiscard]] inline bool Poisoned() {
	return Bench::SkipLogEnabled() && Global().Active();
}

// Who is reading, for the SRT-walk hook that cannot see the shader: set around a materialisation.
class ReaderScope {
public:
	ReaderScope(uint64_t shader_hash, const char* op);
	~ReaderScope();
	ReaderScope(const ReaderScope&)            = delete;
	ReaderScope& operator=(const ReaderScope&) = delete;

private:
	uint64_t    m_prev_hash;
	const char* m_prev_op;
};

// A graphics draw was dropped. Registers the poison and writes the `event` row ("skip_program" /
// "skip_pipeline"). `stores` describes the shaders' non-render-target writes ("none", "atomics",
// "unknown": the program was not ready, so its stores are not known).
void NoteSkippedDraw(const char* event, uint64_t vs, uint64_t ps, const char* draw_name,
                     std::span<const Range> writes, const char* stores, const std::string& targets);

// A draw of this shader pair executed: its pipeline is ready. Writes a `ready` row per resolved root.
void NoteDrawReady(uint64_t vs, uint64_t ps);

// A pass reads `range`. Writes a `reader` row for every first hit. Returns the hits (for NoteTaint).
bool NoteRead(const Range& range, ReadKind kind, uint64_t shader_hash, const char* op,
              std::vector<Hit>& hits);

// Hits found while preparing a draw that CommitBindings turns into taint (thread-local, GPU thread).
[[nodiscard]] std::vector<Hit>& PendingHits();

// A pass whose reads or writes the tracker cannot see (shader-computed addresses). `reads_poison` says
// whether it also read tracked poison. Writes one `untracked` row per (shader, reason).
void NoteUntracked(uint64_t shader_hash, const char* op, const char* reason, bool reads_poison);

// SRT walk reads (all threads); uses the ReaderScope for attribution.
void NoteScalarRead(uint64_t address, uint64_t bytes);

// The materialisation of `shader_hash` produced something that is not a descriptor.
void NoteBadDescriptor(uint64_t shader_hash, const char* what, uint32_t slot,
                       const uint32_t* words, uint32_t word_count);

// Writes the rows of still-waiting roots (periodically from NoteSkippedDraw, and at exit).
void DumpOpenRoots(const char* why);

[[nodiscard]] std::string FormatRange(const Range& range);

} // namespace Libs::Graphics::SkipDeps

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_ */
