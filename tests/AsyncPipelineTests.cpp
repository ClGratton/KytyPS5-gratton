// Unit tests for the --async-pipelines worker pool, the job life cycle and the in-flight
// de-duplication registry (src/graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.*).
// No Vulkan: the real shader/pipeline jobs are covered by the shader_recompiler_compute tests run
// with KYTY_TEST_ASYNC_PIPELINES=stall|skip (see CMakeLists.txt).

#include "graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace Libs::Graphics;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AsyncPipelineTests: failed: %s\n", message);
		std::abort();
	}
}

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

// A job that runs a std::function and optionally unregisters itself, like the real jobs.
class TestJob final: public AsyncJob {
public:
	using Registry = InFlightRegistry<int>;

	TestJob(Registry* registry, int key, std::function<void()> body)
	    : AsyncJob(Kind::Program, static_cast<uint64_t>(key)), m_registry(registry), m_key(key),
	      m_body(std::move(body)) {}

	void Execute() override {
		ran.fetch_add(1);
		m_thread = std::this_thread::get_id();
		if (m_body) {
			m_body();
		}
		if (m_registry != nullptr) {
			m_registry->Erase(m_key); // publish (in m_body) first, unregister last
		}
	}
	void Cancel() override {
		cancelled.fetch_add(1);
		if (m_registry != nullptr) {
			m_registry->Erase(m_key);
		}
	}

	std::atomic<int> ran {0};
	std::atomic<int> cancelled {0};
	[[nodiscard]] std::thread::id Thread() const { return m_thread; }

private:
	Registry*             m_registry;
	int                   m_key;
	std::function<void()> m_body;
	std::thread::id       m_thread;
};

void TestJobsRunOnWorkersConcurrently() {
	constexpr int workers = 4;
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, workers);
	Check(pool.WorkerCount() == workers, "worker count");
	std::atomic<int> started {0};
	std::vector<std::shared_ptr<TestJob>> jobs;
	for (int i = 0; i < workers; i++) {
		jobs.push_back(std::make_shared<TestJob>(nullptr, i, [&] {
			started.fetch_add(1);
			// Every job waits for all of them: only passes if `workers` threads run at once.
			Check(WaitUntil([&] { return started.load() == workers; }),
			      "jobs did not run concurrently");
		}));
		Check(pool.Submit(jobs.back()), "submit");
	}
	for (auto& job: jobs) {
		Check(pool.WaitFor(*job), "job completed");
		Check(job->GetState() == AsyncJob::State::Done, "state done");
		Check(job->ran.load() == 1, "ran once");
		Check(job->Thread() != std::this_thread::get_id(), "ran on a worker thread");
	}
	pool.Shutdown();
}

void TestDedupeSharesOneJob() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 4);
	TestJob::Registry     registry;
	std::atomic<bool>     release {false};
	std::atomic<int>      published {0};

	Check(registry.Find(7) == nullptr, "empty registry");
	auto first = std::make_shared<TestJob>(&registry, 7, [&] {
		Check(WaitUntil([&] { return release.load(); }), "release");
		published.fetch_add(1);
	});
	bool inserted = false;
	auto registered = registry.Insert(7, first, inserted);
	Check(inserted && registered == first, "first insert registers the job");
	Check(pool.Submit(first), "submit");

	// A second request for the same key joins the same job and never creates another.
	auto duplicate = std::make_shared<TestJob>(&registry, 7, [&] { published.fetch_add(1); });
	auto joined    = registry.Insert(7, duplicate, inserted);
	Check(!inserted && joined == first, "second insert joins the first job");
	Check(registry.Find(7, /*count_skip=*/true) == first, "find returns the in-flight job");
	Check(registry.Find(7, true) == first, "find returns the in-flight job again");
	Check(first->Skipped() == 2, "skipped draws are counted on the shared job");
	Check(registry.Find(8) == nullptr, "other keys are unaffected");

	// Eight "draws" wait on the same job; all wake up, the body ran exactly once.
	std::vector<std::thread> waiters;
	std::atomic<int>         woke {0};
	for (int i = 0; i < 8; i++) {
		waiters.emplace_back([&] {
			auto job = registry.Find(7);
			if (job == nullptr) {
				woke.fetch_add(1); // already published and unregistered
				return;
			}
			if (pool.WaitFor(*job)) {
				woke.fetch_add(1);
			}
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	release.store(true);
	for (auto& waiter: waiters) {
		waiter.join();
	}
	Check(woke.load() == 8, "all waiters were released");
	Check(first->ran.load() == 1 && duplicate->ran.load() == 0, "executed exactly once");
	Check(published.load() == 1, "published exactly once");
	Check(registry.Find(7) == nullptr, "job unregistered after it finished");
	Check(first->WaitNs() >= 0, "wait time recorded");
	pool.Shutdown();
}

// The ordering contract of the registry, hammered: a creator thread (the "GPU thread") needs a
// result for each key; workers publish into a shared_mutex-protected map and unregister last.
// The creator must never find a key that is neither registered nor published, must never lose a
// result, and no key may be built twice.
void TestPublishThenUnregisterContract() {
	constexpr int keys = 400;
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 8);
	TestJob::Registry     registry;
	std::shared_mutex     map_mutex;
	std::map<int, int>    published_map;
	std::atomic<int>      builds {0};
	std::atomic<bool>     stop_readers {false};

	const auto find_published = [&](int key) -> const int* {
		std::shared_lock lock(map_mutex);
		const auto       iter = published_map.find(key);
		return iter != published_map.end() ? &iter->second : nullptr;
	};

	// Concurrent readers, like draws touching the map while workers publish.
	std::vector<std::thread> readers;
	for (int r = 0; r < 3; r++) {
		readers.emplace_back([&] {
			uint64_t sum = 0;
			while (!stop_readers.load()) {
				for (int key = 0; key < keys; key += 7) {
					if (const auto* value = find_published(key)) {
						Check(*value == key * 3, "reader saw a torn value");
						sum += static_cast<uint64_t>(*value);
					}
				}
			}
			(void)sum;
		});
	}

	std::vector<std::shared_ptr<AsyncJob>> keep_alive;
	for (int round = 0; round < 2; round++) {
		for (int key = 0; key < keys; key++) {
			// Same sequence as PipelineCache::GetGraphicsPipeline in stall mode.
			if (find_published(key) != nullptr) {
				continue; // hit
			}
			auto job = registry.Find(key);
			if (job == nullptr) {
				if (find_published(key) != nullptr) {
					continue;
				}
				auto fresh = std::make_shared<TestJob>(&registry, key, [&, key] {
					builds.fetch_add(1);
					std::this_thread::yield();
					std::unique_lock lock(map_mutex);
					published_map.emplace(key, key * 3);
				});
				bool inserted = false;
				job           = registry.Insert(key, fresh, inserted);
				Check(inserted, "only the creator inserts");
				Check(pool.Submit(fresh), "submit");
			}
			Check(pool.WaitFor(*job), "job completed");
			const auto* value = find_published(key);
			Check(value != nullptr && *value == key * 3, "published result missing after wait");
		}
	}
	stop_readers.store(true);
	for (auto& reader: readers) {
		reader.join();
	}
	Check(builds.load() == keys, "each key built exactly once");
	Check(published_map.size() == static_cast<size_t>(keys), "all keys published");
	pool.Shutdown();
}

// Many submitters, many workers, nothing lost.
void TestManyJobsAllRunOnce() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Skip, 8);
	constexpr int         count = 2000;
	std::atomic<int>      ran {0};
	std::set<std::thread::id> threads;
	std::mutex                threads_mutex;
	std::vector<std::shared_ptr<TestJob>> jobs;
	for (int i = 0; i < count; i++) {
		jobs.push_back(std::make_shared<TestJob>(nullptr, i, [&] {
			ran.fetch_add(1);
			std::lock_guard lock(threads_mutex);
			threads.insert(std::this_thread::get_id());
		}));
		Check(pool.Submit(jobs.back()), "submit");
	}
	for (auto& job: jobs) {
		Check(pool.WaitFor(*job), "job completed");
		Check(job->ran.load() == 1, "ran once");
	}
	Check(ran.load() == count, "all jobs ran");
	Check(threads.size() > 1, "work was spread over several workers");
	Check(pool.Stats().submitted.load() == count && pool.Stats().completed.load() == count,
	      "stats");
	pool.Shutdown();
}

void TestShutdownCancelsQueuedAndJoinsRunning() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Skip, 1);
	TestJob::Registry     registry;
	std::atomic<bool>     release {false};
	std::atomic<bool>     running {false};

	auto blocker = std::make_shared<TestJob>(&registry, 1, [&] {
		running.store(true);
		Check(WaitUntil([&] { return release.load(); }), "release");
	});
	bool inserted = false;
	registry.Insert(1, blocker, inserted);
	Check(pool.Submit(blocker), "submit blocker");
	Check(WaitUntil([&] { return running.load(); }), "blocker started");

	std::vector<std::shared_ptr<TestJob>> queued;
	for (int key = 2; key <= 4; key++) {
		queued.push_back(std::make_shared<TestJob>(&registry, key, nullptr));
		registry.Insert(key, queued.back(), inserted);
		Check(pool.Submit(queued.back()), "submit queued");
	}
	Check(registry.Size() == 4, "four jobs registered");

	// Shutdown blocks on the running job; the queued ones are cancelled right away.
	std::atomic<bool> shutdown_done {false};
	std::thread       stopper([&] {
        pool.Shutdown();
        shutdown_done.store(true);
    });
	for (auto& job: queued) {
		Check(!pool.WaitFor(*job), "queued job was cancelled (Wait returns false)");
		Check(job->GetState() == AsyncJob::State::Cancelled, "state cancelled");
		Check(job->cancelled.load() == 1 && job->ran.load() == 0, "cancelled, never executed");
	}
	Check(registry.Size() == 1, "cancelled jobs unregistered themselves");
	Check(!pool.Running(), "pool stopped");
	Check(!shutdown_done.load(), "shutdown waits for the running job");
	release.store(true);
	stopper.join();
	Check(shutdown_done.load(), "shutdown returned");
	Check(blocker->GetState() == AsyncJob::State::Done && blocker->ran.load() == 1,
	      "running job completed");
	Check(registry.Size() == 0, "registry empty");

	auto late = std::make_shared<TestJob>(nullptr, 9, nullptr);
	Check(!pool.Submit(late), "submit after shutdown is refused");
	pool.Shutdown(); // idempotent
	Check(pool.Stats().cancelled.load() == 3, "cancel count");
	Check(!pool.Summary().empty(), "summary");
}

// --pipeline-lookahead: an urgent submit goes in front of the queue, and WaitFor() promotes a job
// that is still queued ("the nearest draw wins"). One worker, so the order is observable.
void TestUrgentSubmitAndPromote() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 1);
	std::atomic<bool>     release {false};
	std::atomic<bool>     running {false};
	std::mutex            order_mutex;
	std::vector<int>      order;
	const auto            record = [&](int id) {
        return [&, id] {
            std::lock_guard lock(order_mutex);
            order.push_back(id);
        };
	};

	auto blocker = std::make_shared<TestJob>(nullptr, 0, [&] {
		running.store(true);
		Check(WaitUntil([&] { return release.load(); }), "release");
	});
	Check(pool.Submit(blocker), "submit blocker");
	Check(WaitUntil([&] { return running.load(); }), "blocker started");

	auto a = std::make_shared<TestJob>(nullptr, 1, record(1));
	auto b = std::make_shared<TestJob>(nullptr, 2, record(2));
	auto c = std::make_shared<TestJob>(nullptr, 3, record(3));
	auto d = std::make_shared<TestJob>(nullptr, 4, record(4));
	Check(pool.Submit(a) && pool.Submit(b) && pool.Submit(c), "submit queued");
	Check(pool.Submit(d, true), "urgent submit");

	// c is still queued: WaitFor() moves it in front of a and b (but behind urgent d).
	std::atomic<bool> waited {false};
	std::thread       waiter([&] {
        Check(pool.WaitFor(*c), "c completes");
        waited.store(true);
    });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	release.store(true);
	waiter.join();
	Check(WaitUntil([&] { return a->IsFinished() && b->IsFinished() && d->IsFinished(); }),
	      "all finished");
	std::lock_guard lock(order_mutex);
	Check(order.size() == 4, "four jobs ran");
	Check(order[0] == 3 && order[1] == 4 && order[2] == 1 && order[3] == 2,
	      "promoted job first, then the urgent one, then FIFO");
	pool.Shutdown();

	auto prefetched = std::make_shared<TestJob>(nullptr, 5, nullptr);
	Check(!prefetched->IsPrefetched() && !prefetched->ClaimByDraw(), "not prefetched");
	prefetched->SetPrefetched();
	Check(prefetched->IsPrefetched() && prefetched->ClaimByDraw(), "first claim of a prefetch");
	Check(!prefetched->ClaimByDraw(), "second claim is not a hit");
}

} // namespace

// A job registered in the in-flight registry, found by the GPU thread and then rejected by
// Submit() (pool shut down in between) must release its waiter instead of leaving it blocked
// forever (the lookahead's "registered but never submitted" window).
void TestRejectedSubmitReleasesWaiter() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 1);
	TestJob::Registry     registry;
	auto                  job = std::make_shared<TestJob>(&registry, 5, nullptr);
	bool                  inserted = false;
	registry.Insert(5, job, inserted);
	Check(inserted, "registered");
	std::atomic<int> waiter_result {-1};
	auto             found = registry.Find(5);
	Check(found != nullptr, "waiter finds the registered job");
	std::thread waiter([&] { waiter_result.store(pool.WaitFor(*found) ? 1 : 0); });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Check(waiter_result.load() == -1, "waiter blocks on the not yet submitted job");
	pool.Shutdown();
	Check(!pool.Submit(job), "submit after shutdown is refused");
	Check(WaitUntil([&] { return waiter_result.load() != -1; }), "waiter released by the refusal");
	waiter.join();
	Check(waiter_result.load() == 0, "waiter sees a cancelled job");
	Check(registry.Size() == 0, "refused job unregistered");
}

// Prefetch accounting (--pipeline-lookahead): only a prefetched job can be claimed, and only the
// first claim of it counts as a prediction hit.
void TestPrefetchClaimCountsOnce() {
	TestJob plain(nullptr, 1, nullptr);
	Check(!plain.IsPrefetched() && !plain.ClaimByDraw(), "a plain job is never a hit");
	TestJob prefetched(nullptr, 2, nullptr);
	prefetched.SetPrefetched();
	Check(prefetched.IsPrefetched(), "prefetched flag");
	Check(prefetched.ClaimByDraw(), "first claim is a hit");
	Check(!prefetched.ClaimByDraw() && !prefetched.ClaimByDraw(), "later claims are not");
}

// Many threads race to register the same key: exactly one wins, everyone sees the winner's job,
// and the job runs once (the lookahead thread and the GPU thread do this for the same pipeline).
void TestConcurrentInsertSameKeyRunsOnce() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 4);
	TestJob::Registry     registry;
	std::atomic<int>      winners {0};
	std::atomic<int>      go {0};
	std::atomic<int>      attempts {0};
	std::vector<std::shared_ptr<AsyncJob>> seen(8);
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++) {
		threads.emplace_back([&, t] {
			// The job stays registered until every thread has tried to register its own, so the
			// outcome does not depend on how fast the pool runs it.
			auto mine = std::make_shared<TestJob>(&registry, 42, [&] {
				Check(WaitUntil([&] { return attempts.load() == 8; }), "all threads tried");
			});
			while (go.load() == 0) {
				std::this_thread::yield();
			}
			bool inserted = false;
			seen[static_cast<size_t>(t)] = registry.Insert(42, mine, inserted);
			attempts.fetch_add(1);
			if (inserted) {
				winners.fetch_add(1);
				Check(pool.Submit(mine), "winner submits");
			}
		});
	}
	go.store(1);
	for (auto& thread: threads) {
		thread.join();
	}
	Check(winners.load() == 1, "exactly one registration wins");
	for (const auto& job: seen) {
		Check(job == seen[0], "every thread sees the same registered job");
	}
	Check(pool.WaitFor(*seen[0]), "the winner ran");
	Check(static_cast<TestJob&>(*seen[0]).ran.load() == 1, "the shared job ran once");
	Check(registry.Size() == 0, "unregistered after publishing");
	pool.Shutdown();
}

// WaitFor() on a job that already finished returns at once and is accounted as a GPU wait.
void TestWaitForFinishedJobAndStats() {
	AsyncPipelineCompiler pool(Config::AsyncPipelineMode::Stall, 1);
	auto job = std::make_shared<TestJob>(nullptr, 3, [] {});
	Check(pool.Submit(job), "submit");
	Check(WaitUntil([&] { return job->IsFinished(); }), "job finished");
	const auto before = pool.Stats().gpu_waits.load();
	Check(pool.WaitFor(*job), "wait on a finished job succeeds");
	Check(pool.Stats().gpu_waits.load() == before + 1, "the wait is counted");
	Check(pool.Stats().submitted.load() == 1 && pool.Stats().completed.load() == 1,
	      "submitted and completed counters");
	// A cancelled job's Cancel() runs exactly once even if Shutdown() is called twice.
	AsyncPipelineCompiler second(Config::AsyncPipelineMode::Stall, 1);
	std::atomic<bool> release {false};
	auto blocker = std::make_shared<TestJob>(nullptr, 4, [&] {
		Check(WaitUntil([&] { return release.load(); }), "release blocker");
	});
	auto queued = std::make_shared<TestJob>(nullptr, 5, [] {});
	Check(second.Submit(blocker) && second.Submit(queued), "submit both");
	Check(WaitUntil([&] { return blocker->GetState() == AsyncJob::State::Running; }),
	      "blocker running");
	std::thread stopper([&] { second.Shutdown(); });
	Check(WaitUntil([&] { return queued->IsFinished(); }), "queued job cancelled");
	second.Shutdown();
	release.store(true);
	stopper.join();
	Check(queued->cancelled.load() == 1 && queued->ran.load() == 0, "Cancel() ran once");
}

int main() {
	TestJobsRunOnWorkersConcurrently();
	TestDedupeSharesOneJob();
	TestPublishThenUnregisterContract();
	TestManyJobsAllRunOnce();
	TestShutdownCancelsQueuedAndJoinsRunning();
	TestUrgentSubmitAndPromote();
	TestRejectedSubmitReleasesWaiter();
	TestPrefetchClaimCountsOnce();
	TestConcurrentInsertSameKeyRunsOnce();
	TestWaitForFinishedJobAndStats();
	std::printf("AsyncPipelineTests: all cases passed\n");
	return 0;
}
