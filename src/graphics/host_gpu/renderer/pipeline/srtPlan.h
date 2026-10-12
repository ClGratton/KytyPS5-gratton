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
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
	//   dirty(address, bytes) -> bool: the range holds GPU-written bytes the guest backing lacks.
	template <class Read, class Dirty>
	[[nodiscard]] Result Validate(Read&& read, Dirty&& dirty) const {
		std::array<uint32_t, kMaxWindowBytes / 4u> scratch;
		for (const auto& window: windows) {
			if (window.kind == ReadKind::Clean && dirty(window.address, window.bytes)) {
				return Result::GpuDirty;
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
			m_entries.push_back({address + i * 4u, values[i], kind});
		}
	}
	void   Clear() { m_entries.clear(); }
	size_t Size() const { return m_entries.size(); }

	// Sorts, drops repeated addresses (the first value read wins), merges contiguous dwords into
	// runs and nearby runs into windows. Leaves the recorder empty.
	ReadSet Finish() {
		ReadSet set;
		for (const auto kind: {ReadKind::Scalar, ReadKind::Clean}) {
			m_sorted.clear();
			for (const auto& entry: m_entries) {
				if (entry.kind == kind) m_sorted.push_back(entry);
			}
			std::stable_sort(m_sorted.begin(), m_sorted.end(),
			                 [](const Entry& a, const Entry& b) { return a.address < b.address; });
			m_sorted.erase(std::unique(m_sorted.begin(), m_sorted.end(),
			                           [](const Entry& a, const Entry& b) { return a.address == b.address; }),
			               m_sorted.end());
			AppendKind(set, kind);
		}
		m_entries.clear();
		return set;
	}

private:
	struct Entry {
		uint64_t address = 0;
		uint32_t value   = 0;
		ReadKind kind    = ReadKind::Scalar;
	};
	struct RawRun {
		uint64_t address;
		uint32_t dwords;
		uint32_t value_offset;
	};

	void AppendKind(ReadSet& set, ReadKind kind) {
		if (m_sorted.empty()) return;
		m_raw_runs.clear();
		for (size_t i = 0; i < m_sorted.size();) {
			size_t j = i + 1;
			while (j < m_sorted.size() && m_sorted[j].address == m_sorted[j - 1].address + 4u &&
			       j - i < ReadSet::kMaxRunDwords) {
				++j;
			}
			m_raw_runs.push_back({m_sorted[i].address, static_cast<uint32_t>(j - i),
			                      static_cast<uint32_t>(set.values.size())});
			for (size_t k = i; k < j; ++k) set.values.push_back(m_sorted[k].value);
			i = j;
		}
		// Windows: runs whose gap is small enough share one memory read.
		size_t i = 0;
		while (i < m_raw_runs.size()) {
			const uint64_t start = m_raw_runs[i].address;
			uint64_t       end   = start + m_raw_runs[i].dwords * uint64_t {4};
			size_t         j     = i + 1;
			while (j < m_raw_runs.size() && m_raw_runs[j].address <= end + ReadSet::kMaxGapBytes &&
			       std::max(end, m_raw_runs[j].address + m_raw_runs[j].dwords * uint64_t {4}) - start <=
			           ReadSet::kMaxWindowBytes) {
				end = std::max(end, m_raw_runs[j].address + m_raw_runs[j].dwords * uint64_t {4});
				++j;
			}
			ReadSet::Window window;
			window.address   = start;
			window.bytes     = static_cast<uint32_t>((end - start + 3u) & ~uint64_t {3}); // whole dwords
			window.first_run = static_cast<uint32_t>(set.runs.size());
			window.run_count = static_cast<uint32_t>(j - i);
			window.kind      = kind;
			for (size_t k = i; k < j; ++k) {
				set.runs.push_back({static_cast<uint32_t>(m_raw_runs[k].address - start),
				                    m_raw_runs[k].dwords, m_raw_runs[k].value_offset});
			}
			set.windows.push_back(window);
			i = j;
		}
	}

	std::vector<Entry>  m_entries;
	std::vector<Entry>  m_sorted;
	std::vector<RawRun> m_raw_runs;
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
};

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

// Bounded FIFO of plans, written by the lookahead thread and read by kyGpu.
class Store {
public:
	Store(size_t max_plans, size_t max_bytes): m_max_plans(max_plans), m_max_bytes(max_bytes) {}

	// Lookahead thread.
	void Put(std::unique_ptr<Plan> plan) {
		std::vector<std::unique_ptr<Plan>> dropped; // freed outside the lock
		{
			std::lock_guard lock(m_mutex);
			const auto      key = plan->key;
			m_bytes += plan->Bytes();
			auto [it, inserted] = m_map.try_emplace(key);
			if (!inserted) {
				m_bytes -= it->second->Bytes();
				dropped.push_back(std::move(it->second));
			} else {
				m_fifo.push_back(key);
			}
			it->second = std::move(plan);
			++m_put;
			while (!m_fifo.empty() && (m_map.size() > m_max_plans || m_bytes > m_max_bytes)) {
				const auto oldest = m_fifo.front();
				m_fifo.pop_front();
				auto found = m_map.find(oldest);
				if (found == m_map.end()) continue; // taken already
				m_bytes -= found->second->Bytes();
				m_evicted = std::max(m_evicted, PositionOf(oldest));
				dropped.push_back(std::move(found->second));
				m_map.erase(found);
				++m_evictions;
			}
		}
	}
	// Lookahead thread: every event up to and including `position` has been looked at.
	void NoteProgress(Position position) {
		std::lock_guard lock(m_mutex);
		m_progress = std::max(m_progress, position);
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
	// kyGpu: removes and returns the plan for `key`, or says why there is none.
	Lookup Take(const Key& key, std::unique_ptr<Plan>& plan) {
		std::lock_guard lock(m_mutex);
		const auto      it = m_map.find(key);
		if (it != m_map.end()) {
			m_bytes -= it->second->Bytes();
			plan = std::move(it->second);
			m_map.erase(it);
			return Lookup::Ready;
		}
		const auto position = PositionOf(key);
		if (m_late.contains(key.seq)) return Lookup::Late;
		if (position <= m_evicted) return Lookup::Evicted;
		if (position <= m_progress) return Lookup::Missing;
		return Lookup::NotReady;
	}

	struct Counters {
		uint64_t put = 0, evictions = 0, queued = 0, queued_bytes = 0;
	};
	[[nodiscard]] Counters GetCounters() {
		std::lock_guard lock(m_mutex);
		return {m_put, m_evictions, m_map.size(), m_bytes};
	}

private:
	std::mutex                                   m_mutex;
	size_t                                       m_max_plans;
	size_t                                       m_max_bytes;
	size_t                                       m_bytes = 0;
	std::unordered_map<Key, std::unique_ptr<Plan>, KeyHash> m_map;
	std::deque<Key>                              m_fifo;
	std::unordered_set<uint64_t>                 m_late;
	std::deque<uint64_t>                         m_late_fifo;
	Position                                     m_progress;
	Position                                     m_evicted;
	uint64_t                                     m_put       = 0;
	uint64_t                                     m_evictions = 0;
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

// ---- process-wide instance (srtPlan.cpp) -------------------------------------------------------

[[nodiscard]] Store& GetStore();
[[nodiscard]] int64_t NowNs();

// Lookahead thread.
void NoteProducerEvent();                  // a draw/dispatch was looked at (counts draws of lead)
[[nodiscard]] uint64_t ProducerEvents();
void NoteProduced(uint64_t bytes);         // plan stored
void NoteProduceFailed();                  // materialization failed / plan not made
void NoteProduceCost(int64_t ns);          // recording + finishing the plan
// kyGpu thread.
void Record(const Sample& sample);
void NoteDrain();
void Poll();
[[nodiscard]] std::string SummaryText();   // the whole run, for the exit log

} // namespace Libs::Graphics::SrtPlan

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SRTPLAN_H_
