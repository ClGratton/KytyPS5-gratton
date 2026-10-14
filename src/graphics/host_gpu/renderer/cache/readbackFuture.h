#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKFUTURE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKFUTURE_H_

// Pure parts of --readback-futures (frame pipelining, notes/frame-pipelining.md): the policy of the
// three steps a game thread's read fault takes, and the one-shot release the faulting thread waits
// on. Unit tested in tests/ReadbackFutureTests.cpp; BufferCache owns the state they act on.
//
//   P1 (GPU thread)   copy the GPU-written bytes out, submit, queue a release behind the write-back
//   wait (guest)      block on the release (the only thread that waits for the GPU)
//   P3 (GPU thread)   the copy landed: unprotect the pages unless a newer GPU write reached them
//
// The GPU thread never waits for the GPU in P1 or P3.

#include <cstdint>
#include <semaphore>

namespace Libs::Graphics {

// What P1 found.
enum class FutureStart : uint8_t {
	Sync, // nothing to copy and nothing in flight, or it could not be scheduled: use the old path
	Wait, // a copy is in flight (new or older): the faulting thread waits for the release
};

// `scheduled`: copies P1 just recorded; `in_flight`: pages that already had a copy in flight.
[[nodiscard]] constexpr FutureStart DecideFutureStart(uint32_t scheduled, bool in_flight) {
	return scheduled != 0 || in_flight ? FutureStart::Wait : FutureStart::Sync;
}

// After P3: `readable` is true when no page of the faulted range is still protected for the GPU.
enum class FutureRound : uint8_t {
	Done,     // the thread may retry its access
	Retry,    // a newer GPU write kept a page protected: run P1 again
	Fallback, // too many rounds (the GPU rewrites the page faster than the copy lands): old path
};

inline constexpr uint32_t kMaxFutureRounds = 3;

[[nodiscard]] constexpr FutureRound NextFutureRound(uint32_t rounds_done, bool readable) {
	if (readable) {
		return FutureRound::Done;
	}
	return rounds_done + 1 < kMaxFutureRounds ? FutureRound::Retry : FutureRound::Fallback;
}

// One-shot, released by the priority-operations thread once the write-back of the copy has run.
class FutureRelease {
public:
	void Signal() { m_semaphore.release(); }
	void Wait() { m_semaphore.acquire(); }
	[[nodiscard]] bool TryWait() { return m_semaphore.try_acquire(); }

private:
	std::binary_semaphore m_semaphore {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKFUTURE_H_
