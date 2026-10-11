#include "graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.h"

#include "common/benchInstrument.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

// See the header for provenance (KytyPlus AsyncPipelineCompiler, GPL-2.0, Kyty Copyright (c) 2021
// InoriRus).

namespace Libs::Graphics {

const char* AsyncModeName(Config::AsyncPipelineMode mode) {
	switch (mode) {
		case Config::AsyncPipelineMode::Off: return "off";
		case Config::AsyncPipelineMode::Stall: return "stall";
		case Config::AsyncPipelineMode::Skip: return "skip";
	}
	return "unknown";
}

AsyncJob::~AsyncJob() {
	if (m_rows.empty() || !Bench::TimingEnabled()) {
		return;
	}
	const auto queue_ns = m_start_ns >= m_submit_ns ? m_start_ns - m_submit_ns : 0;
	const auto tail     = fmt::format(";mode={};queue_ns={};wait_ns={};skipped={};prefetch={};claimed={}",
	                                  AsyncModeName(m_mode), queue_ns, WaitNs(), Skipped(),
	                                  IsPrefetched() ? 1 : 0,
	                                  m_claimed.load(std::memory_order_relaxed) ? 1 : 0);
	for (auto& row: m_rows) {
		row.detail += tail;
		Bench::LogTimingAs(row.kind, row.key_hash, row.t_start_ns, row.dur_ns, row.thread_id,
		                   false, row.detail);
	}
}

bool AsyncJob::Wait() {
	const auto t0 = Bench::NowNs();
	{
		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [this] { return IsFinished(); });
	}
	m_wait_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
	return GetState() == State::Done;
}

void AsyncJob::AddTimingRow(const char* kind, uint64_t key_hash, int64_t t_start_ns,
                            int64_t dur_ns, std::string detail) {
	m_rows.push_back({kind, key_hash, t_start_ns, dur_ns, Bench::ThreadId(), std::move(detail)});
}

void AsyncJob::Finish(State final_state) {
	{
		std::lock_guard lock(m_mutex);
		m_end_ns = Bench::NowNs();
		m_state.store(final_state, std::memory_order_release);
	}
	m_cv.notify_all();
}

AsyncPipelineCompiler::AsyncPipelineCompiler(Config::AsyncPipelineMode mode,
                                             uint32_t                  worker_count)
    : m_mode(mode), m_worker_count(std::max(1u, worker_count)) {
	m_workers.reserve(m_worker_count);
	for (uint32_t i = 0; i < m_worker_count; i++) {
		m_workers.emplace_back([this, i] { WorkerLoop(i); });
	}
}

AsyncPipelineCompiler::~AsyncPipelineCompiler() {
	Shutdown();
}

bool AsyncPipelineCompiler::Submit(std::shared_ptr<AsyncJob> job, bool urgent) {
	if (Bench::TraceEnabled()) {
		const char* kind = job->GetKind() == AsyncJob::Kind::Program    ? "prog"
		                   : job->GetKind() == AsyncJob::Kind::Graphics ? "gfx"
		                                                                : "cs";
		Bench::LogTiming("gpu_miss", job->KeyHash(), Bench::NowNs(), 0, true,
		                 fmt::format("job={};seq={};draw={};disp={}", kind, Bench::TraceSubmission(),
		                             Bench::TraceDrawCount(), Bench::TraceDispatchCount()));
	}
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping.load(std::memory_order_relaxed)) {
			return false;
		}
		job->m_submit_ns = Bench::NowNs();
		job->m_mode      = m_mode;
		if (urgent) {
			m_queue.push_front(std::move(job));
		} else {
			m_queue.push_back(std::move(job));
		}
		const auto depth = static_cast<uint64_t>(m_queue.size());
		auto       peak  = m_stats.peak_queue.load(std::memory_order_relaxed);
		while (depth > peak &&
		       !m_stats.peak_queue.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
		}
		m_stats.submitted.fetch_add(1, std::memory_order_relaxed);
	}
	m_signal.notify_one();
	return true;
}

void AsyncPipelineCompiler::Shutdown() {
	std::deque<std::shared_ptr<AsyncJob>> cancelled;
	std::vector<std::thread>              workers;
	{
		std::lock_guard lock(m_mutex);
		// Idempotent: a second call finds the queue and the worker list already moved out.
		m_stopping.store(true, std::memory_order_release);
		cancelled.swap(m_queue);
		workers.swap(m_workers);
	}
	m_signal.notify_all();
	for (auto& job: cancelled) {
		job->Cancel();
		job->Finish(AsyncJob::State::Cancelled);
		m_stats.cancelled.fetch_add(1, std::memory_order_relaxed);
	}
	cancelled.clear();
	for (auto& worker: workers) {
		if (worker.joinable()) {
			worker.join();
		}
	}
}

void AsyncPipelineCompiler::Promote(AsyncJob& job) {
	std::lock_guard lock(m_mutex);
	if (job.GetState() != AsyncJob::State::Queued || m_queue.empty() ||
	    m_queue.front().get() == &job) {
		return;
	}
	for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
		if (it->get() == &job) {
			auto moved = std::move(*it);
			m_queue.erase(it);
			m_queue.push_front(std::move(moved));
			return;
		}
	}
}

bool AsyncPipelineCompiler::WaitFor(AsyncJob& job) {
	if (job.GetState() == AsyncJob::State::Queued) {
		Promote(job);
	}
	const auto before = job.WaitNs();
	const bool ok     = job.Wait();
	m_stats.gpu_waits.fetch_add(1, std::memory_order_relaxed);
	m_stats.gpu_wait_ns.fetch_add(job.WaitNs() - before, std::memory_order_relaxed);
	return ok;
}

void AsyncPipelineCompiler::WorkerLoop(uint32_t index) {
#if defined(__linux__)
	{
		const auto name = fmt::format("kyty-pipe-{}", index);
		pthread_setname_np(pthread_self(), name.c_str());
	}
#else
	(void)index;
#endif
	for (;;) {
		std::shared_ptr<AsyncJob> job;
		{
			std::unique_lock lock(m_mutex);
			m_signal.wait(lock, [this] {
				return m_stopping.load(std::memory_order_relaxed) || !m_queue.empty();
			});
			if (m_stopping.load(std::memory_order_relaxed)) {
				// Shutdown() took the queue; nothing left for us.
				return;
			}
			job = std::move(m_queue.front());
			m_queue.pop_front();
			job->m_start_ns = Bench::NowNs();
			job->m_state.store(AsyncJob::State::Running, std::memory_order_release);
		}
		job->Execute();
		job->Finish(AsyncJob::State::Done);
		m_stats.completed.fetch_add(1, std::memory_order_relaxed);
	}
}

std::string AsyncPipelineCompiler::Summary() const {
	const auto skipped_program  = m_stats.skipped_program_draws.load(std::memory_order_relaxed);
	const auto skipped_pipeline = m_stats.skipped_pipeline_draws.load(std::memory_order_relaxed);
	return fmt::format(
	    "mode={};workers={};jobs_submitted={};jobs_completed={};jobs_cancelled={};peak_queue={};"
	    "gpu_waits={};gpu_wait_ns={};skipped_draws={};skipped_program_draws={};"
	    "skipped_pipeline_draws={}",
	    AsyncModeName(m_mode), m_worker_count, m_stats.submitted.load(), m_stats.completed.load(),
	    m_stats.cancelled.load(), m_stats.peak_queue.load(), m_stats.gpu_waits.load(),
	    m_stats.gpu_wait_ns.load(), skipped_program + skipped_pipeline, skipped_program,
	    skipped_pipeline);
}

void AsyncPipelineCompiler::LogSummary() {
	{
		std::lock_guard lock(m_mutex);
		if (m_summary_logged) {
			return;
		}
		m_summary_logged = true;
	}
	if (Bench::TimingEnabled()) {
		const auto now = Bench::NowNs();
		Bench::LogTiming("summary_async", 0, now, 0, false, Summary());
	}
}

} // namespace Libs::Graphics
