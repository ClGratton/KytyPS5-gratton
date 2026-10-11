#include "graphics/guest_gpu/pipelineLookahead.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/emulatorConfig.h"
#include "common/speculation.h"
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

PipelineLookahead::PipelineLookahead(RenderContext& renderer): m_renderer(renderer) {
	m_thread = std::thread([this] { Run(); });
}

PipelineLookahead::~PipelineLookahead() {
	Stop();
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
		m_queue.push_back(std::move(item));
		const auto depth = static_cast<uint64_t>(m_queue.size());
		auto       peak  = m_stats.max_queue.load(std::memory_order_relaxed);
		while (depth > peak &&
		       !m_stats.max_queue.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
		}
	}
	m_cv.notify_one();
}

void PipelineLookahead::Stop() {
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping) {
			return;
		}
		m_stopping = true;
		m_queue.clear();
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
	const auto summary = fmt::format(
	    "items={};items_late={};draws={};dispatches={};max_queue={};busy_ms={};aborted={}",
	    m_stats.items.load(), m_stats.items_late.load(), m_stats.draws.load(),
	    m_stats.dispatches.load(), m_stats.max_queue.load(), m_stats.busy_ns.load() / 1000000,
	    Common::Speculation::Aborted() ? 1 : 0);
	std::printf("Pipeline lookahead thread: %s\n", summary.c_str());
	if (Bench::TimingEnabled()) {
		Bench::LogTiming("summary_lookahead_thread", 0, Bench::NowNs(), 0, false, summary);
	}
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
			item = std::move(m_queue.front());
			m_queue.pop_front();
		}
		Process(item);
	}
	m_exited.store(true, std::memory_order_release);
}

void PipelineLookahead::Process(Item& item) {
	const auto t0 = Bench::NowNs();
	m_stats.items.fetch_add(1, std::memory_order_relaxed);
	m_current_seq  = item.seq;
	m_current_late = item.seq != 0 && item.seq <= m_started_seq.load(std::memory_order_acquire);
	if (m_current_late) {
		m_stats.items_late.fetch_add(1, std::memory_order_relaxed);
	}
	auto& cp = Shadow(item.queue_id);
	switch (item.kind) {
		case Kind::Reset: cp.Reset(); break;
		case Kind::Graphics:
		case Kind::Compute: {
			cp.ResetDeCe();
			cp.SetFlip({});
			if (!item.constant_commands.empty()) {
				Pm4Execution execution;
				for (int round = 0; round < 8; round++) {
					if (cp.Process(execution, item.constant_commands) == Pm4ProcessResult::Complete ||
					    !execution.MadeProgress()) {
						break;
					}
				}
			}
			if (!item.commands.empty()) {
				Pm4Execution execution;
				for (int round = 0; round < 8; round++) {
					if (cp.Process(execution, item.commands) == Pm4ProcessResult::Complete ||
					    !execution.MadeProgress()) {
						break;
					}
				}
			}
			break;
		}
	}
	m_stats.busy_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
}

void PipelineLookahead::OnDraw(CommandProcessor& cp, const ShadowDraw& draw) {
	m_stats.draws.fetch_add(1, std::memory_order_relaxed);
	if (m_current_late) {
		return;
	}
	RenderExecutor::PrefetchDrawStatus status {};
	(void)m_renderer.GetRenderExecutor().PrefetchDraw(cp.GetCtx(), cp.GetShCtx(), cp.GetUcfg(),
	                                                  draw, status);
}

void PipelineLookahead::OnDispatch(CommandProcessor& cp, const ShadowDispatch& dispatch) {
	m_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
	if (m_current_late) {
		return; // already executing or executed: nothing to gain
	}
	if (!dispatch.indirect && (dispatch.x == 0 || dispatch.y == 0 || dispatch.z == 0)) {
		return;
	}
	m_renderer.GetPipelineCache().PrefetchCompute(cp.GetShCtx().GetCs(),
	                                              cp.GetCtx().GetShaderRegisters(), dispatch.mode);
}

} // namespace Libs::Graphics
