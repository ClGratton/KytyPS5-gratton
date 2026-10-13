#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_

// The renderer side of --debug-skip-log's dependency check (see skipDependencies.h): one global
// tracker, the CSV rows it writes, and the hooks the draw / dispatch / DMA / readback paths call.
// Every hook is a no-op unless the skip log is on AND at least one skipped draw is still waiting for
// its pipeline, so the cost outside a skip-mode debug run is one relaxed atomic load.

#include "common/benchInstrument.h"
#include "graphics/host_gpu/renderer/skipDependencies.h"

#include <atomic>
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

// Lookahead / GPU-thread progress (--pipeline-lookahead), for the bad-descriptor rows: the lookahead
// item (submission sequence) being scanned, the highest sequence the GPU thread has started, and how
// many draws the lookahead scanned after the GPU thread had started (or passed) their submission.
struct LookaheadProgress {
	std::atomic<uint64_t> item_seq {0};
	std::atomic<uint64_t> gpu_started {0};
	std::atomic<uint64_t> draws {0};
	std::atomic<uint64_t> draws_gpu_started {0}; // the GPU thread had started this draw's submission
	std::atomic<uint64_t> draws_gpu_passed {0};  // ... and has started a later one
	std::atomic<uint64_t> items {0};
	std::atomic<uint64_t> items_gpu_started_at_start {0};
};
[[nodiscard]] LookaheadProgress& Progress();
inline void NoteLookaheadItem(uint64_t seq, bool started_at_start) {
	if (!Bench::SkipLogEnabled()) return;
	auto& p = Progress();
	p.item_seq.store(seq, std::memory_order_relaxed);
	p.items.fetch_add(1, std::memory_order_relaxed);
	if (started_at_start) p.items_gpu_started_at_start.fetch_add(1, std::memory_order_relaxed);
}
inline void NoteGpuStarted(uint64_t seq) {
	if (!Bench::SkipLogEnabled()) return;
	Progress().gpu_started.store(seq, std::memory_order_relaxed);
}
inline void NoteLookaheadDraw(uint64_t item_seq) {
	if (!Bench::SkipLogEnabled()) return;
	auto&      p       = Progress();
	const auto started = p.gpu_started.load(std::memory_order_relaxed);
	p.draws.fetch_add(1, std::memory_order_relaxed);
	if (item_seq != 0 && item_seq <= started) p.draws_gpu_started.fetch_add(1, std::memory_order_relaxed);
	if (item_seq != 0 && item_seq < started) p.draws_gpu_passed.fetch_add(1, std::memory_order_relaxed);
}

// Writes the rows of still-waiting roots (periodically from NoteSkippedDraw, and at exit).
void DumpOpenRoots(const char* why);

[[nodiscard]] std::string FormatRange(const Range& range);

} // namespace Libs::Graphics::SkipDeps

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_ */
