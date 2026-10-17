#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/cache/uploadCopier.h"

#include "common/emulatorConfig.h"
#include "common/gpuPhaseStats.h"
#include "common/hangWatchdog.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/gpuOps.h"
#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	GpuOps::Initialize(graphics, m_master, Config::GetGpuOpsMode());
	// A hang report shows the state of the operation queues (never blocks on their mutex).
	m_hang_provider_name = "command_scheduler@" + std::to_string(reinterpret_cast<uintptr_t>(this));
	Common::Hang::RegisterProvider(m_hang_provider_name.c_str(), [this] {
		std::string text = "tick current=" + std::to_string(m_master.CurrentTick()) +
		                   " known_gpu=" + std::to_string(m_master.KnownGpuTick());
		std::unique_lock lock(m_operation_mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return text + " (operation mutex busy)";
		}
		text += " | priority ops=" + std::to_string(m_priority_operations.size());
		if (!m_priority_operations.empty()) {
			text += " front_tick=" + std::to_string(m_priority_operations.front().tick) +
			        " back_tick=" + std::to_string(m_priority_operations.back().tick);
		}
		text += " active=" + std::to_string(m_priority_active ? m_priority_active_tick : 0) +
		        " | pending ops=" + std::to_string(m_pending_operations.size());
		if (!m_pending_operations.empty()) {
			text += " front_tick=" + std::to_string(m_pending_operations.front().tick);
		}
		return text;
	});
}

CommandScheduler::~CommandScheduler() {
	Common::Hang::UnregisterProvider(m_hang_provider_name.c_str());
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

// --srt-plan-shadow: a drain ends the span of draws whose plan lead is reported together.
static void NoteShadowDrain() {
	if (SrtPlan::ShadowEnabled() && !SrtPlan::AdoptEnabled() && GuestGpu::IsGpuThread()) SrtPlan::NoteDrain();
}

void CommandScheduler::FlushAndWait() {
	NoteShadowDrain();
	Common::GpuPhase::Scope gpu_phase_drain(Common::GpuPhase::Phase::DrainFlush);
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	NoteShadowDrain();
	Common::GpuPhase::Scope gpu_phase_drain(Common::GpuPhase::Phase::DrainFinish);
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		Common::GpuPhase::NoteDrainFull(true);
		NoteShadowDrain();
		Common::GpuPhase::Scope gpu_phase_drain(Common::GpuPhase::Phase::DrainTick);
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else if (!m_master.IsFree(tick)) {
		Common::GpuPhase::NoteDrainFull(false);
		NoteShadowDrain();
		Common::GpuPhase::Scope gpu_phase_drain(Common::GpuPhase::Phase::DrainTick);
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperationsLazy() {
	m_lazy_poll = Config::LazyOpPollEnabled();
	PopPendingOperations();
	m_lazy_poll = false;
}

void CommandScheduler::PopPendingOperations() {
	// Resolved draw: nothing is queued (the common case), so there is no lock to take and nothing a fresh
	// timeline value could release. The value is read again by every wait, so no one sees a stale tick as free.
	if (Config::ResolvedDrawEnabled() && m_pending_count.load(std::memory_order_acquire) == 0) {
		return;
	}
	bool refreshed = false;
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty()) {
				return;
			}
			const auto tick = m_pending_operations.front().tick;
			if (!m_master.IsFree(tick)) {
				// The timeline is polled once, and not for the open tick: it is not submitted yet.
				if (refreshed || tick >= CurrentTick()) {
					return;
				}
				if (m_lazy_poll) {
					const auto now = static_cast<uint64_t>(
					    std::chrono::duration_cast<std::chrono::nanoseconds>(
					        std::chrono::steady_clock::now().time_since_epoch()).count());
					if (now - m_last_poll_ns < 100'000u) {
						return;
					}
					m_last_poll_ns = now;
				}
			} else {
				// The per-draw form does not block on the priority thread: an operation that has to
				// run after an earlier guest-memory completion stays queued and is looked at again by
				// the next draw (the draw that waited here stalled 2 ms per frame in the level).
				if (m_lazy_poll && PriorityOperationsBeforeUnlocked(tick)) {
					return;
				}
				operation = std::move(m_pending_operations.front());
				m_pending_operations.pop();
				m_pending_count.fetch_sub(1, std::memory_order_release);
			}
		}
		if (!operation.callback) {
			m_master.Refresh();
			refreshed = true;
			continue;
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

bool CommandScheduler::HasOpenTickPriorityOperation() {
	std::lock_guard lock(m_operation_mutex);
	// An operation is queued with the tick of the command buffer that is open at the time; once that
	// buffer is submitted the current tick moves past it.
	return !m_priority_operations.empty() && m_max_priority_tick >= m_master.CurrentTick();
}

void CommandScheduler::DeferPriorityOperationAt(Common::UniqueFunction<void>&& operation,
                                                uint64_t                        tick) {
	QueueOperation(std::move(operation), true, tick);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority,
                                      uint64_t tick) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		const uint64_t op_tick = tick != 0 ? tick : CurrentTick();
		if (priority) {
			m_max_priority_tick = std::max(m_max_priority_tick, op_tick);
		}
		queue.push({std::move(operation), op_tick});
		if (!priority) {
			m_pending_count.fetch_add(1, std::memory_order_release);
		}
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	KYTY_PROFILER_THREAD("kyPrioOps");
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

bool CommandScheduler::PriorityOperationsDone(uint64_t tick) {
	std::lock_guard lock(m_operation_mutex);
	const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
	const bool queued_before_or_at =
	    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
	return !active_before_or_at && !queued_before_or_at;
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	m_dynamic_state.Invalidate();
	m_push_cache.Invalidate();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	Common::GpuPhase::Scope gpu_phase_submit(Common::GpuPhase::Phase::Submit);
	Common::GpuPhase::NoteSubmit();
	EXIT_IF(m_command.IsInvalid());
	UploadCopier::FenceIfCreated(); // --async-upload-copies: the ring is complete before the GPU may read it
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
