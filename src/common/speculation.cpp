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
thread_local AbortHandler t_abort_handler = nullptr;
thread_local void*        t_abort_user    = nullptr;
std::atomic<uint32_t>     g_isolated_aborts {0};
} // namespace

void SetThreadAbortHandler(AbortHandler handler, void* user) noexcept {
	t_abort_handler = handler;
	t_abort_user    = user;
}

uint32_t IsolatedAborts() noexcept {
	return g_isolated_aborts.load(std::memory_order_acquire);
}

thread_local const GuestRetirement* t_retirement = nullptr;
thread_local uint32_t               t_item_queue = 0;
thread_local uint64_t               t_item_seq   = 0;

void SetItemWatch(const GuestRetirement* retirement, uint32_t queue, uint64_t item_seq) noexcept {
	t_retirement = retirement;
	t_item_queue = queue;
	t_item_seq   = item_seq;
}

void ClearItemWatch() noexcept {
	t_retirement = nullptr;
	t_item_queue = 0;
	t_item_seq   = 0;
}

bool ItemStale() noexcept {
	return t_retirement != nullptr && t_item_seq != 0 &&
	       t_retirement->IsRetired(t_item_queue, t_item_seq);
}

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
	if (t_abort_handler != nullptr) {
		const auto count = g_isolated_aborts.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (count <= 8) {
			std::fprintf(stderr, "Pipeline precompile: speculative check failed at %s:%d (%.*s); "
			                     "that shader is skipped (%u so far)\n",
			             file, line, static_cast<int>(text.size()), text.data(), count);
			std::fflush(stderr);
		}
		t_abort_handler(t_abort_user);
	} else if (!g_aborted.exchange(true, std::memory_order_acq_rel)) {
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
