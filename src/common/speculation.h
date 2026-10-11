#ifndef KYTY_COMMON_SPECULATION_H_
#define KYTY_COMMON_SPECULATION_H_

// Speculative execution guard for --pipeline-lookahead (docs/lookahead-notes.md).
//
// The lookahead thread runs the normal PM4 decoder and the normal shader/pipeline key code on
// command buffers the GPU thread has not executed yet, so it can see state the real path would
// never see (half-written descriptors, stale pointers). The project is built with
// -fno-exceptions, so a fatal check (EXIT*) on that thread cannot be unwound. Instead of killing
// the emulator for a check that only fires on speculative input, the fatal handlers call
// Abort(): it prints one line, switches the lookahead off for the rest of the run and parks the
// thread forever. The lookahead code never holds a lock across code that can reach an EXIT*
// (see the notes), so a parked thread cannot block the real GPU thread.

#include <atomic>
#include <shared_mutex>
#include <string_view>

namespace Common::Speculation {

// True on the lookahead thread while it runs speculative code.
[[nodiscard]] bool Active() noexcept;
void               Enter() noexcept;
void               Leave() noexcept;

// Non-fatal speculative failure ("this input is not usable"): code that would EXIT on the real
// path records it and returns early; the lookahead caller checks TakeFailure() and drops the
// request. Only meaningful on a thread where Active() is true.
void               NoteFailure() noexcept;
[[nodiscard]] bool TakeFailure() noexcept;
[[nodiscard]] bool HasFailure() noexcept;

// Set by Abort(); the lookahead producers check it and stop queueing work.
[[nodiscard]] bool Aborted() noexcept;

// Called by the fatal handlers when Active(). Never returns.
[[noreturn]] void Abort(const char* file, int line, std::string_view text);

// Registers a lock the calling (speculative) thread currently holds, so Abort() can release it
// before parking the thread. Only the shared-mutex flavour is needed.
class HeldLock {
public:
	explicit HeldLock(std::shared_mutex* mutex, bool shared = false) noexcept;
	~HeldLock();
	HeldLock(const HeldLock&)            = delete;
	HeldLock& operator=(const HeldLock&) = delete;

private:
	std::shared_mutex* m_mutex;
	bool               m_shared;
};

struct Scope {
	Scope() noexcept { Enter(); }
	~Scope() { Leave(); }
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
};

} // namespace Common::Speculation

#endif // KYTY_COMMON_SPECULATION_H_
