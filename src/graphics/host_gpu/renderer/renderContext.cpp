#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/gpuPhaseStats.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memResidency.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	MemResidency::Enable(Config::MemResidencyEnabled());
	// --pipelines-unoptimized-first: a pipeline replaced by its optimized build may still be used
	// by command buffers in flight, so it is destroyed once the GPU finished the current tick.
	m_pipeline_cache.SetRetireHandler([this](vk::Pipeline replaced) {
		if (!m_command_scheduler.Active()) {
			return false;
		}
		m_command_scheduler.DeferOperation([device = m_graphics.device, replaced] {
			device.destroyPipeline(replaced, nullptr);
		});
		return true;
	});
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
	MemResidency::Enable(false);
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (MemResidency::Enabled()) {
		const auto actor = MemResidency::CurrentActor();
		if (m_page_manager.ConsumeReadProbe(fault_vaddr)) {
			// A one-shot probe on a clean uploaded texture page. The access is retried with the
			// tracker's own protection, so a write is counted again as the write fault it is.
			const auto cls = m_texture_cache.ClassifyAddress(fault_vaddr);
			if (access == PageFaultAccess::Write) {
				MemResidency::NoteProbeWrite(cls, actor);
			} else {
				MemResidency::NoteProbeRead(cls, actor);
			}
			return true;
		}
		if (access == PageFaultAccess::Write) {
			MemResidency::NoteWriteFault(m_texture_cache.ClassifyAddress(fault_vaddr), actor);
		} else {
			MemResidency::NoteReadFault(actor);
		}
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		if (GuestGpu::IsGpuThread()) {
			Common::GpuPhase::NoteGpuThreadFault();
		}
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size, false,
		                          Common::GpuPhase::DrainCause::ReadbackFault);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_mapped_generation++;
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		m_mapped_generation++;
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda(bool address_writes) {
	if (address_writes) {
		m_buffer_cache.NoteAddressWrites();
	}
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	// Generations are read before the pass, so a change that lands during it is seen next time.
	const BdaSync::Generations now {BdaSync::g_cpu_dirty_generation.load(std::memory_order_acquire),
	                                m_buffer_cache.BufferSetGeneration(), m_mapped_generation};
	if (m_bda_sync_gate.CanSkip(Config::BdaSyncSkip(), now)) {
		m_bda_skipped++;
	} else {
		const auto pass_start = std::chrono::steady_clock::now();
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
		});
		m_bda_full_ns += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - pass_start)
		        .count());
		m_bda_sync_gate.RecordFullPass(now);
		m_bda_full++;
	}
	if (((m_bda_full + m_bda_skipped) & 0x3FFF) == 0) {
		Log::WriteToConsoleAndLog(
		    fmt::format("GPU: BDA sync calls={} full={} skipped={} full_pass_ms={:.1f} "
		                "(--bda-sync-skip {})\n",
		                m_bda_full + m_bda_skipped, m_bda_full, m_bda_skipped,
		                static_cast<double>(m_bda_full_ns) / 1e6,
		                Config::BdaSyncSkip() ? "on" : "off"));
	}
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	Common::GpuPhase::Scope gpu_phase_gc(Common::GpuPhase::Phase::Gc);
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
	if (MemResidency::SampleDue()) {
		SampleMemResidency();
	}
}

void RenderContext::SampleMemResidency() {
	MemResidency::Snapshot snapshot;
	snapshot.frame  = Common::GpuPhase::Frames();
	snapshot.probes = m_page_manager.GetProbeStats();
	m_texture_cache.CollectResidencyRanges(snapshot);
	m_buffer_cache.CollectResidencyRanges(snapshot);
	MemResidency::Submit(std::move(snapshot));
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
