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
// Cost when off: one thread_local load per scope. When on: two TSC reads per scope; a run with it
// is for reading shares, never an fps baseline (bench/CONTROLS.md, timing-log rule).
//
// Idea from BryanKAdams/BryKytyPS5 (drain stats, draw-phase and PM4 opcode timers; GPL-2.0),
// re-implemented around an exclusive-time stack.

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
// Final report for the time since the last one (on the attached thread, at shutdown).
void Finish();

// Counters, always on (one relaxed atomic add): used for the "submits per frame" figure.
void NoteSubmit() noexcept;
void NoteFrame() noexcept;
[[nodiscard]] uint64_t Submits() noexcept;
[[nodiscard]] uint64_t Frames() noexcept;

// --submit-coalesce bookkeeping (always on, one relaxed add): events whose submit was held back,
// and what finally submitted them.
enum class Coalesce : uint8_t { Deferred, FlushBatch, FlushDeadline, FlushForced, Count };
void NoteCoalesce(Coalesce kind, uint64_t count = 1) noexcept;

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
	~Scope() {
		if (m_state != nullptr) {
			detail::Exit(m_state, m_phase, m_start);
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
