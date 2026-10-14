#include "graphics/guest_gpu/pipelineLookahead.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/emulatorConfig.h"
#include "common/framePath.h"
#include "common/hangWatchdog.h"
#include "common/guestRetirement.h"
#include "common/speculation.h"
#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <chrono>
#include <fmt/format.h>
#include <cstdio>

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

PipelineLookahead::PipelineLookahead(RenderContext& renderer): m_renderer(renderer) {
	m_thread = std::thread([this] { Run(); });
	if (Common::Hang::Enabled()) {
		Common::Hang::RegisterProvider("pipeline_lookahead", [this] { return DebugState(); });
	}
}

PipelineLookahead::~PipelineLookahead() {
	Stop();
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
	    "items={};items_late={};draws={};dispatches={};max_queue={};busy_ms={};aborted={};"
	    "abandoned={};stale={};retire_signals={};dropped={}",
	    m_stats.items.load(), m_stats.items_late.load(), m_stats.draws.load(),
	    m_stats.dispatches.load(), m_stats.max_queue.load(), m_stats.busy_ns.load() / 1000000,
	    Common::Speculation::Aborted() ? 1 : 0, m_stats.items_abandoned.load(),
	    m_stats.items_stale.load(), Common::GuestRetirement::Global().SignalCalls(),
	    m_stats.items_dropped.load());
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
			m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
			if (m_stopping) {
				break;
			}
			m_fp_queue_depth = m_queue.size();
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
	Common::FramePath::Scope frame_path(Common::FramePath::Lookahead, item.seq);
	m_stats.items.fetch_add(1, std::memory_order_relaxed);
	m_current_seq  = item.seq;
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
	if (plan_shadow) {
		// Everything of this submission has been looked at (also when it was dropped as late).
		SrtPlan::GetStore().NoteProgress({item.seq, UINT32_MAX});
	}
	frame_path.SetB((m_current_late ? 1u : 0u) | static_cast<uint64_t>(item.kind) << 8 |
	                std::min<uint64_t>(m_fp_queue_depth, 0xffffu) << 16 |
	                static_cast<uint64_t>(ItemDwords(item)) << 32);
	Common::Speculation::ClearItemWatch();
	m_item_start_ns.store(0, std::memory_order_relaxed);
	m_stats.busy_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
}

namespace {
// --srt-plan-shadow: after the event's plans were stored, tell kyGpu's lookups how far the
// lookahead has got (so a missing plan can be told from one that is not there yet).
struct PlanProgress {
	bool active = SrtPlan::Enabled();
	PlanProgress() {
		if (active) SrtPlan::NoteProducerEvent();
	}
	~PlanProgress() {
		if (active && SrtPlan::t_context.active) {
			SrtPlan::GetStore().NoteProgress({SrtPlan::t_context.seq, SrtPlan::t_context.ordinal});
		}
	}
};
} // namespace

void PipelineLookahead::OnDraw(CommandProcessor& cp, const ShadowDraw& draw) {
	PlanProgress plan_progress;
	m_stats.draws.fetch_add(1, std::memory_order_relaxed);
	SkipDeps::NoteLookaheadDraw(m_current_seq);
	if (m_current_late || Common::Speculation::ItemStale() ||
	    (Config::GetPipelineLookaheadKinds() & 1u) == 0) {
		return;
	}
	Common::Hang::Phase phase("lookahead_draw", m_stats.draws.load(std::memory_order_relaxed));
	RenderExecutor::PrefetchDrawStatus status {};
	const auto                         call_start = Bench::NowNs();
	(void)m_renderer.GetRenderExecutor().PrefetchDraw(cp.GetCtx(), cp.GetShCtx(), cp.GetUcfg(),
	                                                  draw, status);
	SkipDeps::NoteLookaheadCall("draw", call_start, Bench::NowNs() - call_start);
}

void PipelineLookahead::OnDispatch(CommandProcessor& cp, const ShadowDispatch& dispatch) {
	PlanProgress plan_progress;
	m_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
	if (m_current_late || Common::Speculation::ItemStale() ||
	    (Config::GetPipelineLookaheadKinds() & 2u) == 0) {
		return; // already executing or executed: nothing to gain
	}
	if (!dispatch.indirect && (dispatch.x == 0 || dispatch.y == 0 || dispatch.z == 0)) {
		return;
	}
	Common::Hang::Phase phase("lookahead_dispatch", m_stats.dispatches.load(std::memory_order_relaxed));
	const uint32_t groups[3] = {dispatch.indirect ? 0u : dispatch.x, dispatch.indirect ? 0u : dispatch.y,
	                            dispatch.indirect ? 0u : dispatch.z};
	const auto call_start = Bench::NowNs();
	m_renderer.GetPipelineCache().PrefetchCompute(cp.GetShCtx().GetCs(),
	                                              cp.GetCtx().GetShaderRegisters(), dispatch.mode,
	                                              cp.IsAsyncComputeQueue(), groups);
	SkipDeps::NoteLookaheadCall("dispatch", call_start, Bench::NowNs() - call_start);
}

} // namespace Libs::Graphics
