#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_ASYNCPIPELINECOMPILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_ASYNCPIPELINECOMPILER_H_

// Worker pool for shader translation and Vulkan pipeline creation (--async-pipelines, shaders.md
// Decision 1 / C1a). Phase 2 of the shader-stutter design, see docs/phase2-notes.md.
//
// The worker-pool shape (fixed set of std::thread workers waiting on a condition variable, a FIFO
// queue, an in-flight set keyed by pipeline key for de-duplication, an explicit Shutdown() that
// joins the workers before the Vulkan device goes away) follows KytyPlus'
// AsyncPipelineCompiler (src/graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.*,
// https://github.com/KytyPlus), licensed GPL-2.0, originating from Kyty, Copyright (c) 2021
// InoriRus (see LICENSES/Kyty-MIT.txt). Differences: the pool is generic (a job is any
// AsyncJob), jobs are shared_ptr objects the GPU thread can wait on, de-duplication is a
// separate registry per key type, queued jobs are cancelled (not drained) at shutdown, and a
// job's timing rows are written when it retires so they can carry wait/skip counts.
//
// This header deliberately depends on nothing Vulkan-related so it can be unit-tested alone.

#include "common/emulatorConfig.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class AsyncPipelineCompiler;

const char* AsyncModeName(Config::AsyncPipelineMode mode);

// One unit of worker work. Created and captured on the GPU thread (every input by value),
// executed on a worker, waited on or polled by the GPU thread.
class AsyncJob {
public:
	enum class Kind { Program, Graphics, Compute, Optimize };
	enum class State { Queued, Running, Done, Cancelled };

	AsyncJob(Kind kind, uint64_t key_hash);
	virtual ~AsyncJob();
	AsyncJob(const AsyncJob&)            = delete;
	AsyncJob& operator=(const AsyncJob&) = delete;

	// Runs on a worker. Must publish its result and unregister itself from its in-flight registry
	// before returning (publish first, so "not registered" always implies "published").
	virtual void Execute() = 0;
	// Called instead of Execute() when the pool shuts down with the job still queued. Must
	// unregister the job from its in-flight registry.
	virtual void Cancel() {}

	// Blocks until the job finished. Returns true when it ran to completion and false when it
	// was cancelled. The time spent blocked is added to wait_ns().
	bool Wait();

	[[nodiscard]] State GetState() const { return m_state.load(std::memory_order_acquire); }
	[[nodiscard]] bool  IsFinished() const {
		const auto state = GetState();
		return state == State::Done || state == State::Cancelled;
	}
	[[nodiscard]] Kind     GetKind() const { return m_kind; }
	[[nodiscard]] uint64_t KeyHash() const { return m_key_hash; }
	[[nodiscard]] int64_t  WaitNs() const { return m_wait_ns.load(std::memory_order_relaxed); }
	[[nodiscard]] uint32_t Skipped() const { return m_skipped.load(std::memory_order_relaxed); }

	// --pipeline-lookahead. A job submitted by the lookahead thread is "prefetched"; the first
	// draw/dispatch that needs it "claims" it (prediction hit). ClaimByDraw() returns true only
	// for that first claim of a prefetched job.
	void SetPrefetched() { m_prefetched.store(true, std::memory_order_relaxed); }
	[[nodiscard]] bool IsPrefetched() const { return m_prefetched.load(std::memory_order_relaxed); }
	[[nodiscard]] bool ClaimByDraw() {
		return IsPrefetched() && !m_claimed.exchange(true, std::memory_order_relaxed);
	}
	void                   CountSkippedDraw() { m_skipped.fetch_add(1, std::memory_order_relaxed); }

	// --debug-hang-watchdog: one-line description (kind, state, queued/submitted, age).
	[[nodiscard]] std::string DebugString() const;

	// Timing rows recorded while the job runs (worker thread only, before Execute() returns).
	// They are written when the job is destroyed, after the GPU thread's waits and skips are
	// final, with `mode`, `queue_ns`, `wait_ns` and `skipped` appended to `detail`.
	void AddTimingRow(const char* kind, uint64_t key_hash, int64_t t_start_ns, int64_t dur_ns,
	                  std::string detail);

private:
	friend class AsyncPipelineCompiler;

	struct TimingRow {
		const char* kind;
		uint64_t    key_hash;
		int64_t     t_start_ns;
		int64_t     dur_ns;
		uint64_t    thread_id;
		std::string detail;
	};

	void Finish(State final_state);

	const Kind                m_kind;
	const uint64_t            m_key_hash;
	std::atomic<State>        m_state {State::Queued};
	std::mutex                m_mutex;
	std::condition_variable   m_cv;
	std::atomic<int64_t>      m_wait_ns {0};
	std::atomic<uint32_t>     m_skipped {0};
	std::atomic<bool>         m_prefetched {false};
	std::atomic<bool>         m_claimed {false};
	std::atomic<bool>         m_submitted {false}; // Submit() accepted it (debug only)
	bool                      m_low = false;       // SubmitLowPriority() job
	uint32_t                  m_priority = 0;      // low-queue order (ascending)
	bool                      m_idle_only = false; // low job that runs only while no normal job runs
	int64_t                   m_created_ns = 0;
	int64_t                   m_submit_ns = 0;
	int64_t                   m_start_ns  = 0;
	int64_t                   m_end_ns    = 0;
	Config::AsyncPipelineMode m_mode      = Config::AsyncPipelineMode::Off;
	std::vector<TimingRow>    m_rows;
};

// De-duplication table for in-flight jobs of one key type ("a second request for the same key
// waits on the same job").
//
// Ordering contract that makes it race-free without holding a lock across the capture:
//  * a job publishes its result first and calls Erase() afterwards, so a key that is neither
//    registered nor published has no job;
//  * only the (serialised) GPU thread creates jobs. The creator does Find(), then, when empty,
//    checks the published results, then Insert().
// All increments of AsyncJob::CountSkippedDraw() made through Find(count_skip) happen under the
// registry lock and therefore before the Erase() of the same job.
template <class Key, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
class InFlightRegistry {
public:
	std::shared_ptr<AsyncJob> Find(const Key& key, bool count_skip = false) {
		std::lock_guard lock(m_mutex);
		const auto      iter = m_jobs.find(key);
		if (iter == m_jobs.end()) {
			return nullptr;
		}
		if (count_skip) {
			iter->second->CountSkippedDraw();
		}
		return iter->second;
	}

	// Registers `job` unless a job is already registered for the key. Returns the registered job
	// and sets `inserted` accordingly.
	std::shared_ptr<AsyncJob> Insert(const Key& key, std::shared_ptr<AsyncJob> job,
	                                 bool& inserted) {
		std::lock_guard lock(m_mutex);
		const auto [iter, was_inserted] = m_jobs.try_emplace(key, std::move(job));
		inserted                        = was_inserted;
		return iter->second;
	}

	void Erase(const Key& key) {
		std::shared_ptr<AsyncJob> released;
		{
			std::lock_guard lock(m_mutex);
			const auto      iter = m_jobs.find(key);
			if (iter == m_jobs.end()) {
				return;
			}
			released = std::move(iter->second);
			m_jobs.erase(iter);
		}
		// `released` is dropped outside the lock: it may be the last reference.
	}

	void Clear() {
		std::unordered_map<Key, std::shared_ptr<AsyncJob>, Hash, Equal> released;
		{
			std::lock_guard lock(m_mutex);
			released.swap(m_jobs);
		}
	}

	[[nodiscard]] size_t Size() {
		std::lock_guard lock(m_mutex);
		return m_jobs.size();
	}

	// Debug (watchdog thread): never blocks, so it still works when a thread holds the lock.
	[[nodiscard]] std::string Describe(size_t max_jobs = 12) {
		std::unique_lock lock(m_mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return "registry locked";
		}
		std::string text = "size=" + std::to_string(m_jobs.size());
		size_t      shown = 0;
		for (const auto& [key, job]: m_jobs) {
			if (shown++ >= max_jobs) {
				text += " ...";
				break;
			}
			text += " {" + job->DebugString() + "}";
		}
		return text;
	}

private:
	std::mutex                                                      m_mutex;
	std::unordered_map<Key, std::shared_ptr<AsyncJob>, Hash, Equal> m_jobs;
};

struct AsyncStats {
	std::atomic<uint64_t> submitted {0};
	std::atomic<uint64_t> completed {0};
	std::atomic<uint64_t> cancelled {0};
	std::atomic<uint64_t> peak_queue {0};
	std::atomic<uint64_t> skipped_program_draws {0};
	std::atomic<uint64_t> skipped_pipeline_draws {0};
	std::atomic<uint64_t> gpu_waits {0};
	std::atomic<int64_t>  gpu_wait_ns {0};
	std::atomic<uint64_t> low_submitted {0};
	std::atomic<uint64_t> low_completed {0};
	std::atomic<uint64_t> low_cancelled {0};
	std::atomic<uint64_t> low_run_ns {0};
	std::atomic<uint64_t> low_promoted {0}; // a draw waited for a low-priority job: moved up
};

class AsyncPipelineCompiler {
public:
	AsyncPipelineCompiler(Config::AsyncPipelineMode mode, uint32_t worker_count);
	~AsyncPipelineCompiler();
	AsyncPipelineCompiler(const AsyncPipelineCompiler&)            = delete;
	AsyncPipelineCompiler& operator=(const AsyncPipelineCompiler&) = delete;

	// Queues `job`. Returns false (and does not queue it) once Shutdown() has begun. `urgent`
	// jobs (a draw is waiting on them) go to the front of the queue.
	bool Submit(std::shared_ptr<AsyncJob> job, bool urgent = false);
	// Queues background work nobody waits for (--pipelines-unoptimized-first: the optimized
	// rebuild of a pipeline that already runs unoptimized). Workers take it only when the normal
	// queue is empty, and at most LowPriorityLimit() of them run at once, so a burst of
	// background builds can neither delay a draw's job nor occupy the whole pool.
	// `idle_only` jobs (the load-time precompile) additionally start only while no normal job is
	// running or queued, so they cannot slow the compile a draw is waiting for (the driver's
	// compile throughput is shared: eight concurrent compiles double each one's time).
	// Background jobs run in ascending `priority` (FIFO among equals): the load-time precompile
	// passes the time its variant was first needed in the previous session.
	bool SubmitLowPriority(std::shared_ptr<AsyncJob> job, bool idle_only = false, uint32_t priority = 0);
	[[nodiscard]] uint32_t LowPriorityLimit() const { return m_low_limit; }

	// Stops accepting jobs, cancels the queued ones (Cancel() + Wait() returns false), lets the
	// running ones finish and joins the workers. Idempotent. Must be called before the Vulkan
	// device, the Config or the driver VkPipelineCache goes away.
	void Shutdown();

	[[nodiscard]] bool     Running() const { return !m_stopping.load(std::memory_order_acquire); }
	[[nodiscard]] Config::AsyncPipelineMode Mode() const { return m_mode; }
	[[nodiscard]] uint32_t WorkerCount() const { return m_worker_count; }
	[[nodiscard]] AsyncStats& Stats() { return m_stats; }

	// Waits for `job` on the calling (GPU) thread and accounts the blocked time. A job still in
	// the queue is moved to the front first (the nearest draw wins); a low-priority one becomes a
	// normal job.
	bool WaitFor(AsyncJob& job);

	// One-line summary (also written as a summary_async timing row when timing is on).
	[[nodiscard]] std::string Summary() const;
	void                      LogSummary();
	// --debug-hang-watchdog: queue depth, queued/running jobs. Never blocks.
	[[nodiscard]] std::string DebugState();

private:
	void WorkerLoop(uint32_t index);
	void ProgramLoop(uint32_t index);
	void Promote(AsyncJob& job);

	const Config::AsyncPipelineMode m_mode;
	const uint32_t                  m_worker_count;
	AsyncStats                      m_stats;

	std::mutex                             m_mutex;
	std::condition_variable                m_signal;
	std::deque<std::shared_ptr<AsyncJob>>  m_queue;
	// SubmitLowPriority(): served only while m_queue is empty and fewer than m_low_limit of them
	// run (m_low_running, guarded by m_mutex).
	std::deque<std::shared_ptr<AsyncJob>>  m_low_queue;
	const uint32_t                         m_low_limit;
	uint32_t                               m_low_running = 0;
	uint32_t                               m_normal_running = 0; // normal jobs executing (m_mutex)
	std::vector<std::thread>               m_workers;
	// Program lane: SPIR-V translation jobs (milliseconds, no driver compile) get their own small
	// set of workers and queue. In the shared queue a translation sat behind the pool's long driver
	// compiles (seconds each, all workers busy), and whoever waited for it (the look-ahead worker,
	// or the GPU thread) waited for a compile to finish first. KYTY_PROGRAM_LANE=<n> sets the
	// worker count (default 2, 0 = translations share the main queue as before).
	std::deque<std::shared_ptr<AsyncJob>>  m_program_queue;
	std::condition_variable                m_program_signal;
	std::vector<std::thread>               m_program_workers;
	uint32_t                               m_program_lane = 0;
	std::atomic<bool>                      m_stopping {false};
	bool                                   m_summary_logged = false;
	// What each worker is running (debug only): job kind (+1, 0 = idle), key hash, start time.
	struct WorkerState {
		std::atomic<int>      kind {0};
		std::atomic<uint64_t> key {0};
		std::atomic<int64_t>  start_ns {0};
		std::atomic<bool>     prefetched {false};
	};
	std::unique_ptr<WorkerState[]>         m_worker_state;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_ASYNCPIPELINECOMPILER_H_
