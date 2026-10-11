#include "graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.h"

#include "common/benchInstrument.h"
#include "common/gpuPhaseStats.h"
#include "common/hangWatchdog.h"

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

AsyncJob::AsyncJob(Kind kind, uint64_t key_hash)
    : m_kind(kind), m_key_hash(key_hash), m_created_ns(Bench::NowNs()) {}

namespace {
const char* KindName(AsyncJob::Kind kind) {
	switch (kind) {
		case AsyncJob::Kind::Program: return "prog";
		case AsyncJob::Kind::Graphics: return "gfx";
		case AsyncJob::Kind::Compute: return "cs";
	}
	return "?";
}
const char* StateName(AsyncJob::State state) {
	switch (state) {
		case AsyncJob::State::Queued: return "queued";
		case AsyncJob::State::Running: return "running";
		case AsyncJob::State::Done: return "done";
		case AsyncJob::State::Cancelled: return "cancelled";
	}
	return "?";
}
} // namespace

std::string AsyncJob::DebugString() const {
	return fmt::format("{} {} key={:x} submitted={} prefetched={} age_ms={}", KindName(m_kind),
	                   StateName(GetState()), m_key_hash, m_submitted.load() ? 1 : 0,
	                   IsPrefetched() ? 1 : 0, (Bench::NowNs() - m_created_ns) / 1000000);
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
	Common::Hang::Phase phase("job_wait", static_cast<uint64_t>(m_kind), m_key_hash);
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
    : m_mode(mode), m_worker_count(std::max(1u, worker_count)),
      m_worker_state(std::make_unique<WorkerState[]>(std::max(1u, worker_count))) {
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
		std::unique_lock lock(m_mutex);
		if (m_stopping.load(std::memory_order_relaxed)) {
			lock.unlock();
			// Rejected: whoever already found the job in a registry (the GPU thread waiting on a
			// request the lookahead registered) must be released, not left waiting forever.
			job->Cancel();
			job->Finish(AsyncJob::State::Cancelled);
			return false;
		}
		job->m_submit_ns = Bench::NowNs();
		job->m_mode      = m_mode;
		job->m_submitted.store(true, std::memory_order_relaxed);
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
	Common::GpuPhase::Scope gpu_phase_job(Common::GpuPhase::Phase::PipelineJob);
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
		{
			auto& state = m_worker_state[index];
			state.key.store(job->KeyHash(), std::memory_order_relaxed);
			state.prefetched.store(job->IsPrefetched(), std::memory_order_relaxed);
			state.start_ns.store(Bench::NowNs(), std::memory_order_relaxed);
			state.kind.store(static_cast<int>(job->GetKind()) + 1, std::memory_order_release);
		}
		{
			Common::Hang::Phase phase("worker_job", static_cast<uint64_t>(job->GetKind()),
			                          job->KeyHash());
			job->Execute();
		}
		m_worker_state[index].kind.store(0, std::memory_order_release);
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

namespace Libs::Graphics {

std::string AsyncPipelineCompiler::DebugState() {
	std::string text = fmt::format("mode={} workers={} submitted={} completed={} stopping={}",
	                               AsyncModeName(m_mode), m_worker_count, m_stats.submitted.load(),
	                               m_stats.completed.load(), m_stopping.load() ? 1 : 0);
	{
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			text += " queue=<pool mutex locked>";
		} else {
			text += fmt::format(" queue={}", m_queue.size());
			size_t shown = 0;
			for (const auto& job: m_queue) {
				if (shown++ >= 8) {
					text += " ...";
					break;
				}
				text += " {" + job->DebugString() + "}";
			}
		}
	}
	const auto now = Bench::NowNs();
	for (uint32_t i = 0; i < m_worker_count; i++) {
		const auto kind = m_worker_state[i].kind.load(std::memory_order_acquire);
		if (kind == 0) {
			continue;
		}
		text += fmt::format(" | worker{} {} key={:x} prefetched={} running_ms={}", i,
		                    KindName(static_cast<AsyncJob::Kind>(kind - 1)),
		                    m_worker_state[i].key.load(), m_worker_state[i].prefetched.load() ? 1 : 0,
		                    (now - m_worker_state[i].start_ns.load()) / 1000000);
	}
	return text;
}

} // namespace Libs::Graphics
