#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/pipeline/pushDescriptorCache.h"
#include "graphics/host_gpu/renderer/render.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <atomic>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	CommandScheduler(RenderContext& context, GraphicContext& graphics);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           FlushAndWait();
	void           Finish();
	CommandBuffer& BeginCommand();
	// `allow_async`: --submit-thread may hand the vkQueueSubmit to the submit thread (a flush at a batch boundary; the callers that
	// are about to wait for the GPU submit in line).
	uint64_t       Submit(SubmitInfo submit = {}, bool allow_async = false);
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	// The per-draw form: with --lazy-op-poll an operation that is not known to be done is looked at again
	// (one vkGetSemaphoreCounterValue, 0.5-1 us on the driver) at most every 100 us, instead of at every draw,
	// and an operation behind an unfinished priority operation stays queued instead of blocking the draw.
	// Operations only release resources or publish results, so running one a few draws later is exact.
	void                      PopPendingOperationsLazy();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	// Non-blocking form: true when every priority operation deferred at or before `tick` has run.
	[[nodiscard]] bool        PriorityOperationsDone(uint64_t tick);
	// Guest-memory completions use the priority queue; normal callbacks maintain GPU resources.
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	// Like the above, but runs after `tick` (a tick already submitted) instead of the open command
	// buffer's tick, so the caller need not submit. The operation must not touch guest memory in a
	// way that depends on its position among the other priority operations (a release does not).
	// True when a priority operation waits for the tick of the open (not yet submitted) command
	// buffer: it cannot run, and nothing behind it can either, until that buffer is submitted.
	[[nodiscard]] bool        HasOpenTickPriorityOperation();
	void                      DeferPriorityOperationAt(Common::UniqueFunction<void>&& operation,
	                                                   uint64_t                        tick);
	[[nodiscard]] bool        HasPendingPriorityOperations();
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] DynamicState&    GetDynamicState() noexcept { return m_dynamic_state; }
	[[nodiscard]] PushDescriptorCache& GetPushDescriptorCache() noexcept { return m_push_cache; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();

	// --submit-thread: what the submit thread needs to submit one command buffer that was ended on the GPU thread.
	struct SubmitJob {
		vk::CommandBuffer buffer;
		uint64_t          tick            = 0;
		uint64_t          copy_target     = 0; // UploadCopier copies 1..copy_target must be finished before the GPU may read the ring
		uint32_t          debug_op        = 0;
		uint64_t          debug_submit_id = 0;
		uint32_t          debug_arg0      = 0;
		uint32_t          debug_arg1      = 0;
		uint32_t          debug_arg2      = 0;
		uint32_t          debug_arg3      = 0;
		uint64_t          debug_arg4      = 0;
	};
	static constexpr size_t kMaxSubmitJobs = 32;
	uint64_t SubmitAsync(vk::CommandBuffer buffer);
	void     SubmitThreadMain(std::stop_token stop);
	// Returns when every job handed to the submit thread has been submitted (a no-op when none is outstanding).
	void     DrainSubmitThread();
	void     StopSubmitThread();
	void PriorityOperationsThread(std::stop_token stop);
	void QueueOperation(Common::UniqueFunction<void>&& operation, bool priority, uint64_t tick = 0);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	DynamicState                 m_dynamic_state;
	PushDescriptorCache          m_push_cache;
	std::queue<PendingOperation> m_pending_operations;
	std::atomic<uint32_t>        m_pending_count {0}; // size of m_pending_operations, readable without the lock
	// True when a priority operation deferred at or before `tick` is queued or running (m_operation_mutex held).
	[[nodiscard]] bool PriorityOperationsBeforeUnlocked(uint64_t tick) const {
		return (m_priority_active && m_priority_active_tick <= tick) ||
		       (!m_priority_operations.empty() && m_priority_operations.front().tick <= tick);
	}
	bool                         m_lazy_poll    = false; // GPU thread: this Pop is the throttled per-draw form
	uint64_t                     m_last_poll_ns = 0;     // GPU thread: when the throttled form last polled
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	std::string                  m_hang_provider_name;
	uint64_t                     m_max_priority_tick = 0; // guarded by m_operation_mutex
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	// --submit-thread. Jobs are submitted in the order of their ticks: the tick is taken and the job queued under m_submit_mutex,
	// and the thread takes them from the front. A job stays at the front until its vkQueueSubmit returned.
	std::mutex                   m_submit_mutex;
	std::condition_variable      m_submit_cv;
	std::deque<SubmitJob>        m_submit_jobs;
	std::atomic<uint32_t>        m_submit_outstanding {0}; // queued or being submitted
	std::jthread                 m_submit_thread;          // last: destroyed first
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
