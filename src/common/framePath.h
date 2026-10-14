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
	// emulator locks and waits of any thread. `a` is the name of the HLE call the thread was serving
	// (a pointer to a string that lives for the whole run, 0 if none), `b` = object address (47 bits),
	// bit 47 = timed wait, bits 48..63 = microseconds between the start of that HLE call and this wait.
	MutexWait = 2,   // contended Common::Mutex::Lock
	CondWait = 3,    // Common::CondVar::Wait / WaitFor
	Sleep = 4,       // Common::Thread::SleepMicro/SleepNano; b = requested ns
	// guest -> GPU thread
	Submit = 10,     // GuestGpu::Enqueue, dur = lock + lookahead copy; a = HLE name, b = Seq()
	GpuSlice = 11,   // one GuestGpu::Process() call on kyGpu; a = sequence, b = complete | type << 8 | queue << 16
	VkSubmit = 12,   // vkQueueSubmit incl. queue mutex; a = timeline tick
	GpuWait = 13,    // blocked in vkWaitSemaphores (any thread); a = tick waited for
	GpuExec = 14,    // t = GPU begin of a command buffer (ns, CLOCK_MONOTONIC), dur = GPU execution, a = tick
	PrioWait = 15,   // kyPrioOps waited for the tick of its next operation; a = tick
	FlipSlotWait = 16, // kyGpu waited for a free flip-queue slot
	PrioRun = 17,    // kyPrioOps ran the operation of tick a (flip complete, interrupt, timestamp, ...)
	Lookahead = 18,  // kyty-lookahead processed one submission; a = sequence, b = late | kind << 8 | queue depth << 16 | dwords << 32
	Phase = 19,      // kyGpu phase span >= 30 us (or any drain / idle); a = GpuPhase::Phase, b = drain cause | full << 8
	// presentation
	FlipReserve = 20,  // a = request id, b = source (0 gpu eop, 1 cpu)
	FlipReady = 21,    // a = request id (FlipQueue::Complete)
	FlipPresent = 22,  // dur = Present(); a = request id of the owner, b = due
	Vblank = 23,       // t = VblankBegin, dur until VblankEnd; a = vblank number, b = presented
	Interrupt = 24,    // RenderContext::TriggerInterrupt; a = event id
	FlipEvent = 25,    // VideoOut flip equeue trigger; a = flip arg
	GuestFlipCmd = 26, // kyGpu parsed a flip packet (SubmitFlipFromGpu); a = request id
	SuspendWait = 27,  // guest thread blocked at GuestGpu::SuspendPoint; a = HLE name
	CmdSyncWait = 28,  // guest thread blocked in SendCommandSync; a = HLE name
	WaitIdle = 29,     // guest thread blocked in GuestGpu::WaitForIdle; a = HLE name
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

// Nanoseconds per rdtsc tick (calibrated once in Start()); 0 when logging is off.
[[nodiscard]] double NsPerTsc() noexcept;

// The HLE call the current thread is serving, for MutexWait / CondWait / Sleep / Submit events:
// the name pointer (0 if none) and `b` bits 48..63 (age of the call in microseconds, saturated).
struct Blame {
	uint64_t name;
	uint64_t age_bits;
};
[[nodiscard]] Blame CurrentBlame(uint64_t now_ns) noexcept;

class Scope {
public:
	Scope(Kind kind, uint64_t a = 0, uint64_t b = 0) noexcept
	    : m_on(On()), m_kind(kind), m_a(a), m_b(b), m_t(m_on ? NowNs() : 0) {}
	~Scope() {
		if (m_on) {
			Log(m_kind, m_t, NowNs() - m_t, m_a, m_b);
		}
	}
	void SetA(uint64_t a) noexcept { m_a = a; }
	void SetB(uint64_t b) noexcept { m_b = b; }
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool     m_on;
	Kind     m_kind;
	uint64_t m_a, m_b, m_t;
};

// A blocking call of the current thread that is blamed on the HLE call it serves (see Kind::CondWait).
// Logged only when it lasted at least `min_ns`.
class BlameScope {
public:
	BlameScope(Kind kind, uint64_t object_bits, uint64_t min_ns = 0) noexcept
	    : m_on(On()), m_kind(kind), m_bits(object_bits), m_min(min_ns), m_t(m_on ? NowNs() : 0) {}
	~BlameScope() {
		if (m_on) {
			const uint64_t now = NowNs();
			if (now - m_t >= m_min) {
				const Blame blame = CurrentBlame(m_t);
				Log(m_kind, m_t, now - m_t, blame.name, m_bits | blame.age_bits);
			}
		}
	}
	BlameScope(const BlameScope&)            = delete;
	BlameScope& operator=(const BlameScope&) = delete;

private:
	bool     m_on;
	Kind     m_kind;
	uint64_t m_bits, m_min, m_t;
};

} // namespace Common::FramePath

#endif
