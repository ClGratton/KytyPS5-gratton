#ifndef KYTY_COMMON_FRAME_PATH_H_
#define KYTY_COMMON_FRAME_PATH_H_

// Frame critical-path timeline (KYTY_FRAME_PATH=<file>, debug only, nothing on by default).
//
// Question it answers: what bounds the level frame? Every thread owns a ring of fixed-size events
// {start, duration, kind, a, b} stamped with CLOCK_MONOTONIC (the clock of --bench-frames-csv and
// --bench-render-csv). Sources: the HLE thunks (guest calls that block or submit), contended
// Common::Mutex locks and Common::CondVar waits, the guest-to-GPU submit queue, the GPU thread's
// slices, vkQueueSubmit, timeline-semaphore waits, GPU execution of each command buffer (timestamp
// queries), the flip queue, the vblank thread and the interrupt events that wake the guest.
//
// Dump: when <file>.go appears, logging pauses, the last KYTY_FRAME_PATH_SECONDS (default 8) of
// every ring are written to <file>.<n>.kfp (a one-line JSON header, then the raw events), the .go
// file is removed and logging resumes. bench/frame_path_report.py reads the files.
//
// Cost with the variable unset: one relaxed atomic load per instrumented site.

#include <atomic>
#include <cstdint>

namespace Common::FramePath {

enum Kind : uint16_t {
	// guest side
	Hle = 1,         // a = HLE record (name), b = result (rax); dur = time inside the call
	// emulator locks and waits
	MutexWait = 2,   // a = caller return address, b = mutex address; contended Common::Mutex::Lock
	CondWait = 3,    // a = caller return address, b = condvar address; Common::CondVar::Wait/WaitFor
	// guest -> GPU thread
	Submit = 10,     // a = sequence, b = type | queue << 8 | pending << 16 | dwords << 32
	GpuSlice = 11,   // a = sequence, b = complete | draws << 8 | dispatches << 32; one Process() call
	VkSubmit = 12,   // a = timeline tick; dur = vkQueueSubmit incl. queue mutex
	GpuWait = 13,    // a = timeline tick waited for; dur = blocked in vkWaitSemaphores (any thread)
	GpuExec = 14,    // t = GPU begin (ns), dur = GPU execution, a = tick; from timestamp queries
	PrioOp = 15,     // a = tick the operation waited for, b = 1 flip complete, 2 interrupt, 0 other
	FlipSlotWait = 16, // kyGpu waited for a free flip-queue slot
	// presentation
	FlipReserve = 20,  // a = request id, b = source (0 gpu eop, 1 cpu)
	FlipReady = 21,    // a = request id (FlipQueue::Complete)
	FlipPresent = 22,  // dur = Present(), a = request id of the owner, b = due
	Vblank = 23,       // t = VblankBegin, dur = until VblankEnd, a = vblank count, b = presented
	Interrupt = 24,    // a = event id; guest interrupt equeue event triggered
	FlipEvent = 25,    // a = flip arg; VideoOut flip equeue event triggered
	GuestFlipCmd = 26, // kyGpu parsed a flip packet; a = submit id
	// mutex/cond wait of a named site
	Mark = 30,         // a = site id, b = value
};

namespace detail {
extern std::atomic<bool> g_on;
} // namespace detail

[[nodiscard]] inline bool On() noexcept {
	return detail::g_on.load(std::memory_order_relaxed);
}

[[nodiscard]] uint64_t NowNs() noexcept;

// Called once from Config::Load(): reads KYTY_FRAME_PATH and starts the dump thread.
void Start();
// True when KYTY_FRAME_PATH is set (also before Start()).
[[nodiscard]] bool Requested() noexcept;

void Log(Kind kind, uint64_t t_ns, uint64_t dur_ns, uint64_t a = 0, uint64_t b = 0) noexcept;

// Names for the HLE record pointers in `Hle` events (the dump writes a table).
void RegisterName(const void* key, const char* name) noexcept;

class Scope {
public:
	Scope(Kind kind, uint64_t a = 0, uint64_t b = 0) noexcept
	    : m_on(On()), m_kind(kind), m_a(a), m_b(b), m_t(m_on ? NowNs() : 0) {}
	~Scope() {
		if (m_on) {
			Log(m_kind, m_t, NowNs() - m_t, m_a, m_b);
		}
	}
	void SetB(uint64_t b) noexcept { m_b = b; }
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool     m_on;
	Kind     m_kind;
	uint64_t m_a, m_b, m_t;
};

} // namespace Common::FramePath

#endif
