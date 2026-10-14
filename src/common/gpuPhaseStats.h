#ifndef KYTY_COMMON_GPU_PHASE_STATS_H_
#define KYTY_COMMON_GPU_PHASE_STATS_H_

// --gpu-phase-stats <seconds>: where the GPU thread (kyGpu) spends its time (K52).
//
// Observation only, off by default. When on, scopes in the submission loop, the PM4 loop, the draw
// path and the command scheduler charge their exclusive time to a phase (pure accounting in
// common/phaseTimers.h), and every <seconds> the GPU thread prints one line with the percentage of
// wall time per phase, the frames presented and Vulkan submits in the window (submits per frame),
// the drains (waits for the GPU) by cause and the PM4 packets that spent the most time. The same
// numbers go to _gpu_phase.csv in the working directory.
//
// With --profile each scope is also a Tracy zone named gpu:<phase> (tracy-csvexport -e -f gpu:
// gives the exclusive time per phase of this thread alone).
// Cost when off: one thread_local load per scope. When on: two TSC reads per scope; a run with it
// is for reading shares, never an fps baseline (bench/CONTROLS.md, timing-log rule).
//
// Idea from BryanKAdams/BryKytyPS5 (drain stats, draw-phase and PM4 opcode timers; GPL-2.0),
// re-implemented around an exclusive-time stack.

#include "common/drainCauses.h"
#include "common/indirectDrawPolicy.h"

#include <cstdint>

namespace Common::GpuPhase {

enum class Phase : uint8_t {
	Idle,        // the GPU thread waits for a submission
	Pm4,         // PM4 parsing and packet handlers, minus the phases below
	Draw,        // draw/dispatch recording, minus the phases below (vkCmd* calls, state, barriers)
	Pipeline,    // program and pipeline lookup and creation on the GPU thread
	PipelineJob, // waiting for an asynchronous program/pipeline job
	Srt,         // SRT walk and binding preparation
	Descriptors, // buffer/image rebinding and descriptor writes
	VkCmd,       // vkCmd* recording (draws, dispatches, binds, barriers, copies, dynamic state)
	VkAlloc,     // vkCreateImage/vkDestroyImage and memory allocation through VMA
	Submit,      // closing the command buffer and vkQueueSubmit
	DrainTick,   // CommandScheduler::Wait for a tick
	DrainFlush,  // FlushAndWait
	DrainFinish, // Finish
	Gc,          // texture/buffer garbage collection and fault processing
	// Sub-phases of the pipeline lookup (carved out of Pipeline, so Pipeline keeps the remainder).
	PipePrepare,     // PrepareProgram: shader map hit, params, static vertex/pixel input info
	PipeProgramKey,  // ProgramCache::Get: static-state key and the source-entry lookup
	PipeMaterialize, // ProgramCache::Get: SRT materialization and the permutation search
	PipeKeyBuild,    // BuildGraphicsKey
	PipeFind,        // key hash and the pipeline map lookup
	PipeEntryLock,   // waiting for a program source entry's lock (cheap unless a worker holds it)
	PipeSrtRead,     // guest-memory reads made by the SRT walk (inside pipe_materialize)
	PipePermSearch,  // scanning a program's permutations for the one that fits the specialization
	// Sub-phases of the descriptor work. `Descriptors` itself is no longer scoped: the three
	// functions (RebindBuffers, RebindImages, CommitBindings) each have a remainder phase, and the
	// costly calls inside them their own. The old "descriptors" share is the sum of all Desc* phases
	// (the report prints it as `descriptors_total`).
	DescBuffers,     // RebindBuffers: its own code (buffer views, memory offsets, packing)
	DescUpload,      // RebindBuffers: stream-buffer uploads of the flattened SRT and shader data
	DescImages,      // RebindImages: its own code (rebind decision, view clears, mip loop)
	DescResolve,     // RebindImages: ResolveTexture + BindImage (texture-cache image lookup)
	DescFindTexture, // RebindImages: TextureCache::FindTexture (image view lookup/creation)
	DescCommit,      // CommitBindings: its own code (binding counts, push-data copy)
	DescHazard,      // CommitBindings: scalar-read vs shader/attachment write overlap checks
	DescWrites,      // CommitBindings: per-stage descriptor write and image-info building
	DescTransit,     // CommitBindings: Image::Transit (layout transitions, barriers excluded: VkCmd)
	DescPush,        // CommitBindings: push constants and the push-descriptor / descriptor-set update
	SrtPlanShadow,   // --srt-plan-shadow: lookup, validity check and comparison of a plan on kyGpu
	SrtPlanAdopt,    // --srt-plans: lookup and validity check of a plan on kyGpu (inside pipe_materialize)
	Count
};

// Seconds between reports; 0 leaves everything off. `phases` false is --submit-stats: only the
// frame/submit/coalescing counters are reported, no scope is timed. Called from Config::Load().
void Configure(uint32_t interval_seconds, bool phases = true);

[[nodiscard]] bool Enabled() noexcept;

// The calling thread becomes the one that is measured (kyGpu). No-op when disabled.
void AttachGpuThread();
// Prints a report when the interval has elapsed (call between scopes, on the attached thread).
void Poll();
// Ends the current report window at the next Poll() whatever the interval (a runtime toggle was
// just applied: every window then lies inside one toggle state). Any thread, relaxed.
void RequestCut() noexcept;
// Final report for the time since the last one (on the attached thread, at shutdown).
void Finish();

// Counters, always on (one relaxed atomic add): used for the "submits per frame" figure.
void NoteSubmit() noexcept;
// Guest global barrier events: recorded, or left out by --elide-barriers.
void NoteBarrier(bool elided) noexcept;
// --known-fills: color-metadata clear discovery outcomes (see TextureCache::MaterializeColorClear).
enum class KnownFill : uint8_t { Hit, Readback, Discarded, DiscardRefused, Count };
void NoteKnownFill(KnownFill kind) noexcept;
// --gpu-indirect-draws: how `draws` indirect draws of one packet were routed. The first time each
// fallback reason is seen it is printed once.
void NoteIndirectDraw(IndirectDraw::Route route, uint32_t draws) noexcept;
// --clean-guest-reads: a read served from the guest backing (clean) or by a plain load (direct).
void NoteCleanRead(bool clean) noexcept;
// --readback-queue: one second-queue readback; `flushed` the producer was still in the open
// command buffer, `lag_ticks` how many submitted ticks lie between the producer and now.
void NoteQueueReadback(bool flushed, uint64_t lag_ticks) noexcept;
// --eager-readback: eager write-backs of hot pages (the report prints them as "eager readback (total)").
enum class Eager : uint8_t { Scheduled, Retired, SkippedNoSpace, VerifyChecks, VerifyMismatches, Count };
void NoteEager(Eager kind) noexcept;
void NoteFrame() noexcept;
[[nodiscard]] uint64_t Submits() noexcept;
[[nodiscard]] uint64_t Frames() noexcept;

// --submit-coalesce bookkeeping (always on, one relaxed add): events whose submit was held back,
// and what finally submitted them.
enum class Coalesce : uint8_t { Deferred, FlushBatch, FlushDeadline, FlushForced, Count };
void NoteCoalesce(Coalesce kind, uint64_t count = 1) noexcept;

// Why the next CommandScheduler::Wait happens (the innermost tagged call site wins). The report
// lists drains per cause (count, full ones, time). Free when disabled: a thread_local store.
using DrainCause = DrainCauses::Cause;

namespace detail {
extern thread_local DrainCause t_drain_cause;
extern thread_local bool       t_drain_full;
} // namespace detail

class DrainCauseScope {
public:
	explicit DrainCauseScope(DrainCause cause) noexcept: m_previous(detail::t_drain_cause) {
		detail::t_drain_cause = cause;
	}
	~DrainCauseScope() { detail::t_drain_cause = m_previous; }
	DrainCauseScope(const DrainCauseScope&)            = delete;
	DrainCauseScope& operator=(const DrainCauseScope&) = delete;

private:
	DrainCause m_previous;
};

// A synchronous buffer readback of `bytes` starting at guest address `address` is about to wait:
// the report lists the regions that cost the most drain time (hot regions). GPU thread only.
void NoteReadback(uint64_t address, uint64_t bytes) noexcept;

// The GPU thread took a page fault on guest memory (it read bytes that share a page with
// GPU-written data). Records the call stack (unique stacks, with hit counts, are listed in the
// report as raw offsets for addr2line). GPU thread only; a no-op unless stats are enabled.
void NoteGpuThreadFault() noexcept;

// Set by CommandScheduler::Wait before its DrainTick scope: the wait forced a submit.
inline void NoteDrainFull(bool full) noexcept {
	detail::t_drain_full = full;
}

namespace detail {
struct ThreadState;
extern thread_local ThreadState* t_state;
void     Enter(ThreadState* state, Phase phase) noexcept;
void     Exit(ThreadState* state, Phase phase, uint64_t start) noexcept;
void     PacketEnd(ThreadState* state, uint32_t opcode, uint64_t start) noexcept;
uint64_t Now() noexcept;
void     SetPacket(ThreadState* state, uint32_t opcode) noexcept;
} // namespace detail

// Charges the exclusive time of its lifetime to `phase` on the attached thread; free elsewhere.
class Scope {
public:
	explicit Scope(Phase phase) noexcept: m_state(detail::t_state), m_phase(phase) {
		if (m_state != nullptr) {
			m_start = detail::Now();
			detail::Enter(m_state, phase);
		}
	}
	~Scope() { End(); }
	// Closes the scope before the end of the block (the destructor then does nothing).
	void End() noexcept {
		if (m_state != nullptr) {
			detail::Exit(m_state, m_phase, m_start);
			m_state = nullptr;
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	detail::ThreadState* m_state;
	Phase                m_phase;
	uint64_t             m_start = 0;
};

// Inclusive time of one PM4 packet handler, by opcode, and the opcode that is current while a
// drain happens (so drains can be attributed to the packet that caused them).
class PacketScope {
public:
	explicit PacketScope(uint32_t opcode) noexcept: m_state(detail::t_state), m_opcode(opcode) {
		if (m_state != nullptr) {
			m_start = detail::Now();
			detail::SetPacket(m_state, opcode);
		}
	}
	~PacketScope() {
		if (m_state != nullptr) {
			detail::PacketEnd(m_state, m_opcode, m_start);
		}
	}
	PacketScope(const PacketScope&)            = delete;
	PacketScope& operator=(const PacketScope&) = delete;

private:
	detail::ThreadState* m_state;
	uint32_t             m_opcode;
	uint64_t             m_start = 0;
};

} // namespace Common::GpuPhase

#endif /* KYTY_COMMON_GPU_PHASE_STATS_H_ */
