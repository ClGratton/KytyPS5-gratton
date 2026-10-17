#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/bdaSyncGate.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "kernel/eventQueue.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

class GuestGpu;

class RenderContext {
public:
	explicit RenderContext(GraphicContext& graphics);
	~RenderContext();
	KYTY_CLASS_NO_COPY(RenderContext);

	[[nodiscard]] GraphicContext&           GetGraphics() const noexcept { return m_graphics; }
	void                                    InitializeGpu(VideoOut::VideoOutDriver* video_out);
	void                                    ShutdownGpu();
	[[nodiscard]] GuestGpu&                 GetGpu() const;
	[[nodiscard]] VideoOut::VideoOutDriver& GetVideoOut() const;

	Common::Mutex&      GetMutex() { return m_mutex; }
	CommandScheduler&   GetCommandScheduler() { return m_command_scheduler; }
	PipelineCache&      GetPipelineCache() { return m_pipeline_cache; }
	DescriptorHeap&     GetDescriptorHeap() { return m_descriptor_heap; }
	SamplerCache&       GetSamplerCache() { return m_sampler_cache; }
	BufferCache&        GetBufferCache() { return m_buffer_cache; }
	TextureCache&       GetTextureCache() { return m_texture_cache; }
	RenderExecutor&     GetRenderExecutor() { return m_render_executor; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	// --cp-direct-stores: the command processor's stores to guest memory (labels, fences, WRITE_DATA). Writes through the
	// backing view and tells the caches what a write fault would have told them, without the fault and without an
	// mprotect. False: not done, the caller stores to the guest address as before.
	[[nodiscard]] bool StoreGuest(uint64_t vaddr, const void* data, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	// After a guest protection change: pages the tracker watches get the tracker's protection back.
	void               ReapplyProtection(uint64_t vaddr, uint64_t size) {
		m_page_manager.ReapplyProtection(vaddr, size);
	}
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	// `address_writes`: the shader may store through raw addresses (see BufferCache::NoteAddressWrites).
	void               PrepareBda(bool address_writes = true);
	void               RunGarbageCollector();

	void AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void TriggerInterrupt(int event_id, uint32_t context_id);

private:
	struct InterruptEqRegistration {
		LibKernel::EventQueue::KernelEqueue eq       = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
		int                                 event_id = 0;
	};

	GraphicContext&           m_graphics;
	Common::Mutex             m_mutex;
	RenderExecutor            m_render_executor;
	CommandScheduler          m_command_scheduler;
	DescriptorHeap            m_descriptor_heap;
	PipelineCache             m_pipeline_cache;
	SamplerCache              m_sampler_cache;
	PageManager               m_page_manager;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	std::unique_ptr<GuestGpu> m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;
	bool                      m_fault_process_pending = false;
	void                      SampleMemResidency();
	bool                      m_bda_logged = false;
	// Page faults of the tracker (HandleFault): counts and the time guest threads spend in the handler.
	std::atomic<uint64_t>     m_fault_writes {0};
	std::atomic<uint64_t>     m_fault_reads {0};
	std::atomic<uint64_t>     m_fault_write_ns {0};
	std::atomic<uint64_t>     m_fault_gpu_thread {0};
	std::chrono::steady_clock::time_point m_fault_log_next {};
	void                      LogFaultStatsPeriodic();
	// PrepareBda skip gate (K15): the mapped-range generation changes under m_mapped_ranges_mutex.
	BdaSync::SkipGate         m_bda_sync_gate;
	uint64_t                  m_mapped_generation = 0;
	uint64_t                  m_bda_full          = 0;
	uint64_t                  m_bda_skipped       = 0;
	uint64_t                  m_bda_full_ns       = 0; // time spent in full passes
	// --bda-dirty-log: the log is armed by a full pass and valid while the mapping generation is the one of
	// that pass; the passes it replaced, the ranges it visited, and the full passes it fell back to.
	std::vector<std::pair<uint64_t, uint64_t>> m_bda_log_runs; // scratch of the log pass: merged [begin, end)
	uint64_t                  m_bda_log_runs_total = 0;
	bool                      m_bda_log_armed      = false;
	uint64_t                  m_bda_log_mapped_gen = 0;
	uint64_t                  m_bda_log_map_epoch  = 0;
	uint64_t                  m_bda_log_passes     = 0;
	uint64_t                  m_bda_log_ranges     = 0;
	uint64_t                  m_bda_log_falls      = 0; // full passes by overflow or a mapping change
	uint64_t                  m_bda_log_verified   = 0;
	void                      BdaFullPass();
	void                      BdaVerifyLogPass();

	Common::Mutex                        m_interrupt_mutex;
	std::vector<InterruptEqRegistration> m_interrupt_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERCONTEXT_H_
