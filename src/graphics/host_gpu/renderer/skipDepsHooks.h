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

namespace detail {
inline std::atomic<bool> g_guard_enabled {false};
} // namespace detail

// --skip-guard is on (the tracker learns and the pipeline cache asks before it skips a draw).
[[nodiscard]] inline bool GuardEnabled() {
	return detail::g_guard_enabled.load(std::memory_order_relaxed);
}

// The tracker is fed: --debug-skip-log writes its rows, --skip-guard acts on what it learns.
[[nodiscard]] inline bool Enabled() {
	return Bench::SkipLogEnabled() || GuardEnabled();
}

// True while some skipped draw's outputs are still poison.
[[nodiscard]] inline bool Poisoned() {
	return Enabled() && Global().Active();
}

// ---- --skip-guard ---------------------------------------------------------------------------
// Called once by the pipeline cache when --async-pipelines skip is the mode and --skip-guard is on.
void ConfigureGuard(bool enabled, bool bda);

// What the draw in flight on the GPU thread would write, asked lazily at the moment the pipeline
// cache is about to skip it (a miss; the common case never evaluates it). `collect` appends the
// guest ranges (render targets, depth/stencil, their metadata). The answer is cached for the query.
class GuardQuery {
public:
	// `collect` is a callable taking std::vector<Range>&; it must outlive the query (no allocation:
	// one of these is built per draw).
	template <class F>
	explicit GuardQuery(F& collect)
	    : m_context(&collect),
	      m_fn([](void* context, std::vector<Range>& writes) { (*static_cast<F*>(context))(writes); }) {}
	[[nodiscard]] bool MustWait();

private:
	void* m_context;
	void (*m_fn)(void*, std::vector<Range>&);
	int m_state = -1;
};

// Installs `query` for the calls that follow on this thread (GPU thread, one draw at a time).
class GuardScope {
public:
	explicit GuardScope(GuardQuery* query);
	~GuardScope();
	GuardScope(const GuardScope&)            = delete;
	GuardScope& operator=(const GuardScope&) = delete;

private:
	GuardQuery* m_prev;
};

// The pipeline cache calls this when it would skip a draw: true = the learned rule says the draw
// writes memory a hard-class reader has read from a skipped draw before, so it waits (like stall
// mode). Without an installed query the ranges are unknown and the answer is also true.
[[nodiscard]] bool GuardVetoesSkip();

// A draw waited for its program / pipeline because the guard vetoed the skip.
void NoteGuardedWait(bool pipeline_stage);
// A draw waited for its program / pipeline for another reason (store-safety rule, ...), skip mode.
void NoteOtherWait();

// A shader with 64-bit address loads (or computed stores) read poison through bound resources:
// what it dereferences is invisible, so the roots it read become unsafe (guard only).
void NoteBdaSuspect(uint64_t shader_hash, const char* op, const char* reason,
                    std::span<const Hit> hits);

// One summary line in the emulator log (periodic, and at exit).
void GuardSummary(const char* why);

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

// SRT walk reads (all threads); uses the ReaderScope for attribution. Called for every scalar read of
// every walk, so the disabled case is one inline atomic load.
void NoteScalarReadSlow(uint64_t address, uint64_t bytes);
inline void NoteScalarRead(uint64_t address, uint64_t bytes) {
	if (Enabled()) NoteScalarReadSlow(address, bytes);
}

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
	std::atomic<int64_t>  item_start_ns {0};
	// Prefetch calls that kept the lookahead thread busy for 20 ms or more (compile waits, translation).
	std::atomic<uint64_t> slow_calls {0};
	std::atomic<int64_t>  slow_total_ns {0};
	std::atomic<int64_t>  last_slow_ns {0};
	std::atomic<int64_t>  last_slow_end_ns {0};
};
[[nodiscard]] LookaheadProgress& Progress();
void NoteLookaheadPeriodic();
inline void NoteLookaheadItem(uint64_t seq, bool started_at_start) {
	if (!Bench::SkipLogEnabled()) return;
	auto& p = Progress();
	p.item_seq.store(seq, std::memory_order_relaxed);
	p.item_start_ns.store(Bench::NowNs(), std::memory_order_relaxed);
	NoteLookaheadPeriodic();
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

// One prefetch call (draw or dispatch) took `duration_ns`; calls of 20 ms or more get a `slow_prefetch`
// row with the lookahead's lag behind the GPU thread when it returned.
void NoteLookaheadCall(const char* what, int64_t start_ns, int64_t duration_ns);

// Writes the rows of still-waiting roots (periodically from NoteSkippedDraw, and at exit).
void DumpOpenRoots(const char* why);
void DumpLookahead(const char* why);

[[nodiscard]] std::string FormatRange(const Range& range);

} // namespace Libs::Graphics::SkipDeps

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPSHOOKS_H_ */
