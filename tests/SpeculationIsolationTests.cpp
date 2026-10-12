// Isolated speculative aborts (src/common/speculation.*): a thread with an abort handler is parked
// by a fatal check without switching the process-wide lookahead off; a thread without one keeps the
// old behaviour.

#include "common/speculation.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SpeculationIsolationTests: failed: %s\n", message);
		std::abort();
	}
}

bool WaitUntil(const std::atomic<bool>& flag, int timeout_ms = 5000) {
	for (int i = 0; i < timeout_ms && !flag.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return flag.load();
}

} // namespace

int main() {
	std::atomic<bool>            handled {false};
	std::atomic<std::thread::id> handler_thread {};
	std::atomic<bool>            returned {false};
	std::thread                  isolated([&] {
        Common::Speculation::SetThreadAbortHandler(
            [](void* user) {
                auto* self = static_cast<std::atomic<bool>*>(user);
                self->store(true);
            },
            &handled);
        handler_thread.store(std::this_thread::get_id());
        Common::Speculation::Scope scope;
        Common::Speculation::Abort("test.cpp", 1, "isolated check");
        returned.store(true); // never reached
    });
	isolated.detach();
	Check(WaitUntil(handled), "the abort handler did not run");
	Check(!Common::Speculation::Aborted(), "an isolated abort switched the lookahead off");
	Check(Common::Speculation::IsolatedAborts() == 1, "isolated abort not counted");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Check(!returned.load(), "an aborted thread must not continue");

	// Without a handler the old behaviour: the process-wide flag is set.
	std::thread plain([] {
		Common::Speculation::Scope scope;
		Common::Speculation::Abort("test.cpp", 2, "plain check");
	});
	plain.detach();
	for (int i = 0; i < 5000 && !Common::Speculation::Aborted(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Check(Common::Speculation::Aborted(), "a plain abort must switch the lookahead off");
	Check(Common::Speculation::IsolatedAborts() == 1, "a plain abort counted as isolated");
	std::printf("SpeculationIsolationTests: all cases passed\n");
	std::fflush(stdout);
	std::_Exit(0); // two threads are parked forever by design
}
