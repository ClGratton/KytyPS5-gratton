#include "graphics/guest_gpu/pipelineLookahead.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/emulatorConfig.h"
#include "common/hangWatchdog.h"
#include "common/guestRetirement.h"
#include "common/speculation.h"
#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <cstdio>
#include <cstdlib>
#include <optional>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Libs::Graphics {

namespace {
constexpr uint32_t ComputeQueueBase = 0x20;
} // namespace

namespace {
// The lookahead keeps at most this many queued submissions / dwords (about 17 MB). If it falls
// behind (a slow request, a stuck decode) the oldest items are dropped: they are hints, and a
// hint that old is useless anyway. Without a cap a stalled lookahead would copy every command
// buffer the guest submits for as long as the stall lasts.
constexpr size_t   kMaxQueuedItems  = 256;
constexpr uint64_t kMaxQueuedDwords = 4'500'000;

uint64_t ItemDwords(const PipelineLookahead::Item& item) {
	return item.commands.size() + item.constant_commands.size();
}
} // namespace

// ---- --lookahead-workers ---------------------------------------------------------------------------

// The lookahead thread decodes PM4 and, instead of running the per-draw prefetch itself, copies the
// register state a draw needs into a ring slot; N workers run the same prefetch on the copy. Finished
// slots are collected in ring order by the sequencer, which keeps SrtPlan::Store single-producer and
// the "everything up to here was looked at" position monotonic. The prefetch is exact for any
// snapshot: a plan is checked against the real inputs and memory when kyGpu adopts it.
class PipelineLookahead::Pool {
public:
	static constexpr uint64_t kCapacity = 512; // slots (power of two)

	struct Task {
		enum class Kind : uint8_t { Draw, Dispatch, EndItem };
		Kind                                        kind = Kind::EndItem;
		uint64_t                                    seq = 0;
		uint32_t                                    ordinal = 0;
		uint32_t                                    sub = 0;
		uint32_t                                    queue_id = 0;
		bool                                        async_compute = false;
		ShadowDraw                                  draw;
		ShadowDispatch                              dispatch;
		HW::Context                                 ctx;
		HW::UserConfig                              ucfg;
		HW::Shader                                  sh;
		std::vector<std::unique_ptr<SrtPlan::Plan>> plans;
		bool                                        discard = false; // replay pass: plans are dropped
		std::atomic<bool>                           done {false};
	};

	Pool(PipelineLookahead& owner, uint32_t workers): m_owner(owner), m_ring(new Task[kCapacity]) {
		for (uint32_t i = 0; i < workers; ++i) {
			m_threads.emplace_back([this, i] { WorkerLoop(i); });
		}
	}

	// Sequencer: waits for a free slot (collecting finished ones meanwhile). Null when stopping or
	// the speculation guard switched the lookahead off.
	Task* Acquire() {
		const auto tail = m_tail.load(std::memory_order_relaxed);
		if (tail - m_head.load(std::memory_order_relaxed) >= kCapacity) {
			const auto t0 = Bench::NowNs();
			while (tail - m_head.load(std::memory_order_relaxed) >= kCapacity) {
				if (m_stopping.load(std::memory_order_acquire) || Common::Speculation::Aborted()) {
					return nullptr;
				}
				Collect();
				if (tail - m_head.load(std::memory_order_relaxed) >= kCapacity) {
					std::this_thread::sleep_for(std::chrono::microseconds(20));
				}
			}
			m_owner.m_stats.sequencer_stall_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
		}
		Task& task = m_ring[tail & (kCapacity - 1)];
		task.done.store(false, std::memory_order_relaxed);
		return &task;
	}

	// Sequencer: the slot returned by Acquire() is filled; hand it over.
	void Publish() {
		{
			std::lock_guard lock(m_mutex);
			m_tail.store(m_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		}
		const auto depth = m_tail.load(std::memory_order_relaxed) - m_head.load(std::memory_order_relaxed);
		auto       peak  = m_owner.m_stats.max_ring.load(std::memory_order_relaxed);
		while (depth > peak && !m_owner.m_stats.max_ring.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
		}
		m_owner.m_stats.tasks.fetch_add(1, std::memory_order_relaxed);
		m_cv.notify_one();
	}

	// Sequencer: moves finished slots, in order, into the plan store.
	void Collect() {
		auto head = m_head.load(std::memory_order_relaxed);
		const auto tail = m_tail.load(std::memory_order_relaxed);
		auto&      store = SrtPlan::GetStore();
		while (head != tail) {
			Task& task = m_ring[head & (kCapacity - 1)];
			if (!task.done.load(std::memory_order_acquire)) {
				break;
			}
			if (SrtPlan::Enabled() && !task.discard) {
				for (auto& plan: task.plans) {
					store.Put(std::move(plan));
				}
				store.NoteProgress(task.kind == Task::Kind::EndItem ? SrtPlan::Position {task.seq, UINT32_MAX}
				                                                    : SrtPlan::Position {task.seq, task.ordinal});
			}
			task.plans.clear();
			++head;
			m_head.store(head, std::memory_order_release);
		}
	}

	// Sequencer: blocks until every published slot has been collected.
	void Drain() {
		while (Pending() && !Common::Speculation::Aborted() && !m_stopping.load(std::memory_order_acquire)) {
			Collect();
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
	}

	[[nodiscard]] bool Pending() const {
		return m_tail.load(std::memory_order_relaxed) != m_head.load(std::memory_order_relaxed);
	}

	// Makes Acquire() give up and the workers leave (no join).
	void Interrupt() {
		{
			std::lock_guard lock(m_mutex);
			m_stopping.store(true, std::memory_order_release);
		}
		m_cv.notify_all();
	}

	// Stops the workers; stragglers (a worker parked by a speculative abort, or waiting for a
	// compile) are abandoned together with the pool (never freed), as the lookahead thread is.
	bool Stop() {
		{
			std::lock_guard lock(m_mutex);
			m_stopping.store(true, std::memory_order_release);
		}
		m_cv.notify_all();
		for (int i = 0; i < 400 && m_exited.load(std::memory_order_acquire) < m_threads.size() &&
		                !Common::Speculation::Aborted();
		     i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		const bool all = m_exited.load(std::memory_order_acquire) == m_threads.size();
		for (auto& thread: m_threads) {
			if (!thread.joinable()) continue;
			if (all) thread.join(); else thread.detach();
		}
		return all;
	}

private:
	void WorkerLoop(uint32_t index) {
#if defined(__linux__)
		pthread_setname_np(pthread_self(), fmt::format("kyty-la-{}", index).c_str());
#endif
		Common::Speculation::Scope speculation;
		for (;;) {
			Task* task = nullptr;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [this] {
					return m_stopping.load(std::memory_order_relaxed) ||
					       m_claim < m_tail.load(std::memory_order_relaxed);
				});
				if (m_stopping.load(std::memory_order_relaxed)) {
					break;
				}
				task = &m_ring[m_claim++ & (kCapacity - 1)];
			}
			Run(*task);
			task->done.store(true, std::memory_order_release);
		}
		m_exited.fetch_add(1, std::memory_order_release);
	}

	void Run(Task& task) {
		if (task.kind == Task::Kind::EndItem || Common::Speculation::Aborted()) {
			return;
		}
		const auto t0 = Bench::NowNs();
		Common::Speculation::SetItemWatch(&Common::GuestRetirement::Global(), task.queue_id, task.seq);
		if (Common::Speculation::ItemStale()) {
			m_owner.m_stats.tasks_stale.fetch_add(1, std::memory_order_relaxed);
		} else {
			SrtPlan::PlanSinkScope sink(&task.plans);
			std::optional<SrtPlan::EventScope> event;
			if (SrtPlan::Enabled()) {
				event.emplace(task.seq, task.ordinal, task.sub);
			}
			if (task.kind == Task::Kind::Draw) {
				Common::Hang::Phase phase("lookahead_draw", task.ordinal);
				RenderExecutor::PrefetchDrawStatus status {};
				(void)m_owner.m_renderer.GetRenderExecutor().PrefetchDraw(task.ctx, task.sh, task.ucfg,
				                                                          task.draw, status);
			} else {
				Common::Hang::Phase phase("lookahead_dispatch", task.ordinal);
				const uint32_t groups[3] = {task.dispatch.indirect ? 0u : task.dispatch.x,
				                            task.dispatch.indirect ? 0u : task.dispatch.y,
				                            task.dispatch.indirect ? 0u : task.dispatch.z};
				m_owner.m_renderer.GetPipelineCache().PrefetchCompute(
				    task.sh.GetCs(), task.ctx.GetShaderRegisters(), task.dispatch.mode,
				    task.async_compute, groups);
			}
		}
		Common::Speculation::ClearItemWatch();
		m_owner.m_stats.worker_busy_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
	}

	PipelineLookahead&       m_owner;
	std::unique_ptr<Task[]>  m_ring;
	std::mutex               m_mutex;
	std::condition_variable  m_cv;
	std::atomic<uint64_t>    m_head {0}; // sequencer: next slot to collect
	std::atomic<uint64_t>    m_tail {0}; // sequencer: next slot to fill
	uint64_t                 m_claim = 0; // under m_mutex: next slot a worker takes
	std::atomic<bool>        m_stopping {false};
	std::atomic<size_t>      m_exited {0};
	std::vector<std::thread> m_threads;
};

PipelineLookahead::PipelineLookahead(RenderContext& renderer): m_renderer(renderer) {
	if (const auto workers = Config::GetLookaheadWorkers(); workers != 0) {
		m_pool = std::make_unique<Pool>(*this, workers);
	}
	m_thread = std::thread([this] { Run(); });
	if (Common::Hang::Enabled()) {
		Common::Hang::RegisterProvider("pipeline_lookahead", [this] { return DebugState(); });
	}
}

PipelineLookahead::~PipelineLookahead() {
	Stop();
	if (m_pool != nullptr && !m_pool->Stop()) {
		(void)m_pool.release(); // a worker is still inside it: leave the pool to the process
	}
}

std::string PipelineLookahead::DebugState() {
	std::string queue = "queue=<locked>";
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if (lock.owns_lock()) {
			queue = fmt::format("queue={}", m_queue.size());
		}
	}
	const auto start = m_item_start_ns.load();
	std::string item = "idle";
	if (start != 0) {
		item = fmt::format("item kind={} dwords={} running_ms={} seq={}", m_item_kind.load(),
		                   m_item_dwords.load(), (Bench::NowNs() - start) / 1000000, m_current_seq);
	}
	return fmt::format("{} queued_dwords={} {} {}", queue, m_queued_dwords.load(), item,
	                   SummaryText());
}

void PipelineLookahead::Push(Item item) {
	if (Common::Speculation::Aborted()) {
		return; // the lookahead thread is parked for good; stop copying buffers
	}
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping) {
			return;
		}
		m_queued_dwords.fetch_add(ItemDwords(item), std::memory_order_relaxed);
		m_queue.push_back(std::move(item));
		while (m_queue.size() > kMaxQueuedItems ||
		       m_queued_dwords.load(std::memory_order_relaxed) > kMaxQueuedDwords) {
			m_queued_dwords.fetch_sub(ItemDwords(m_queue.front()), std::memory_order_relaxed);
			m_queue.pop_front();
			m_stats.items_dropped.fetch_add(1, std::memory_order_relaxed);
		}
		const auto depth = static_cast<uint64_t>(m_queue.size());
		auto       peak  = m_stats.max_queue.load(std::memory_order_relaxed);
		while (depth > peak &&
		       !m_stats.max_queue.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
		}
	}
	m_cv.notify_one();
}

void PipelineLookahead::Stop() {
	Common::Hang::UnregisterProvider("pipeline_lookahead");
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping) {
			return;
		}
		m_stopping = true;
		m_queue.clear();
		m_queued_dwords.store(0, std::memory_order_relaxed);
	}
	if (m_pool != nullptr) {
		m_pool->Interrupt();
	}
	m_cv.notify_all();
	if (!m_thread.joinable()) {
		return;
	}
	// A thread parked by a speculative abort never exits: do not wait for it.
	for (int i = 0; i < 400 && !m_exited.load(std::memory_order_acquire) &&
	                !Common::Speculation::Aborted();
	     i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (m_exited.load(std::memory_order_acquire)) {
		m_thread.join();
	} else {
		m_thread.detach();
	}
}

std::string PipelineLookahead::SummaryText() const {
	return fmt::format(
	    "items={};items_late={};draws={};dispatches={};max_queue={};busy_ms={};draw_ms={};dispatch_ms={};"
	    "aborted={};abandoned={};stale={};retire_signals={};dropped={};workers={};tasks={};worker_busy_ms={};"
	    "sequencer_stall_ms={};max_ring={};tasks_stale={};replay_tasks={};replay_ms={};sizeof_ctx={};sizeof_ucfg={};"
	    "sizeof_sh={};sizeof_vsinfo={};sizeof_psinfo={}",
	    m_stats.items.load(), m_stats.items_late.load(), m_stats.draws.load(),
	    m_stats.dispatches.load(), m_stats.max_queue.load(), m_stats.busy_ns.load() / 1000000,
	    m_stats.draw_ns.load() / 1000000, m_stats.dispatch_ns.load() / 1000000,
	    Common::Speculation::Aborted() ? 1 : 0, m_stats.items_abandoned.load(),
	    m_stats.items_stale.load(), Common::GuestRetirement::Global().SignalCalls(),
	    m_stats.items_dropped.load(), Config::GetLookaheadWorkers(), m_stats.tasks.load(),
	    m_stats.worker_busy_ns.load() / 1000000, m_stats.sequencer_stall_ns.load() / 1000000,
	    m_stats.max_ring.load(), m_stats.tasks_stale.load(), m_stats.replay_tasks.load(),
	    m_stats.replay_ns.load() / 1000000, sizeof(HW::Context), sizeof(HW::UserConfig), sizeof(HW::Shader),
	    sizeof(ShaderVertexInputInfo), sizeof(ShaderPixelInputInfo));
}

CommandProcessor& PipelineLookahead::Shadow(uint32_t queue_id) {
	auto& slot = m_shadows[queue_id];
	if (slot == nullptr) {
		const int event_id = queue_id == 0 ? 0 : static_cast<int>(ComputeQueueBase + queue_id - 1);
		slot               = std::make_unique<CommandProcessor>(m_renderer, event_id);
		slot->SetShadowSink(this);
		slot->Reset();
	}
	return *slot;
}

void PipelineLookahead::Run() {
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "kyty-lookahead");
#endif
	Common::Speculation::Scope speculation;
	for (;;) {
		Item item;
		{
			std::unique_lock lock(m_mutex);
			if (m_pool != nullptr && m_pool->Pending()) {
				// Finished prefetch results still have to reach the plan store while no stream arrives.
				while (!m_stopping && m_queue.empty() && m_pool->Pending()) {
					lock.unlock();
					m_pool->Collect();
					lock.lock();
					if (m_queue.empty() && m_pool->Pending()) {
						m_cv.wait_for(lock, std::chrono::microseconds(100));
					}
				}
			}
			m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
			if (m_stopping) {
				break;
			}
			item = std::move(m_queue.front());
			m_queue.pop_front();
			m_queued_dwords.fetch_sub(ItemDwords(item), std::memory_order_relaxed);
		}
		Process(item);
	}
	m_exited.store(true, std::memory_order_release);
}

void PipelineLookahead::RunStream(CommandProcessor& cp, std::span<const uint32_t> commands) {
	Pm4Execution execution;
	for (int round = 0; round < 8; round++) {
		if (cp.Process(execution, commands) == Pm4ProcessResult::Complete ||
		    !execution.MadeProgress()) {
			break;
		}
	}
	if (execution.Abandoned() && execution.AbandonReason() == Pm4Execution::kStaleAbandonReason) {
		// The guest has observed a completion that covers this submission while it was scanned: its
		// memory may be recycled, nothing more to trust in it. Routine under --async-pipelines skip,
		// so no log line.
		m_stats.items_stale.fetch_add(1, std::memory_order_relaxed);
		cp.Reset();
	} else if (execution.Abandoned()) {
		// The shadow's register state is now somewhere in the middle of the stream: forget it.
		const auto count = m_stats.items_abandoned.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count <= 3) {
			std::fprintf(stderr, "Pipeline lookahead: stream abandoned (%s), item seq=%llu\n",
			             execution.AbandonReason(), static_cast<unsigned long long>(m_current_seq));
		}
		cp.Reset();
	}
}

void PipelineLookahead::Process(Item& item) {
	const auto t0 = Bench::NowNs();
	m_stats.items.fetch_add(1, std::memory_order_relaxed);
	m_current_seq  = item.seq;
	m_current_queue = item.queue_id;
	m_current_late = item.seq != 0 && item.seq <= m_started_seq.load(std::memory_order_acquire);
	if (m_current_late) {
		m_stats.items_late.fetch_add(1, std::memory_order_relaxed);
	}
	Common::Speculation::SetItemWatch(&Common::GuestRetirement::Global(), item.queue_id, item.seq);
	SkipDeps::NoteLookaheadItem(item.seq, m_current_late);
	m_item_dwords.store(ItemDwords(item), std::memory_order_relaxed);
	m_item_kind.store(static_cast<uint32_t>(item.kind), std::memory_order_relaxed);
	m_item_start_ns.store(t0, std::memory_order_relaxed);
	Common::Hang::Phase phase("lookahead_item", item.seq, static_cast<uint64_t>(item.kind));
	auto& cp = Shadow(item.queue_id);
	const bool plan_shadow = SrtPlan::Enabled();
	if (plan_shadow) {
		if (m_current_late) SrtPlan::GetStore().NoteLate(item.seq);
		cp.SetPlanSequence(item.seq);
	}
	switch (item.kind) {
		case Kind::Reset: cp.Reset(); break;
		case Kind::Graphics:
		case Kind::Compute: {
			cp.ResetDeCe();
			cp.SetFlip({});
			if (!item.constant_commands.empty()) {
				RunStream(cp, item.constant_commands);
			}
			if (!item.commands.empty()) {
				RunStream(cp, item.commands);
			}
			break;
		}
	}
	if (m_pool != nullptr && plan_shadow) {
		// KYTY_LOOKAHEAD_REPLAY: decode the stream again and let the workers run its tasks with the
		// plans thrown away, timing how fast they get through them.
		static const uint32_t replays = [] {
			const char* text = std::getenv("KYTY_LOOKAHEAD_REPLAY");
			return text != nullptr ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) : 0u;
		}();
		if (replays > 1 && item.kind != Kind::Reset && !m_current_late) {
			m_pool->Drain();
			const auto tasks_before = m_stats.tasks.load(std::memory_order_relaxed);
			const auto t_replay     = Bench::NowNs();
			m_replaying             = true;
			for (uint32_t r = 1; r < replays && !Common::Speculation::Aborted(); ++r) {
				cp.SetPlanSequence(item.seq);
				cp.ResetDeCe();
				if (!item.constant_commands.empty()) RunStream(cp, item.constant_commands);
				if (!item.commands.empty()) RunStream(cp, item.commands);
			}
			m_pool->Drain();
			m_replaying = false;
			m_stats.replay_tasks.fetch_add(m_stats.tasks.load(std::memory_order_relaxed) - tasks_before,
			                               std::memory_order_relaxed);
			m_stats.replay_ns.fetch_add(Bench::NowNs() - t_replay, std::memory_order_relaxed);
		}
	}
	if (plan_shadow && m_pool == nullptr) {
		// Everything of this submission has been looked at (also when it was dropped as late).
		SrtPlan::GetStore().NoteProgress({item.seq, UINT32_MAX});
	} else if (plan_shadow) {
		// With workers the position moves when the submission's last task is collected.
		if (auto* end = m_pool->Acquire(); end != nullptr) {
			end->kind    = Pool::Task::Kind::EndItem;
			end->seq     = item.seq;
			end->discard = false;
			end->plans.clear();
			m_pool->Publish();
		}
		m_pool->Collect();
	}
	Common::Speculation::ClearItemWatch();
	m_item_start_ns.store(0, std::memory_order_relaxed);
	m_stats.busy_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
}

namespace {
// --srt-plan-shadow: after the event's plans were stored, tell kyGpu's lookups how far the
// lookahead has got (so a missing plan can be told from one that is not there yet). With
// --lookahead-workers the position moves when the workers' results are collected instead.
struct PlanProgress {
	bool active = SrtPlan::Enabled();
	bool notify;
	explicit PlanProgress(bool notify_progress): notify(notify_progress) {
		if (active) SrtPlan::NoteProducerEvent();
	}
	~PlanProgress() {
		if (active && notify && SrtPlan::t_context.active) {
			SrtPlan::GetStore().NoteProgress({SrtPlan::t_context.seq, SrtPlan::t_context.ordinal});
		}
	}
};
} // namespace

void PipelineLookahead::OnDraw(CommandProcessor& cp, const ShadowDraw& draw) {
	PlanProgress plan_progress(m_pool == nullptr);
	m_stats.draws.fetch_add(1, std::memory_order_relaxed);
	SkipDeps::NoteLookaheadDraw(m_current_seq);
	if (m_current_late || Common::Speculation::ItemStale() ||
	    (Config::GetPipelineLookaheadKinds() & 1u) == 0) {
		return;
	}
	if (m_pool != nullptr) {
		const auto call_start = Bench::NowNs();
		if (auto* task = m_pool->Acquire(); task != nullptr) {
			task->kind          = Pool::Task::Kind::Draw;
			task->seq           = m_current_seq;
			task->ordinal       = SrtPlan::t_context.active ? SrtPlan::t_context.ordinal : 0;
			task->sub           = SrtPlan::t_context.active ? SrtPlan::t_context.sub : 0;
			task->queue_id      = m_current_queue;
			task->draw          = draw;
			task->ctx           = cp.GetCtx();
			task->ucfg          = cp.GetUcfg();
			task->sh            = cp.GetShCtx();
			task->plans.clear();
			task->discard = m_replaying;
			m_pool->Publish();
		}
		if ((m_stats.draws.load(std::memory_order_relaxed) & 15u) == 0) m_pool->Collect();
		m_stats.draw_ns.fetch_add(Bench::NowNs() - call_start, std::memory_order_relaxed);
		return;
	}
	Common::Hang::Phase phase("lookahead_draw", m_stats.draws.load(std::memory_order_relaxed));
	RenderExecutor::PrefetchDrawStatus status {};
	const auto                         call_start = Bench::NowNs();
	(void)m_renderer.GetRenderExecutor().PrefetchDraw(cp.GetCtx(), cp.GetShCtx(), cp.GetUcfg(),
	                                                  draw, status);
	const auto call_ns = Bench::NowNs() - call_start;
	m_stats.draw_ns.fetch_add(call_ns, std::memory_order_relaxed);
	SkipDeps::NoteLookaheadCall("draw", call_start, call_ns);
}

void PipelineLookahead::OnDispatch(CommandProcessor& cp, const ShadowDispatch& dispatch) {
	PlanProgress plan_progress(m_pool == nullptr);
	m_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
	if (m_current_late || Common::Speculation::ItemStale() ||
	    (Config::GetPipelineLookaheadKinds() & 2u) == 0) {
		return; // already executing or executed: nothing to gain
	}
	if (!dispatch.indirect && (dispatch.x == 0 || dispatch.y == 0 || dispatch.z == 0)) {
		return;
	}
	if (m_pool != nullptr) {
		const auto call_start = Bench::NowNs();
		if (auto* task = m_pool->Acquire(); task != nullptr) {
			task->kind          = Pool::Task::Kind::Dispatch;
			task->seq           = m_current_seq;
			task->ordinal       = SrtPlan::t_context.active ? SrtPlan::t_context.ordinal : 0;
			task->sub           = SrtPlan::t_context.active ? SrtPlan::t_context.sub : 0;
			task->queue_id      = m_current_queue;
			task->dispatch      = dispatch;
			task->async_compute = cp.IsAsyncComputeQueue();
			task->ctx           = cp.GetCtx();
			task->ucfg          = cp.GetUcfg();
			task->sh            = cp.GetShCtx();
			task->plans.clear();
			task->discard       = m_replaying;
			m_pool->Publish();
		}
		m_stats.dispatch_ns.fetch_add(Bench::NowNs() - call_start, std::memory_order_relaxed);
		return;
	}
	Common::Hang::Phase phase("lookahead_dispatch", m_stats.dispatches.load(std::memory_order_relaxed));
	const uint32_t groups[3] = {dispatch.indirect ? 0u : dispatch.x, dispatch.indirect ? 0u : dispatch.y,
	                            dispatch.indirect ? 0u : dispatch.z};
	const auto call_start = Bench::NowNs();
	m_renderer.GetPipelineCache().PrefetchCompute(cp.GetShCtx().GetCs(),
	                                              cp.GetCtx().GetShaderRegisters(), dispatch.mode,
	                                              cp.IsAsyncComputeQueue(), groups);
	const auto call_ns = Bench::NowNs() - call_start;
	m_stats.dispatch_ns.fetch_add(call_ns, std::memory_order_relaxed);
	SkipDeps::NoteLookaheadCall("dispatch", call_start, call_ns);
}

} // namespace Libs::Graphics
