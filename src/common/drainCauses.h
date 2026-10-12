#ifndef KYTY_COMMON_DRAIN_CAUSES_H_
#define KYTY_COMMON_DRAIN_CAUSES_H_

// Pure bookkeeping for "why did the GPU thread wait for the GPU" (K20-K22 groundwork; see
// tests/DrainCausesTests.cpp). No clock, no threads, no dependencies. The runtime wrapper is
// common/gpuPhaseStats.h (--gpu-phase-stats), which tags every CommandScheduler::Wait with the
// cause that was current on the GPU thread.
//
// A drain is "full" when the waited tick is the current, not yet submitted one: the wait forces a
// submit and then idles the thread until everything queued so far has finished. A drain on an
// older tick only waits for work that was already in flight.
//
// Idea from BryanKAdams/BryKytyPS5 (--drain-stats; GPL-2.0), re-implemented.

#include <array>
#include <cstddef>
#include <cstdint>

namespace Common::DrainCauses {

enum class Cause : uint8_t {
	Other,          // not tagged
	FaultBuffer,    // GPU page-fault buffer parse: waits for the previous parse of the same area
	StreamWrap,     // upload/stream/staging ring wrapped onto work still in flight
	DownloadWrap,   // download ring wrapped onto work still in flight
	BufferReadback, // synchronous GPU->guest buffer download, caller not tagged
	ReadbackFault,  // ... for a game-thread read of GPU-written memory (page fault)
	ReadbackFaultGpu, // ... for a read by the GPU thread itself (shader/metadata parsing) of a
	                  //     page that also holds GPU-written bytes
	ReadbackCpuWrite, // ... for a CPU write over GPU-written memory (page fault)
	ReadbackIndirect, // ... for the arguments of an indirect dispatch (LDS sizing)
	ReadbackTextureMeta, // ... for the clear code of a render-target metadata block
	BufferGc,       // buffer GC publishing downloads before it frees GPU-modified buffers
	TextureReclaim, // texture reclaim waiting for deferred deletions
	FramePool,      // presentation frame pool waiting for the frame's previous present
	Count
};

constexpr std::array<const char*, static_cast<size_t>(Cause::Count)> kCauseNames = {
    "other",          "fault_buffer",  "stream_wrap", "download_wrap",
    "buffer_readback", "readback_fault", "readback_fault_gpu", "readback_cpu_write", "readback_indirect",
    "readback_texture_meta", "buffer_gc", "texture_reclaim", "frame_pool"};

struct Entry {
	uint64_t calls      = 0; // drains
	uint64_t full_calls = 0; // of which forced a submit of the current command buffer
	uint64_t ticks      = 0; // time units of the caller's clock

	[[nodiscard]] Entry Minus(const Entry& base) const {
		return {calls - base.calls, full_calls - base.full_calls, ticks - base.ticks};
	}
};

class Table {
public:
	void Add(Cause cause, bool full, uint64_t ticks) {
		auto index = static_cast<size_t>(cause);
		if (index >= m_entries.size()) {
			index = 0;
		}
		auto& e = m_entries[index];
		++e.calls;
		e.full_calls += full ? 1 : 0;
		e.ticks += ticks;
	}

	[[nodiscard]] const Entry& Get(Cause cause) const {
		return m_entries[static_cast<size_t>(cause)];
	}

	[[nodiscard]] uint64_t TotalTicks() const {
		uint64_t sum = 0;
		for (const auto& e: m_entries) {
			sum += e.ticks;
		}
		return sum;
	}

	[[nodiscard]] uint64_t TotalCalls() const {
		uint64_t sum = 0;
		for (const auto& e: m_entries) {
			sum += e.calls;
		}
		return sum;
	}

	// Entries accumulated since `base` (an earlier copy of this table).
	[[nodiscard]] Table Since(const Table& base) const {
		Table out;
		for (size_t i = 0; i < m_entries.size(); ++i) {
			out.m_entries[i] = m_entries[i].Minus(base.m_entries[i]);
		}
		return out;
	}

private:
	std::array<Entry, static_cast<size_t>(Cause::Count)> m_entries {};
};

} // namespace Common::DrainCauses

#endif /* KYTY_COMMON_DRAIN_CAUSES_H_ */
