#include "common/speculation.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace Common::Speculation {

namespace {
thread_local int     t_depth = 0;
thread_local bool    t_failed = false;
constexpr int        kMaxHeld = 4;
thread_local std::shared_mutex* t_held[kMaxHeld] = {};
thread_local bool t_held_shared[kMaxHeld] = {};
thread_local int     t_held_count = 0;
std::atomic<bool>    g_aborted {false};
} // namespace

bool Active() noexcept {
	return t_depth > 0;
}

void Enter() noexcept {
	t_depth++;
}

void Leave() noexcept {
	t_depth--;
}

HeldLock::HeldLock(std::shared_mutex* mutex, bool shared) noexcept
    : m_mutex(mutex), m_shared(shared) {
	if (t_held_count < kMaxHeld) {
		t_held[t_held_count]        = mutex;
		t_held_shared[t_held_count] = shared;
	}
	t_held_count++;
}

HeldLock::~HeldLock() {
	t_held_count--;
	if (t_held_count < kMaxHeld) {
		t_held[t_held_count] = nullptr;
	}
}

void NoteFailure() noexcept {
	t_failed = true;
}

bool HasFailure() noexcept {
	return t_failed;
}

bool TakeFailure() noexcept {
	const bool failed = t_failed;
	t_failed          = false;
	return failed;
}

bool Aborted() noexcept {
	return g_aborted.load(std::memory_order_acquire);
}

void Abort(const char* file, int line, std::string_view text) {
	// Release the locks the speculative code held when the check fired (their guards never run).
	for (int i = t_held_count - 1; i >= 0; i--) {
		if (i < kMaxHeld && t_held[i] != nullptr) {
			if (t_held_shared[i]) {
				t_held[i]->unlock_shared();
			} else {
				t_held[i]->unlock();
			}
			t_held[i] = nullptr;
		}
	}
	t_held_count = 0;
	if (!g_aborted.exchange(true, std::memory_order_acq_rel)) {
		std::fprintf(stderr, "Pipeline lookahead: speculative check failed at %s:%d (%.*s); "
		                     "lookahead disabled for the rest of the run\n",
		             file, line, static_cast<int>(text.size()), text.data());
		std::fflush(stderr);
	}
	for (;;) {
		std::this_thread::sleep_for(std::chrono::hours(1));
	}
}

} // namespace Common::Speculation
