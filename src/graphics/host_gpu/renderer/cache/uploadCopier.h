#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_UPLOADCOPIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_UPLOADCOPIER_H_

// --async-upload-copies: the guest-memory-to-staging memcpy of a buffer upload, on helper threads.
//
// BufferCache::UploadCopies copies every dirty run from the guest backing into the upload ring (23 MB a frame in the level, about 3 ms of
// the GPU thread, bound by write-combined memory bandwidth). The GPU does not read the ring before vkQueueSubmit, and the tracker has
// already protected the pages again before the copy starts (a write after that faults and dirties the page for the next upload), so
// the memcpy may finish any time before the submit. The GPU thread queues the runs and returns; CommandScheduler::Submit (and anything
// that unmaps guest memory) calls Fence() first, which waits for the helpers and redoes on the calling thread what they could not read.
// Only for coherent staging memory (a non-coherent ring needs its flush after the write; Commit() still does it, so the caller keeps
// the plain path there).

#include "common/finishedPrefix.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class UploadCopier {
public:
	static UploadCopier& Get();
	// Any thread: Fence() on the copier if one exists (nothing was ever queued otherwise).
	static void FenceIfCreated();

	struct Item {
		uint8_t* destination   = nullptr;
		uint64_t guest_address = 0;
		uint64_t size          = 0;
		uint64_t seq           = 0; // 1, 2, 3... in queue order; set by Submit()
	};

	// GPU thread. Queues the copies [guest_address, guest_address + size) -> destination, one wake-up for the lot.
	void Submit(const Item* items, size_t count);
	// GPU thread. Returns when every queued copy has finished (and was redone here if a helper could not read the range).
	void Fence();
	// --submit-thread: the number of copies queued so far (the sequence number of the last one), and the wait for the copies up to
	// one. Any thread. The helpers finish in any order, so the wait is on the longest finished prefix of the queue, not on a count:
	// FenceUpTo(n) returns when copies 1..n are all done (redone on the calling thread if a helper could not read the range), while
	// later copies queued for the next command buffer may still run.
	[[nodiscard]] static uint64_t SubmittedCountIfCreated();
	static void                   FenceUpToIfCreated(uint64_t seq);
	void                          FenceUpTo(uint64_t seq);
	[[nodiscard]] bool Outstanding() const noexcept {
		return m_done.load(std::memory_order_acquire) != m_submitted.load(std::memory_order_relaxed);
	}

private:
	UploadCopier();
	~UploadCopier() = default;

	void Run();
	void RedoFailed();

	static constexpr size_t kThreads = 2;

	std::mutex              m_mutex;
	std::condition_variable m_wake;
	std::deque<Item>        m_queue;
	std::vector<Item>       m_failed; // under m_mutex
	std::vector<std::thread> m_threads;
	std::atomic<uint64_t>   m_submitted {0};
	std::atomic<uint64_t>   m_done {0};
	// The longest prefix 1..n of the queue whose copies are all finished (common/finishedPrefix.h). The producer keeps fewer than
	// kRing copies ahead of it.
	static constexpr size_t kRing = 4096;
	Common::FinishedPrefix<kRing> m_prefix;
	std::atomic<uint32_t>   m_pending {0};  // queued, not yet taken
	std::atomic<uint32_t>   m_sleepers {0}; // helpers waiting on m_wake
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_UPLOADCOPIER_H_
