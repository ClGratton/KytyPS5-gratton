#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_POLLWAIT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_POLLWAIT_H_

// --poll-before-wait <us>: before blocking in vkWaitSemaphores on a timeline value, poll the value
// for a bounded time. A readback of a few bytes submits a tiny command buffer on a (nearly) idle
// GPU and waits for it: the GPU finishes within tens of microseconds, but a blocking wait costs the
// kernel sleep and wake-up on top. Unit tested in tests/PollWaitTests.cpp.

#include <cstdint>

namespace Libs::Graphics {

// Polls `reached()` until it returns true or `budget_ns` has elapsed on `now_ns()`. Returns true
// when the condition was seen. The clock is read once per poll; with budget 0 nothing is polled.
// `pause()` is called between polls (a CPU relax hint in production, a counter in tests).
template <typename Reached, typename Now, typename Pause>
[[nodiscard]] bool PollUntil(Reached&& reached, Now&& now_ns, Pause&& pause, uint64_t budget_ns) {
	if (budget_ns == 0) {
		return false;
	}
	const uint64_t start = now_ns();
	for (;;) {
		if (reached()) {
			return true;
		}
		if (now_ns() - start >= budget_ns) {
			return false;
		}
		pause();
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_POLLWAIT_H_
