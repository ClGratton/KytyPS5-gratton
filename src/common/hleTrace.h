#ifndef KYTY_COMMON_HLE_TRACE_H_
#define KYTY_COMMON_HLE_TRACE_H_

// --debug-hle-trace: a per-thread ring buffer of the last HLE calls the guest made, dumped by the
// fatal fault report (and on demand).
//
// Why: a guest crash is usually the end of a chain that started earlier, in the guest reacting to
// an HLE call that returned an error or less than it asked for (a short read, a failed open, a
// refused mapping). The fault report only shows the faulting instruction, not what the thread was
// doing in the seconds before it.
//
// How: with the flag on, every HLE function (SymbolType::Func) the guest can import is bound to a
// 24-byte thunk instead of the function itself. The thunk loads a record pointer into r11 and
// jumps to a shared trampoline, which saves the argument registers (including xmm0-7), appends
// {time, function name, first four integer arguments, caller} to the calling thread's ring, restores
// everything and tail-jumps to the real function. The real function sees the original stack and
// registers, so tail calls, stack arguments, varargs and functions that never return are
// unaffected. HLE functions can additionally attach a short note (a path) and their result to the
// entry they are serving with NoteString / NoteResult.
//
// Cost: off = one predictable branch per NoteString / NoteResult site and nothing else (the
// functions are bound directly). On = about 40 ns per HLE call. Linux x86-64 hosts only; the flag
// is accepted and ignored elsewhere.

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Common::HleTrace {

// Called before the HLE symbols are registered.
void Enable(bool enabled) noexcept;

namespace detail {
extern bool g_enabled; // written once before any guest thread exists
void        NoteStringSlow(const char* text) noexcept;
void        NoteResultSlow(int64_t result) noexcept;
void        NoteResultStringSlow(int64_t result, const char* text) noexcept;
} // namespace detail

[[nodiscard]] inline bool Enabled() noexcept {
	return detail::g_enabled;
}

// Attach text (a path, a name; truncated) to the HLE call the current thread is serving.
inline void NoteString(const char* text) noexcept {
	if (detail::g_enabled) {
		detail::NoteStringSlow(text);
	}
}

// Attach the HLE call's result (return value, or -errno / guest error code) to it.
inline void NoteResult(int64_t result) noexcept {
	if (detail::g_enabled) {
		detail::NoteResultSlow(result);
	}
}

inline void NoteResultString(int64_t result, const char* text) noexcept {
	if (detail::g_enabled) {
		detail::NoteResultStringSlow(result, text);
	}
}

// `return Ret(x);` notes x as the HLE call's result and returns it unchanged.
template <class T>
inline T Ret(T value) noexcept {
	if (detail::g_enabled) {
		detail::NoteResultSlow(static_cast<int64_t>(value));
	}
	return value;
}

// Returns a function address that records the call under `name` and then runs `target`; or
// `target` itself when tracing is off or unsupported on this host.
[[nodiscard]] uint64_t Wrap(const char* name, uint64_t target);

// Appends " <module>+0x<offset>" style text for a code address to `out` (at most `size` bytes),
// or nothing; supplied by the loader so this file does not depend on it.
using LocateFn = void (*)(uint64_t address, char* out, size_t size);

// Writes the calling thread's last `own_entries` calls, then the last `merged_entries` calls of
// all other threads on one timeline (most recent last). Safe to call from a fault handler: it takes
// no locks and allocates nothing (a thread registering at that moment is skipped).
void Dump(FILE* out, unsigned own_entries, unsigned merged_entries, LocateFn locate) noexcept;

// Number of HLE calls recorded so far (all threads); 0 when off.
[[nodiscard]] uint64_t CallCount() noexcept;

} // namespace Common::HleTrace

#endif
