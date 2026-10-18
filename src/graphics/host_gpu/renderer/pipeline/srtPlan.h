#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLAN_H_

// --srt-plan-shadow: per-draw SRT plans produced by the lookahead thread, compared with what the
// GPU thread (kyGpu) materializes for the same draw. Shadow only: nothing here changes what is
// drawn, kyGpu always materializes by itself and a plan is only looked at afterwards.
//
// The lookahead already runs MaterializeResources for the draws it predicts and drops the result.
// With the flag on it keeps it as a Plan: the ResourceSnapshot and ResourceSpecialization, the
// inputs they were computed from (user data, shader base, workgroup counts) and a ReadSet, the
// guest-memory dwords the walk read. A plan is stored under the key (submission, draw ordinal,
// Get() call index), which the shadow command processor and the real one derive identically
// (CommandProcessor counts draw/dispatch packets per submission).
//
// When kyGpu materializes the same draw it looks the plan up and records: whether it was ready,
// how long before its use it was produced (microseconds and draws), whether it would still be
// valid (inputs equal, ReadSet re-read and equal, no read of GPU-dirty memory), whether its bytes
// equal kyGpu's, and what the validity check costs. The guest memory has no page version counter
// (MemoryTracker keeps consume-on-upload dirty bits, not generations), so a ReadSet stores the
// exact dwords instead of a hash and the check re-reads and compares them.
//
// Pure logic and statistics live in this header (unit tested in tests/SrtPlanTests.cpp); the
// process-wide instance, the periodic and exit summaries are in srtPlan.cpp.

#include "graphics/host_gpu/renderer/pipeline/srtPlanContext.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::SrtPlan {

// ---- keys ---------------------------------------------------------------------------------------

// ---- read set -------------------------------------------------------------------------------

enum class ReadKind : uint8_t {
	Scalar, // plain backing read, unmapped memory reads as zeros (SRT scalar loads)
	Clean,  // specialization read: must not be GPU-dirty (kyGpu would drain and read it back)
};

// The guest dwords a materialization read, merged into windows that one memory read can fetch.
// Runs are contiguous recorded dwords; a window bridges small gaps between runs (the gap bytes are
// read but never compared).
struct ReadSet {
	static constexpr uint32_t kMaxGapBytes    = 64;
	static constexpr uint32_t kMaxWindowBytes = 1024;
	static constexpr uint32_t kMaxRunDwords   = kMaxWindowBytes / 4;

	struct Run {
		uint32_t offset_bytes = 0; // from the window start
		uint32_t dwords       = 0;
		uint32_t value_offset = 0; // into `values`
	};
	struct Window {
		uint64_t address   = 0;
		uint32_t bytes     = 0; // multiple of 4
		uint32_t first_run = 0;
		uint32_t run_count = 0;
		ReadKind kind      = ReadKind::Scalar;
	};
	enum class Result : uint8_t { Valid, Changed, Unreadable, GpuDirty };

	std::vector<Window>   windows;
	std::vector<Run>      runs;
	std::vector<uint32_t> values;

	[[nodiscard]] size_t   RunCount() const { return runs.size(); }
	[[nodiscard]] size_t   DwordCount() const { return values.size(); }
	[[nodiscard]] size_t   Bytes() const {
        return windows.size() * sizeof(Window) + runs.size() * sizeof(Run) + values.size() * 4u;
	}

	// Re-reads the set and compares it with what was recorded.
	//   read(kind, address, std::span<uint32_t> out, bool exact) -> bool
	//     exact: a bridged window; fail instead of answering for unmapped memory. Not exact: the
	//     semantics of the original reader of that kind (scalar reads zero-fill unmapped memory).
	//   dirty(address, bytes) -> bool: the range (one run of a Clean window) holds GPU-written bytes the
	//     guest backing lacks and whose value the reader cannot supply otherwise.
	template <class Read, class Dirty>
	[[nodiscard]] Result Validate(Read&& read, Dirty&& dirty) const {
		std::array<uint32_t, kMaxWindowBytes / 4u> scratch;
		for (const auto& window: windows) {
			if (window.kind == ReadKind::Clean) {
				// Only the bytes the walk read matter: a bridged gap that holds GPU-written bytes is
				// not compared, so it cannot invalidate the plan.
				for (uint32_t i = 0; i < window.run_count; ++i) {
					const auto& run = runs[window.first_run + i];
					if (dirty(window.address + run.offset_bytes, run.dwords * 4u)) {
						return Result::GpuDirty;
					}
				}
			}
			const bool bridged = window.run_count > 1 || window.bytes != runs[window.first_run].dwords * 4u;
			bool       read_ok = read(window.kind, window.address,
                               std::span<uint32_t>(scratch.data(), window.bytes / 4u), bridged);
			if (read_ok) {
				for (uint32_t i = 0; i < window.run_count; ++i) {
					const auto& run = runs[window.first_run + i];
					if (std::memcmp(reinterpret_cast<const uint8_t*>(scratch.data()) + run.offset_bytes,
					                values.data() + run.value_offset, run.dwords * 4u) != 0) {
						return Result::Changed;
					}
				}
				continue;
			}
			// The bridged window crossed into unreadable memory: read the runs on their own.
			for (uint32_t i = 0; i < window.run_count; ++i) {
				const auto& run = runs[window.first_run + i];
				if (!read(window.kind, window.address + run.offset_bytes,
				          std::span<uint32_t>(scratch.data(), run.dwords), false)) {
					return Result::Unreadable;
				}
				if (std::memcmp(scratch.data(), values.data() + run.value_offset, run.dwords * 4u) != 0) {
					return Result::Changed;
				}
			}
		}
		return Result::Valid;
	}
};

class ReadRecorder {
public:
	void Record(ReadKind kind, uint64_t address, std::span<const uint32_t> values) {
		for (size_t i = 0; i < values.size(); ++i) {
			const auto order = static_cast<uint32_t>(m_entries.size());
			m_entries.push_back({address + i * 4u, values[i], kind == ReadKind::Clean ? (order | kCleanBit) : order});
		}
	}
	void   Clear() { m_entries.clear(); }
	size_t Size() const { return m_entries.size(); }

	// Sorts, drops repeated addresses (the first value read wins), merges contiguous dwords into
	// runs and nearby runs into windows. Leaves the recorder empty.
	ReadSet Finish() {
		ReadSet set;
		FinishInto(set);
		return set;
	}
	// The same into `set`, whose previous content is replaced. Its vectors are sized exactly (a plan
	// holds thousands of these, and each growth step of a push_back loop was a heap block freed on
	// another thread); a set that comes from the plan pool keeps the capacity it already has.
	void FinishInto(ReadSet& set) {
		set.windows.clear();
		set.runs.clear();
		set.values.clear();
		if (m_entries.empty()) return;
		// Scalar entries first, then Clean ones; inside a kind by address, and among reads of one
		// address in the order they were recorded (the first one wins below).
		std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
			if ((a.order & kCleanBit) != (b.order & kCleanBit)) return (a.order & kCleanBit) < (b.order & kCleanBit);
			return a.address != b.address ? a.address < b.address : a.order < b.order;
		});
		size_t unique = 0;
		for (size_t i = 0; i < m_entries.size(); ++i) {
			if (unique != 0 && m_entries[unique - 1].address == m_entries[i].address &&
			    (m_entries[unique - 1].order & kCleanBit) == (m_entries[i].order & kCleanBit)) {
				continue;
			}
			m_entries[unique++] = m_entries[i];
		}
		m_entries.resize(unique);
		set.values.reserve(unique);
		for (const auto& entry: m_entries) set.values.push_back(entry.value);
		BuildRuns(set);
		m_entries.clear();
	}

private:
	static constexpr uint32_t kCleanBit = 0x80000000u;

	// address and value of a recorded dword; `order` is its position in the recording and, in the top
	// bit, its ReadKind (Clean = set).
	struct Entry {
		uint64_t address = 0;
		uint32_t value   = 0;
		uint32_t order   = 0;
	};
	struct RawRun {
		uint64_t address;
		uint32_t dwords;
		uint32_t value_offset;
		bool     clean;
	};

	// m_entries is sorted and unique here, and set.values holds its values in the same order.
	void BuildRuns(ReadSet& set) {
		m_raw_runs.clear();
		for (size_t i = 0; i < m_entries.size();) {
			const bool clean = (m_entries[i].order & kCleanBit) != 0;
			size_t     j     = i + 1;
			while (j < m_entries.size() && (m_entries[j].order & kCleanBit) == (m_entries[i].order & kCleanBit) &&
			       m_entries[j].address == m_entries[j - 1].address + 4u && j - i < ReadSet::kMaxRunDwords) {
				++j;
			}
			m_raw_runs.push_back({m_entries[i].address, static_cast<uint32_t>(j - i), static_cast<uint32_t>(i), clean});
			i = j;
		}
		// Windows: runs of one kind whose gap is small enough share one memory read.
		m_windows.clear();
		m_out_runs.clear();
		size_t i = 0;
		while (i < m_raw_runs.size()) {
			const uint64_t start = m_raw_runs[i].address;
			uint64_t       end   = start + m_raw_runs[i].dwords * uint64_t {4};
			size_t         j     = i + 1;
			while (j < m_raw_runs.size() && m_raw_runs[j].clean == m_raw_runs[i].clean &&
			       m_raw_runs[j].address <= end + ReadSet::kMaxGapBytes &&
			       std::max(end, m_raw_runs[j].address + m_raw_runs[j].dwords * uint64_t {4}) - start <=
			           ReadSet::kMaxWindowBytes) {
				end = std::max(end, m_raw_runs[j].address + m_raw_runs[j].dwords * uint64_t {4});
				++j;
			}
			ReadSet::Window window;
			window.address   = start;
			window.bytes     = static_cast<uint32_t>((end - start + 3u) & ~uint64_t {3}); // whole dwords
			window.first_run = static_cast<uint32_t>(m_out_runs.size());
			window.run_count = static_cast<uint32_t>(j - i);
			window.kind      = m_raw_runs[i].clean ? ReadKind::Clean : ReadKind::Scalar;
			for (size_t k = i; k < j; ++k) {
				m_out_runs.push_back({static_cast<uint32_t>(m_raw_runs[k].address - start), m_raw_runs[k].dwords,
				                      m_raw_runs[k].value_offset});
			}
			m_windows.push_back(window);
			i = j;
		}
		set.windows.assign(m_windows.begin(), m_windows.end());
		set.runs.assign(m_out_runs.begin(), m_out_runs.end());
	}

	std::vector<Entry>            m_entries;
	std::vector<RawRun>           m_raw_runs;
	std::vector<ReadSet::Window>  m_windows;
	std::vector<ReadSet::Run>     m_out_runs;
};

// ---- plan ---------------------------------------------------------------------------------------

struct Plan {
	Key                                              key;
	uint32_t                                         stage       = 0;
	uint64_t                                         shader_hash = 0;
	uint64_t                                         shader_base = 0;
	ShaderRecompiler::IR::ResourceSnapshot           snapshot;
	ShaderRecompiler::IR::ResourceSpecialization    specialization;
	std::vector<uint32_t>                            workgroup_counts;
	ReadSet                                          reads;
	// Resolved draw: the source entry and the permutation the producer found for this specialization
	// (null: none yet), and the push-data cursor it searched with. Opaque here; the pipeline cache
	// owns both types. Entries and permutations are never freed or reordered, so the pointers stay
	// valid for the process lifetime.
	const void*                                      entry              = nullptr;
	const void*                                      permutation        = nullptr;
	uint32_t                                         permutation_cursor = 0;
	int64_t                                          produced_ns     = 0;
	uint64_t                                         produced_serial = 0;

	[[nodiscard]] size_t Bytes() const {
		const auto& s = snapshot;
		return sizeof(Plan) + (s.flattened_srt.size() + s.user_data.size() + workgroup_counts.size()) * 4u +
		       (s.buffers.size() + s.images.size() + s.samplers.size()) *
		           sizeof(ShaderRecompiler::IR::DescriptorValue) +
		       s.specialization_reads.size() * 16u + specialization.buffers.size() * 32u +
		       specialization.images.size() * 48u + reads.Bytes();
	}

	// Back to a default-constructed plan, keeping the capacity of the vectors a pool reuses. A vector
	// that grew past kPooledVectorBytes (one huge shader) is released instead of being kept forever.
	// Every member above is reset here: add the new one to this function too.
	static constexpr size_t kPooledVectorBytes = 16u << 10u;
	void ResetForReuse() {
		key         = {};
		stage       = 0;
		shader_hash = 0;
		shader_base = 0;
		snapshot.Reset();
		specialization.Reset();
		workgroup_counts.clear();
		reads.windows.clear();
		reads.runs.clear();
		reads.values.clear();
		entry              = nullptr;
		permutation        = nullptr;
		permutation_cursor = 0;
		produced_ns        = 0;
		produced_serial    = 0;
		ReleaseOversized(snapshot.buffers);
		ReleaseOversized(snapshot.images);
		ReleaseOversized(snapshot.samplers);
		ReleaseOversized(snapshot.flattened_srt);
		ReleaseOversized(snapshot.user_data);
		ReleaseOversized(snapshot.specialization_reads);
		ReleaseOversized(snapshot.read_hull.intervals);
		ReleaseOversized(specialization.buffers);
		ReleaseOversized(specialization.images);
		ReleaseOversized(workgroup_counts);
		ReleaseOversized(reads.windows);
		ReleaseOversized(reads.runs);
		ReleaseOversized(reads.values);
	}

private:
	template <class T>
	static void ReleaseOversized(std::vector<T>& v) {
		if (v.capacity() * sizeof(T) > kPooledVectorBytes) std::vector<T>().swap(v);
	}
};

// Whether finished plans are reused instead of freed (--plan-pool). srtPlan.cpp reads the config.
[[nodiscard]] bool PoolEnabled();

// Plans that kyGpu is done with, kept with the capacity of their vectors. A lookahead worker builds a
// plan out of a dozen heap blocks (descriptor vectors, user data, read set); without a pool each one
// was allocated on a worker, freed on the sequencer thread and taken from glibc's arenas through
// their locks. Plans come back in batches (Give, sequencer thread, from Store::Publish) and go out
// to the workers in batches of kTakeBatch per lock (Acquire keeps the rest in a thread-local list).
// Nothing about a plan's content survives: Acquire resets it (ResetForReuse).
class PlanPool {
public:
	static constexpr size_t kMaxPooled = 8192;
	static constexpr size_t kTakeBatch = 16;

	struct Counters {
		uint64_t acquired = 0, created = 0, given = 0, freed = 0;
	};

	// A reset plan: from the thread's list, else from the shared one, else new.
	[[nodiscard]] std::unique_ptr<Plan> Acquire() {
		auto& local = Local();
		if (local.empty()) {
			std::lock_guard lock(m_mutex);
			for (size_t i = 0; i < kTakeBatch && !m_free.empty(); ++i) {
				local.push_back(std::move(m_free.back()));
				m_free.pop_back();
			}
		}
		m_acquired.fetch_add(1, std::memory_order_relaxed);
		if (local.empty()) {
			m_created.fetch_add(1, std::memory_order_relaxed);
			return std::make_unique<Plan>();
		}
		auto plan = std::move(local.back());
		local.pop_back();
		// Reset by the thread that is about to fill it: the lines of the plan are fetched once, by that
		// core, and not first by the thread that gave it back (measured: resetting in Give made the
		// workers slower by about what the sequencer saved).
		plan->ResetForReuse();
		return plan;
	}
	// An unused plan the caller got from Acquire goes back to its thread's list.
	void Return(std::unique_ptr<Plan> plan) {
		auto& local = Local();
		if (local.size() < 2 * kTakeBatch) local.push_back(std::move(plan));
	}
	// Finished plans, untouched: they are reset when they are taken. What does not fit under the limit is freed.
	void Give(std::vector<std::unique_ptr<Plan>>& plans) {
		uint64_t given = 0;
		{
			std::lock_guard lock(m_mutex);
			for (auto& plan: plans) {
				if (!plan || m_free.size() >= kMaxPooled) continue;
				m_free.push_back(std::move(plan));
				++given;
			}
		}
		m_given.fetch_add(given, std::memory_order_relaxed);
		m_freed.fetch_add(plans.size() - given, std::memory_order_relaxed);
		plans.clear(); // frees the ones that did not fit
	}
	[[nodiscard]] Counters GetCounters() const {
		return {m_acquired.load(std::memory_order_relaxed), m_created.load(std::memory_order_relaxed),
		        m_given.load(std::memory_order_relaxed), m_freed.load(std::memory_order_relaxed)};
	}
	[[nodiscard]] size_t Pooled() {
		std::lock_guard lock(m_mutex);
		return m_free.size();
	}

private:
	static std::vector<std::unique_ptr<Plan>>& Local() {
		thread_local std::vector<std::unique_ptr<Plan>> list;
		return list;
	}

	std::mutex                         m_mutex;
	std::vector<std::unique_ptr<Plan>> m_free;
	std::atomic<uint64_t>              m_acquired {0}, m_created {0}, m_given {0}, m_freed {0};
};

[[nodiscard]] inline PlanPool& GetPlanPool() {
	static PlanPool pool;
	return pool;
}

// The inputs a materialization depends on besides guest memory.
struct Inputs {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base = 0;
	std::span<const uint32_t> workgroup_counts;
};

[[nodiscard]] inline bool InputsMatch(const Plan& plan, const Inputs& inputs) {
	const auto equal = [](std::span<const uint32_t> a, std::span<const uint32_t> b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
	};
	return plan.shader_base == inputs.shader_base &&
	       equal(plan.snapshot.user_data, inputs.user_data) &&
	       equal(plan.workgroup_counts, inputs.workgroup_counts);
}

enum DiffBits : uint32_t {
	DiffFlattened   = 1u << 0,
	DiffBuffers     = 1u << 1,
	DiffImages      = 1u << 2,
	DiffSamplers    = 1u << 3,
	DiffUserData    = 1u << 4,
	DiffReads       = 1u << 5,
	DiffFill        = 1u << 6,
	DiffSpecialization = 1u << 7,
};

// Which parts of two materializations differ (0 = byte-equal).
[[nodiscard]] inline uint32_t Diff(const ShaderRecompiler::IR::ResourceSnapshot& a,
                                   const ShaderRecompiler::IR::ResourceSpecialization& a_spec,
                                   const ShaderRecompiler::IR::ResourceSnapshot& b,
                                   const ShaderRecompiler::IR::ResourceSpecialization& b_spec) {
	uint32_t mask = 0;
	if (a.flattened_srt != b.flattened_srt) mask |= DiffFlattened;
	if (a.buffers != b.buffers) mask |= DiffBuffers;
	if (a.images != b.images) mask |= DiffImages;
	if (a.samplers != b.samplers) mask |= DiffSamplers;
	if (a.user_data != b.user_data) mask |= DiffUserData;
	if (a.specialization_reads != b.specialization_reads) mask |= DiffReads;
	if (!(a.uniform_fill == b.uniform_fill)) mask |= DiffFill;
	if (!(a_spec == b_spec)) mask |= DiffSpecialization;
	return mask;
}

// ---- store ----------------------------------------------------------------------------------

enum class Lookup : uint8_t {
	Ready,    // a plan was found
	NotReady, // the lookahead has not reached this draw yet
	Missing,  // it passed this draw without a plan (skipped, diverged, dropped)
	Late,     // it skipped the whole submission because kyGpu had already started it
	Evicted,  // a plan was made and fell out of the bounded queue unused
};

// Hand-off of plans from the lookahead thread (single producer) to kyGpu (single consumer), batched
// per submission. The producer collects plans in a private bucket and publishes it (one lock) every
// kPublishPlans plans, at the end of a submission and when it moves on to the next one. kyGpu detaches
// the published pieces of a submission on its first lookup of it (one lock) and then answers every
// lookup from that bucket without locks: plans are in key order, so the common case is the next
// entry. A lookup that finds no plan takes the lock again, to merge pieces published since and to say
// why there is none (not yet looked at, late, evicted, looked at without a plan). Submissions may be
// consumed in any order and be resumed.
//
// Thread contract: Put and NoteProgress from one thread (the lookahead); Take from one thread (kyGpu);
// NoteLate, Recycle and GetCounters from anywhere.
class Store {
public:
	Store(size_t max_plans, size_t max_bytes): m_max_plans(max_plans), m_max_bytes(max_bytes) {}

	// Lookahead thread.
	void Put(std::unique_ptr<Plan> plan) {
		const auto key = plan->key;
		if (m_open && m_open->seq != key.seq) Publish();
		if (!m_open) {
			m_open      = std::make_unique<Bucket>();
			m_open->seq = key.seq;
		}
		auto& bucket = *m_open;
		if (!bucket.entries.empty() && !KeyLess(bucket.entries.back().key, key)) {
			if (bucket.entries.back().key == key) { // a second plan for the same call replaces the first
				bucket.bytes -= bucket.entries.back().plan->Bytes();
				bucket.dead.push_back(std::move(bucket.entries.back().plan));
				bucket.entries.pop_back();
				--bucket.live;
			} else {
				bucket.sorted = false;
			}
		}
		bucket.bytes += plan->Bytes();
		++bucket.live;
		bucket.entries.push_back({key, std::move(plan)});
		m_put.fetch_add(1, std::memory_order_relaxed);
		m_open_plans.store(bucket.live, std::memory_order_relaxed);
		m_open_bytes.store(bucket.bytes, std::memory_order_relaxed);
		if (bucket.entries.size() >= kPublishPlans) Publish();
	}
	// Lookahead thread: every event up to and including `position` has been looked at. A plan lookup
	// is only answered "made no plan" up to the position whose plans are published, so the open
	// bucket is published here when the submission ends, and while kyGpu is on this submission or the
	// one before it (it asks for plans that were made a moment ago); otherwise it stays private.
	void NoteProgress(Position position) {
		const uint64_t packed = Pack(position);
		if (!m_open) {
			RaiseVisible(packed);
		} else if (position.ordinal == UINT32_MAX ? m_open->seq <= position.seq
		                                          : m_consumer_seq.load(std::memory_order_relaxed) + 1 >= m_open->seq) {
			Publish(packed);
		}
	}
	// Lookahead thread: a whole submission was skipped because kyGpu had started it.
	void NoteLate(uint64_t seq) {
		std::lock_guard lock(m_mutex);
		if (m_late.insert(seq).second) {
			m_late_fifo.push_back(seq);
			if (m_late_fifo.size() > 4096) {
				m_late.erase(m_late_fifo.front());
				m_late_fifo.pop_front();
			}
		}
	}
	// kyGpu (--srt-plans): hands back plans it is done with, so the lookahead thread frees them
	// (and the buffers a swapped-in snapshot left in them) instead of kyGpu. Freed by the next publish.
	void Recycle(std::vector<std::unique_ptr<Plan>>&& dead) {
		std::lock_guard lock(m_mutex);
		for (auto& plan: dead) m_recycled.push_back(std::move(plan));
		dead.clear();
	}
	// kyGpu: removes and returns the plan for `key`, or says why there is none.
	Lookup Take(const Key& key, std::unique_ptr<Plan>& plan) {
		if (key.seq > m_noted_seq) {
			m_noted_seq = key.seq;
			m_consumer_seq.store(key.seq, std::memory_order_relaxed);
		}
		Local* local = FindLocal(key.seq);
		if (local != nullptr) {
			if (Find(*local, key, plan)) return Lookup::Ready;
		}
		// Slow path: pieces published since, or no bucket at all.
		std::lock_guard lock(m_mutex);
		const auto      it = m_published.find(key.seq);
		if (it != m_published.end()) {
			m_published_plans -= it->second->live;
			m_published_bytes -= it->second->bytes;
			if (local == nullptr) {
				m_local.push_back({std::move(it->second)});
				m_published.erase(it);
				EvictOldestLocal();
				local = FindLocal(key.seq);
			} else {
				AppendBucket(*local->bucket, std::move(*it->second));
				m_published.erase(it);
			}
			if (Find(*local, key, plan)) return Lookup::Ready;
		}
		if (local == nullptr) {
			if (m_late.contains(key.seq)) return Lookup::Late;
			if (key.seq <= m_evicted_seq) return Lookup::Evicted;
		}
		// The lookahead looked at this draw and made no plan, or it has not got there yet.
		return m_visible.load(std::memory_order_acquire) >= Pack({key.seq, key.ordinal}) ? Lookup::Missing
		                                                                                   : Lookup::NotReady;
	}

	struct Counters {
		uint64_t put = 0, evictions = 0, queued = 0, queued_bytes = 0;
	};
	[[nodiscard]] Counters GetCounters() {
		std::lock_guard lock(m_mutex);
		return {m_put.load(std::memory_order_relaxed), m_evictions,
		        m_published_plans + m_open_plans.load(std::memory_order_relaxed),
		        m_published_bytes + m_open_bytes.load(std::memory_order_relaxed)};
	}

private:
	static constexpr size_t kMaxLocal     = 16;
	static constexpr size_t kPublishPlans = 64;

	struct Entry {
		Key                   key;
		std::unique_ptr<Plan> plan;
	};
	struct Bucket {
		uint64_t                           seq = 0;
		std::vector<Entry>                 entries;
		std::vector<std::unique_ptr<Plan>> dead; // replaced before the bucket was published
		size_t                             live   = 0;
		size_t                             bytes  = 0;
		bool                               sorted = true;
	};
	struct Local {
		std::unique_ptr<Bucket> bucket;
		size_t                  cursor = 0;
	};

	static bool KeyLess(const Key& a, const Key& b) {
		return a.seq != b.seq ? a.seq < b.seq : a.ordinal != b.ordinal ? a.ordinal < b.ordinal : a.call < b.call;
	}
	static uint64_t Pack(Position p) { return (p.seq << 24u) | std::min<uint32_t>(p.ordinal, 0xffffffu); }

	// --plan-prefetch (DeepPrefetchEnabled is in srtPlan.cpp, it reads the config): every cache line of the plan's
	// blocks, not the first line of five of them.
	// A plan is built by a lookahead worker out of a dozen heap blocks (descriptor vectors, user data, read set); kyGpu
	// meets each of them cold, one dependent cache miss after another. Touching every line of every block while the
	// previous plan is used turns those misses into parallel prefetches.
	static void PrefetchPlanDeep(const Plan& p) {
		const auto block = [](const auto& v) {
			const auto* base  = reinterpret_cast<const char*>(v.data());
			const auto  bytes = v.size() * sizeof(*v.data());
			for (size_t offset = 0; offset < bytes && offset < 1024; offset += 64) {
				__builtin_prefetch(base + offset, 0, 3);
			}
		};
		const auto& s = p.snapshot;
		block(s.buffers);
		block(s.images);
		block(s.samplers);
		block(s.flattened_srt);
		block(s.user_data);
		block(s.specialization_reads);
		block(s.read_hull.intervals);
		block(p.specialization.buffers);
		block(p.specialization.images);
		block(p.workgroup_counts);
		block(p.reads.windows);
		block(p.reads.runs);
		block(p.reads.values);
	}
	static bool DeepPrefetchEnabled();

	static void PrefetchBody(const Plan& p) {
		__builtin_prefetch(p.snapshot.user_data.data(), 0, 3);
		__builtin_prefetch(p.workgroup_counts.data(), 0, 3);
		__builtin_prefetch(p.reads.windows.data(), 0, 3);
		__builtin_prefetch(p.reads.runs.data(), 0, 3);
		__builtin_prefetch(p.reads.values.data(), 0, 3);
	}

	void RaiseVisible(uint64_t packed) {
		uint64_t seen = m_visible.load(std::memory_order_relaxed);
		while (seen < packed && !m_visible.compare_exchange_weak(seen, packed, std::memory_order_release,
		                                                        std::memory_order_relaxed)) {
		}
	}

	// Moves the pieces of `src` (later keys of the same submission) to the end of `dst`.
	static void AppendBucket(Bucket& dst, Bucket&& src) {
		if (!src.entries.empty() && !dst.entries.empty() && !KeyLess(dst.entries.back().key, src.entries.front().key)) {
			dst.sorted = false;
		}
		dst.sorted = dst.sorted && src.sorted;
		dst.live += src.live;
		dst.bytes += src.bytes;
		dst.entries.insert(dst.entries.end(), std::make_move_iterator(src.entries.begin()),
		                   std::make_move_iterator(src.entries.end()));
		for (auto& d: src.dead) dst.dead.push_back(std::move(d));
	}

	Local* FindLocal(uint64_t seq) {
		if (m_hint < m_local.size() && m_local[m_hint].bucket->seq == seq) return &m_local[m_hint];
		for (size_t i = 0; i < m_local.size(); ++i) {
			if (m_local[i].bucket->seq == seq) {
				m_hint = i;
				return &m_local[i];
			}
		}
		return nullptr;
	}

	// m_mutex held: the oldest detached bucket (never the newest one) goes back to be freed.
	void EvictOldestLocal() {
		if (m_local.size() <= kMaxLocal) return;
		size_t oldest = 0;
		for (size_t i = 1; i + 1 < m_local.size(); ++i) {
			if (m_local[i].bucket->seq < m_local[oldest].bucket->seq) oldest = i;
		}
		for (auto& e: m_local[oldest].bucket->entries) if (e.plan) m_recycled.push_back(std::move(e.plan));
		for (auto& d: m_local[oldest].bucket->dead) m_recycled.push_back(std::move(d));
		m_local[oldest] = std::move(m_local.back());
		m_local.pop_back();
		m_hint = 0;
	}

	// kyGpu: looks `key` up in a detached bucket; true and `plan` set when its plan was there.
	static bool Find(Local& local, const Key& key, std::unique_ptr<Plan>& plan) {
		auto&  bucket  = *local.bucket;
		auto&  entries = bucket.entries;
		size_t i       = local.cursor;
		if (!(i < entries.size() && entries[i].key == key)) {
			if (bucket.sorted) {
				i = static_cast<size_t>(std::lower_bound(entries.begin(), entries.end(), key,
				        [](const Entry& e, const Key& k) { return KeyLess(e.key, k); }) - entries.begin());
			} else {
				i = entries.size();
				for (size_t j = 0; j < entries.size(); ++j) {
					if (entries[j].key == key && entries[j].plan) { i = j; break; }
				}
			}
		}
		if (i >= entries.size() || !(entries[i].key == key) || !entries[i].plan) return false;
		plan         = std::move(entries[i].plan);
		local.cursor = i + 1;
		// The next plans are checked next: pull their cache lines in while this one is used.
		if (DeepPrefetchEnabled()) {
			// The plan is spread over a dozen heap blocks written by another core: the Plan struct (and so the
			// vector headers) two plans ahead, all the blocks one plan ahead.
			if (i + 1 < entries.size() && entries[i + 1].plan) PrefetchPlanDeep(*entries[i + 1].plan);
			for (size_t j = i + 2; j <= i + 3 && j < entries.size(); ++j) {
				if (!entries[j].plan) continue;
				const auto* base = reinterpret_cast<const char*>(entries[j].plan.get());
				for (size_t o = 0; o < sizeof(Plan); o += 64) __builtin_prefetch(base + o, 0, 3);
			}
			return true;
		}
		if (i + 1 < entries.size() && entries[i + 1].plan) PrefetchBody(*entries[i + 1].plan);
		if (i + 2 < entries.size() && entries[i + 2].plan) __builtin_prefetch(entries[i + 2].plan.get(), 0, 3);
		return true;
	}

	// Producer: the open bucket becomes visible to kyGpu; the oldest published ones fall out when the
	// queue is over its limits.
	void Publish(uint64_t visible_upto = 0) {
		auto bucket = std::move(m_open);
		m_open_plans.store(0, std::memory_order_relaxed);
		m_open_bytes.store(0, std::memory_order_relaxed);
		std::vector<std::unique_ptr<Plan>> dropped; // freed (or pooled) outside the lock
		for (auto& d: bucket->dead) dropped.push_back(std::move(d));
		bucket->dead.clear();
		std::unique_lock lock(m_mutex);
		dropped.insert(dropped.end(), std::make_move_iterator(m_recycled.begin()),
		               std::make_move_iterator(m_recycled.end()));
		m_recycled.clear();
		if (bucket->live != 0) {
			m_published_plans += bucket->live;
			m_published_bytes += bucket->bytes;
			const auto seq = bucket->seq;
			const auto it  = m_published.find(seq);
			if (it == m_published.end()) {
				m_published[seq] = std::move(bucket);
			} else {
				AppendBucket(*it->second, std::move(*bucket));
			}
		}
		RaiseVisible(visible_upto);
		while (m_published.size() > 1 && (m_published_plans > m_max_plans || m_published_bytes > m_max_bytes)) {
			auto oldest = m_published.begin();
			m_published_plans -= oldest->second->live;
			m_published_bytes -= oldest->second->bytes;
			m_evictions += oldest->second->live;
			m_evicted_seq = std::max(m_evicted_seq, oldest->first);
			for (auto& e: oldest->second->entries) if (e.plan) dropped.push_back(std::move(e.plan));
			m_published.erase(oldest);
		}
		lock.unlock();
		if (!dropped.empty() && PoolEnabled()) GetPlanPool().Give(dropped);
	}

	size_t                                        m_max_plans;
	size_t                                        m_max_bytes;
	// producer only
	std::unique_ptr<Bucket>                       m_open;
	// consumer only
	std::vector<Local>                            m_local;
	size_t                                        m_hint = 0;
	uint64_t                                      m_noted_seq = 0;
	// shared
	std::atomic<uint64_t>                         m_consumer_seq {0}; // highest submission kyGpu asked about
	std::atomic<uint64_t>                         m_visible {0}; // looked at, and its plans published
	std::atomic<uint64_t>                         m_put {0};
	std::atomic<size_t>                           m_open_plans {0};
	std::atomic<size_t>                           m_open_bytes {0};
	std::mutex                                    m_mutex; // guards the members below
	std::map<uint64_t, std::unique_ptr<Bucket>>   m_published;
	size_t                                        m_published_plans = 0;
	size_t                                        m_published_bytes = 0;
	std::vector<std::unique_ptr<Plan>>            m_recycled;
	std::unordered_set<uint64_t>                  m_late;
	std::deque<uint64_t>                          m_late_fifo;
	uint64_t                                      m_evicted_seq = 0;
	uint64_t                                      m_evictions   = 0;
};

// ---- statistics (kyGpu thread only) ---------------------------------------------------------

struct Percentiles {
	double p10 = 0, p50 = 0, p90 = 0;
	size_t n = 0;
};

[[nodiscard]] inline Percentiles ComputePercentiles(std::vector<double> values) {
	Percentiles result;
	result.n = values.size();
	if (values.empty()) return result;
	std::sort(values.begin(), values.end());
	const auto at = [&](double q) {
		return values[static_cast<size_t>(q * static_cast<double>(values.size() - 1) + 0.5)];
	};
	result.p10 = at(0.10);
	result.p50 = at(0.50);
	result.p90 = at(0.90);
	return result;
}

// Fixed-size uniform sample of a stream (algorithm R).
class Reservoir {
public:
	explicit Reservoir(size_t capacity = 1u << 18u): m_capacity(capacity) { m_values.reserve(1024); }
	void Add(double value) {
		++m_seen;
		if (m_values.size() < m_capacity) {
			m_values.push_back(value);
			return;
		}
		m_rng ^= m_rng << 13u;
		m_rng ^= m_rng >> 7u;
		m_rng ^= m_rng << 17u;
		const auto slot = m_rng % m_seen;
		if (slot < m_capacity) m_values[slot] = value;
	}
	void AddAll(const Reservoir& other) {
		for (const auto value: other.m_values) Add(value);
	}
	void Clear() {
		m_values.clear();
		m_seen = 0;
	}
	[[nodiscard]] Percentiles Get() const { return ComputePercentiles(m_values); }
	[[nodiscard]] size_t      Seen() const { return m_seen; }

private:
	size_t              m_capacity;
	size_t              m_seen = 0;
	uint64_t            m_rng  = 0x2545f4914f6cdd1dull;
	std::vector<double> m_values;
};

enum class Outcome : uint8_t {
	Ready,
	NotReady,
	Missing,
	Late,
	Evicted,
	Desync,    // a plan exists but is for another stage/shader: the two processors diverged
	SubDraw,   // nested draw of an indirect multi-draw; never planned
	NewSource, // kyGpu translated the shader for the first time; the lookahead cannot have a plan
	Count
};
inline constexpr std::array<const char*, static_cast<size_t>(Outcome::Count)> kOutcomeNames = {
    "ready", "not_ready", "missing", "late", "evicted", "desync", "subdraw", "new_source"};

enum class Invalid : uint8_t { None, Inputs, Changed, Unreadable, GpuDirty, Count };
inline constexpr std::array<const char*, static_cast<size_t>(Invalid::Count)> kInvalidNames = {
    "none", "inputs", "reads_changed", "unreadable", "gpu_dirty"};

struct Sample {
	Outcome  outcome = Outcome::Missing;
	bool     compute = false;
	Invalid  invalid = Invalid::None; // Ready only
	bool     equal   = false;         // Ready only: plan bytes == kyGpu bytes
	uint32_t diff    = 0;
	int64_t  lead_ns = 0;
	uint64_t lead_draws = 0;
	int64_t  check_ns = 0;
	int64_t  materialize_ns = 0;
	int64_t  lookup_ns = 0;  // store lookup (Take)
	int64_t  compare_ns = 0; // shadow-only byte comparison
	uint32_t windows = 0, runs = 0, dwords = 0, clean_windows = 0;
};

struct IntervalRecord {
	uint64_t    events = 0, ready = 0, valid = 0, equal = 0;
	Percentiles lead_us, lead_draws;
};

class Stats {
public:
	struct Totals {
		uint64_t events = 0;
		std::array<uint64_t, static_cast<size_t>(Outcome::Count)> outcome {};
		std::array<uint64_t, static_cast<size_t>(Invalid::Count)> invalid {};
		uint64_t ready_graphics = 0, ready_compute = 0;
		uint64_t valid = 0, equal = 0, valid_equal = 0, valid_unequal = 0, invalid_equal = 0;
		std::array<uint64_t, 8> diff_bits {};
		int64_t  check_ns = 0, materialize_ns = 0, lookup_ns = 0, compare_ns = 0;
		uint64_t windows = 0, runs = 0, dwords = 0, clean_windows = 0;
		void Add(const Totals& o) {
			events += o.events;
			for (size_t i = 0; i < outcome.size(); ++i) outcome[i] += o.outcome[i];
			for (size_t i = 0; i < invalid.size(); ++i) invalid[i] += o.invalid[i];
			ready_graphics += o.ready_graphics;
			ready_compute += o.ready_compute;
			valid += o.valid;
			equal += o.equal;
			valid_equal += o.valid_equal;
			valid_unequal += o.valid_unequal;
			invalid_equal += o.invalid_equal;
			for (size_t i = 0; i < diff_bits.size(); ++i) diff_bits[i] += o.diff_bits[i];
			check_ns += o.check_ns;
			materialize_ns += o.materialize_ns;
			lookup_ns += o.lookup_ns;
			compare_ns += o.compare_ns;
			windows += o.windows;
			runs += o.runs;
			dwords += o.dwords;
			clean_windows += o.clean_windows;
		}
	};

	void Add(const Sample& s) {
		++m_window.events;
		++m_window.outcome[static_cast<size_t>(s.outcome)];
		m_window.lookup_ns += s.lookup_ns;
		++m_interval.events;
		if (s.outcome != Outcome::Ready) return;
		(s.compute ? m_window.ready_compute : m_window.ready_graphics)++;
		++m_window.invalid[static_cast<size_t>(s.invalid)];
		const bool valid = s.invalid == Invalid::None;
		m_window.valid += valid;
		m_window.equal += s.equal;
		m_window.valid_equal += valid && s.equal;
		m_window.valid_unequal += valid && !s.equal;
		m_window.invalid_equal += !valid && s.equal;
		for (size_t bit = 0; bit < m_window.diff_bits.size(); ++bit) {
			m_window.diff_bits[bit] += (s.diff >> bit) & 1u;
		}
		m_window.check_ns += s.check_ns;
		m_window.materialize_ns += s.materialize_ns;
		m_window.compare_ns += s.compare_ns;
		m_window.windows += s.windows;
		m_window.runs += s.runs;
		m_window.dwords += s.dwords;
		m_window.clean_windows += s.clean_windows;
		const double lead_us = static_cast<double>(s.lead_ns) / 1000.0;
		m_window_lead_us.Add(lead_us);
		m_window_lead_draws.Add(static_cast<double>(s.lead_draws));
		m_window_check_ns.Add(static_cast<double>(s.check_ns));
		m_window_materialize_ns.Add(static_cast<double>(s.materialize_ns));
		++m_interval.ready;
		m_interval.valid += valid;
		m_interval.equal += s.equal;
		m_interval_lead_us.push_back(lead_us);
		m_interval_lead_draws.push_back(static_cast<double>(s.lead_draws));
	}

	// A drain on kyGpu ends the current interval (the span of draws between two GPU waits).
	IntervalRecord EndInterval() {
		IntervalRecord record = m_interval;
		record.lead_us        = ComputePercentiles(std::move(m_interval_lead_us));
		record.lead_draws     = ComputePercentiles(std::move(m_interval_lead_draws));
		m_interval_lead_us.clear();
		m_interval_lead_draws.clear();
		m_interval = {};
		if (record.events != 0) {
			if (m_intervals.size() < 200000) m_intervals.push_back(record);
			if (record.ready >= 8) {
				m_interval_median_lead_us.push_back(record.lead_us.p50);
				m_interval_median_lead_draws.push_back(record.lead_draws.p50);
				m_interval_p10_lead_us.push_back(record.lead_us.p10);
				m_interval_p90_lead_us.push_back(record.lead_us.p90);
			}
		}
		return record;
	}

	// Moves the window into the totals and returns what it held.
	struct Window {
		Totals      totals;
		Percentiles lead_us, lead_draws, check_ns, materialize_ns;
	};
	Window TakeWindow() {
		Window w{m_window, m_window_lead_us.Get(), m_window_lead_draws.Get(), m_window_check_ns.Get(),
		         m_window_materialize_ns.Get()};
		m_total.Add(m_window);
		m_total_lead_us.AddAll(m_window_lead_us);
		m_total_lead_draws.AddAll(m_window_lead_draws);
		m_total_check_ns.AddAll(m_window_check_ns);
		m_total_materialize_ns.AddAll(m_window_materialize_ns);
		m_window = {};
		m_window_lead_us.Clear();
		m_window_lead_draws.Clear();
		m_window_check_ns.Clear();
		m_window_materialize_ns.Clear();
		return w;
	}

	[[nodiscard]] const Totals& Total() const { return m_total; }
	[[nodiscard]] Window        TotalWindow() const {
        return {m_total, m_total_lead_us.Get(), m_total_lead_draws.Get(), m_total_check_ns.Get(),
                m_total_materialize_ns.Get()};
	}
	[[nodiscard]] const std::vector<IntervalRecord>& Intervals() const { return m_intervals; }
	[[nodiscard]] Percentiles IntervalMedianLeadUs() const { return ComputePercentiles(m_interval_median_lead_us); }
	[[nodiscard]] Percentiles IntervalMedianLeadDraws() const {
		return ComputePercentiles(m_interval_median_lead_draws);
	}
	[[nodiscard]] Percentiles IntervalP10LeadUs() const { return ComputePercentiles(m_interval_p10_lead_us); }
	[[nodiscard]] Percentiles IntervalP90LeadUs() const { return ComputePercentiles(m_interval_p90_lead_us); }

private:
	Totals                    m_window, m_total;
	Reservoir                 m_window_lead_us, m_window_lead_draws, m_window_check_ns, m_window_materialize_ns;
	Reservoir                 m_total_lead_us, m_total_lead_draws, m_total_check_ns, m_total_materialize_ns;
	IntervalRecord            m_interval;
	std::vector<double>       m_interval_lead_us, m_interval_lead_draws;
	std::vector<IntervalRecord> m_intervals;
	std::vector<double>       m_interval_median_lead_us, m_interval_median_lead_draws;
	std::vector<double>       m_interval_p10_lead_us, m_interval_p90_lead_us;
};

// ---- adoption (--srt-plans) -------------------------------------------------------------------

// What kyGpu does with the plan of a draw. Adopted iff outcome == Ready and invalid == None.
struct Decision {
	Outcome outcome = Outcome::Missing;
	Invalid invalid = Invalid::None;
	[[nodiscard]] bool Adopted() const { return outcome == Outcome::Ready && invalid == Invalid::None; }
};

// Looks the draw's plan up and checks it: the plan is for this stage and shader, was made from the
// same inputs, and the guest memory its walk read is unchanged and holds no GPU-written bytes.
// `plan` receives the plan whenever one was found, adopted or not (the caller recycles it).
//   read(kind, address, std::span<uint32_t>, exact) -> bool, dirty(address, bytes) -> bool: see
//   ReadSet::Validate.
template <class Read, class Dirty>
[[nodiscard]] Decision Decide(Store& store, const Key& key, bool nested, uint32_t stage,
                              uint64_t shader_hash, const Inputs& inputs, Read&& read, Dirty&& dirty,
                              std::unique_ptr<Plan>& plan) {
	if (nested) return {Outcome::SubDraw, Invalid::None};
	switch (store.Take(key, plan)) {
		case Lookup::NotReady: return {Outcome::NotReady, Invalid::None};
		case Lookup::Missing: return {Outcome::Missing, Invalid::None};
		case Lookup::Late: return {Outcome::Late, Invalid::None};
		case Lookup::Evicted: return {Outcome::Evicted, Invalid::None};
		case Lookup::Ready: break;
	}
	if (plan->stage != stage || plan->shader_hash != shader_hash) return {Outcome::Desync, Invalid::None};
	if (!InputsMatch(*plan, inputs)) return {Outcome::Ready, Invalid::Inputs};
	switch (plan->reads.Validate(read, dirty)) {
		case ReadSet::Result::Valid: return {Outcome::Ready, Invalid::None};
		case ReadSet::Result::Changed: return {Outcome::Ready, Invalid::Changed};
		case ReadSet::Result::Unreadable: return {Outcome::Ready, Invalid::Unreadable};
		case ReadSet::Result::GpuDirty: return {Outcome::Ready, Invalid::GpuDirty};
	}
	return {Outcome::Ready, Invalid::Changed};
}

// The adoption itself: the draw's materialization result (the program source entry's snapshot and
// specialization) becomes the plan's, and the plan keeps the old buffers for recycling.
inline void AdoptInto(ShaderRecompiler::IR::ResourceSnapshot&       resources,
                      ShaderRecompiler::IR::ResourceSpecialization& specialization, Plan& plan) {
	std::swap(resources, plan.snapshot);
	std::swap(specialization, plan.specialization);
}

// Counters of --srt-plans, kept by kyGpu alone (no clocks, no samples: this runs on every draw).
struct AdoptTotals {
	uint64_t events = 0, adopted = 0, call_source = 0, new_source = 0;
	std::array<uint64_t, static_cast<size_t>(Outcome::Count)> outcome {};
	std::array<uint64_t, static_cast<size_t>(Invalid::Count)> invalid {};
	// --srt-plan-shadow together with --srt-plans: every adopted plan is compared with kyGpu's own
	// materialization of the same draw.
	uint64_t verified = 0, mismatched = 0;
	std::array<uint64_t, 8> diff_bits {};

	void Note(const Decision& decision) {
		++events;
		++outcome[static_cast<size_t>(decision.outcome)];
		if (decision.outcome == Outcome::Ready) ++invalid[static_cast<size_t>(decision.invalid)];
		adopted += decision.Adopted();
	}
	void NoteVerified(uint32_t diff) {
		++verified;
		if (diff == 0) return;
		++mismatched;
		for (size_t bit = 0; bit < diff_bits.size(); ++bit) diff_bits[bit] += (diff >> bit) & 1u;
	}
	void Add(const AdoptTotals& o) {
		events += o.events;
		adopted += o.adopted;
		call_source += o.call_source;
		new_source += o.new_source;
		for (size_t i = 0; i < outcome.size(); ++i) outcome[i] += o.outcome[i];
		for (size_t i = 0; i < invalid.size(); ++i) invalid[i] += o.invalid[i];
		verified += o.verified;
		mismatched += o.mismatched;
		for (size_t i = 0; i < diff_bits.size(); ++i) diff_bits[i] += o.diff_bits[i];
	}
};

// ---- process-wide instance (srtPlan.cpp) -------------------------------------------------------

[[nodiscard]] Store& GetStore();
[[nodiscard]] int64_t NowNs();

// Lookahead thread or prefetch worker: where a finished plan goes. A prefetch worker (--lookahead-workers)
// collects the plans of one draw into its task and the sequencer puts them into the store in draw
// order, which keeps the store single-producer; elsewhere the plan goes straight into the store.
inline thread_local std::vector<std::unique_ptr<Plan>>* t_plan_sink = nullptr;
inline void PutPlan(std::unique_ptr<Plan> plan) {
	if (t_plan_sink != nullptr) {
		t_plan_sink->push_back(std::move(plan));
	} else {
		GetStore().Put(std::move(plan));
	}
}
class PlanSinkScope {
public:
	explicit PlanSinkScope(std::vector<std::unique_ptr<Plan>>* sink) noexcept: m_previous(t_plan_sink) {
		t_plan_sink = sink;
	}
	~PlanSinkScope() { t_plan_sink = m_previous; }
	PlanSinkScope(const PlanSinkScope&)            = delete;
	PlanSinkScope& operator=(const PlanSinkScope&) = delete;

private:
	std::vector<std::unique_ptr<Plan>>* m_previous;
};

// Lookahead thread.
void NoteProducerEvent();                  // a draw/dispatch was looked at (counts draws of lead)
[[nodiscard]] uint64_t ProducerEvents();
void NoteProduced(uint64_t bytes);         // plan stored
void NoteProduceFailed();                  // materialization failed / plan not made
void NoteProduceCost(int64_t ns);          // recording + finishing the plan
// kyGpu thread.
[[nodiscard]] bool AdoptEnabled();          // --srt-plans
[[nodiscard]] bool ShadowEnabled();         // --srt-plan-shadow
[[nodiscard]] AdoptTotals& Adopting();      // the counters of the current window (folded into the total at Poll)
void RecyclePlan(std::unique_ptr<Plan> plan); // batches finished plans back to the lookahead thread
// kyGpu thread: one plan lookup and how it ended, for the look-ahead governor (--lookahead-adaptive). A miss
// is a plan that was not there in time: not ready, its whole submission was started before the look-ahead
// looked at it, or the look-ahead passed the draw without a plan (workers that cannot keep up drop tasks
// as stale, which is what overloaded workers look like). Draws that are never planned (nested draws, a
// shader seen for the first time) do not count.
void NoteLookup(Outcome outcome);
struct LookupCounts {
	uint64_t events = 0;
	uint64_t misses = 0;
};
[[nodiscard]] LookupCounts LookupSnapshot(); // any thread
void Record(const Sample& sample);
void NoteDrain();
void Poll();
[[nodiscard]] std::string SummaryText();   // the whole run, for the exit log

} // namespace Libs::Graphics::SrtPlan

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLAN_H_
