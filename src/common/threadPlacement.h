#ifndef KYTY_COMMON_THREADPLACEMENT_H_
#define KYTY_COMMON_THREADPLACEMENT_H_

// --thread-placement: give the serial threads a physical core of their own (see common/cpuTopology.h
// for the plan and why).
//
// The emulator's critical threads call SetCurrentThreadRole when they start. While the flag is on, a
// small background thread (kyPlacement) keeps every thread of the process on the CPUs its role gets:
//   Gpu        kyGpu: all logical CPUs of one physical core
//   Sequencer  the look-ahead sequencer: all logical CPUs of a second physical core (machines with
//              eight or more physical cores)
//   Other      everything else (guest threads, look-ahead and compile workers, the driver's and the
//              window system's threads, this process's main thread): the remaining CPUs
// The scan repeats a few times a second because threads come and go, and a thread inherits the
// affinity of the thread that created it (the driver starts helper threads from inside kyGpu).
// A thread whose affinity somebody else set (not the start-up mask, not one of ours) is left alone.
//
// Runtime toggle: turning the flag off puts every thread we moved back to the mask it had. Changing
// affinity at any time is safe (the kernel moves the thread at its next scheduling point; nothing in
// the emulator depends on where a thread runs).
//
// Linux: sched_setaffinity per thread (/proc/self/task lists the threads). Windows: group affinity per
// thread (Toolhelp lists the threads); written but not tested. Other hosts: the flag does nothing and
// says so.

#include <cstdint>

namespace Common::ThreadPlacement {

enum class Role : uint8_t {
	Other = 0,
	Gpu,
	Sequencer,
};

// After Config::Load. `toggleable`: the runtime toggle control is on, so the flag may be turned on
// later. Starts the background thread only when the flag is on or may become on.
void Start(bool toggleable);
void Stop();

// From the thread itself, when it starts. Cheap; a no-op for the placement while the flag is off.
void SetCurrentThreadRole(Role role);

// Run a scan now (a runtime toggle was applied).
void Refresh();

} // namespace Common::ThreadPlacement

#endif /* KYTY_COMMON_THREADPLACEMENT_H_ */
