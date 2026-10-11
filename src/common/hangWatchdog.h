#ifndef KYTY_COMMON_HANG_WATCHDOG_H_
#define KYTY_COMMON_HANG_WATCHDOG_H_

// --debug-hang-watchdog <seconds> (docs/lookahead-notes.md, "Hang investigation").
//
// Debug aid, off by default (one relaxed atomic load per probe). When no frame has been presented
// for <seconds>, a watchdog thread writes one report to stderr and to _hang_watchdog.txt in the
// working directory:
//   * every thread of the process: name, state, CPU use over a one-second sample, kernel wait
//     channel, the phase tag it published (Phase below) and how long it has been in it;
//   * the state reported by registered providers (worker pool queue and running jobs, in-flight
//     job registries, the lookahead thread, ...);
//   * a native backtrace (raw addresses + module offsets, resolve with the lld map file or
//     addr2line) of every thread that used CPU during the sample.
// A report is repeated every <seconds> while the stall lasts (at most 5 times).
//
// ptrace/gdb attach is not needed, so it works under Yama ptrace_scope=1.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace Common::Hang {

// Starts the watchdog thread. seconds == 0 leaves everything disabled. Idempotent.
void Start(uint32_t seconds);

namespace detail {
extern std::atomic<bool> g_enabled;
void        PhaseEnter(const char* what, uint64_t a, uint64_t b, const char*& prev_what,
                       uint64_t& prev_a, uint64_t& prev_b, int64_t& prev_since);
void        PhaseLeave(const char* prev_what, uint64_t prev_a, uint64_t prev_b, int64_t prev_since);
} // namespace detail

[[nodiscard]] inline bool Enabled() noexcept {
	return detail::g_enabled.load(std::memory_order_relaxed);
}

// A frame was presented (resets the stall timer).
void NoteProgress() noexcept;

// Named state provider, called from the watchdog thread while it writes a report. Must be safe to
// call at any time from another thread. Re-registering a name replaces it.
void RegisterProvider(const char* name, std::function<std::string()> provider);
void UnregisterProvider(const char* name);

// RAII phase tag for the calling thread: "I am in <what>(a, b) since now". Nests (the previous tag
// is restored). `what` must be a string literal.
class Phase {
public:
	Phase(const char* what, uint64_t a = 0, uint64_t b = 0) noexcept {
		if (Enabled()) {
			m_active = true;
			detail::PhaseEnter(what, a, b, m_prev_what, m_prev_a, m_prev_b, m_prev_since);
		}
	}
	~Phase() {
		if (m_active) {
			detail::PhaseLeave(m_prev_what, m_prev_a, m_prev_b, m_prev_since);
		}
	}
	Phase(const Phase&)            = delete;
	Phase& operator=(const Phase&) = delete;

private:
	bool        m_active     = false;
	const char* m_prev_what  = nullptr;
	uint64_t    m_prev_a     = 0;
	uint64_t    m_prev_b     = 0;
	int64_t     m_prev_since = 0;
};

} // namespace Common::Hang

#endif // KYTY_COMMON_HANG_WATCHDOG_H_
