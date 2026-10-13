#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPENDENCIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPENDENCIES_H_

// --debug-skip-log, log-only dependency check for --async-pipelines skip.
//
// A skipped graphics draw leaves the memory it should have written (render targets, depth/stencil,
// their metadata) with stale contents. This tracker remembers those ranges ("poison") until a draw
// with the same shader pair really executes (its pipeline is ready), and reports every later reader
// of poisoned memory: a pass that samples it, a scalar SRT walk that reads a descriptor table from it,
// an indirect-argument read, a DMA, a CPU read. A pass that reads poison and writes memory passes the
// poison on ("taint"), so a descriptor table built by a compute pass from a skipped draw's output is
// still attributed to the skipped draw.
//
// Without --skip-guard nothing here changes what the emulator does: it is the evidence for a
// dependency-aware skip rule. With --skip-guard the tracker also learns (see "Guard" below): a
// hard-class reader (descriptor-table walk, indirect arguments, CP DMA, CPU readback, index / vertex
// data) hitting a skipped draw's output marks that output's ranges unsafe, and later draws that
// would write an unsafe range wait for their pipeline instead of being skipped. Pure data structure
// with one mutex, so it can be fed from the GPU thread, the lookahead thread and guest threads;
// unit tested in tests/SkipDependenciesTests.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace Libs::Graphics::SkipDeps {

struct Range {
	uint64_t address = 0;
	uint64_t size    = 0;

	bool operator==(const Range&) const = default;
};

// Empty ranges never overlap; wrapping ranges are clamped to the end of the address space.
[[nodiscard]] constexpr bool Overlaps(const Range& a, const Range& b) {
	if (a.size == 0 || b.size == 0) return false;
	const uint64_t a_end = a.address > UINT64_MAX - a.size ? UINT64_MAX : a.address + a.size;
	const uint64_t b_end = b.address > UINT64_MAX - b.size ? UINT64_MAX : b.address + b.size;
	return a.address < b_end && b.address < a_end;
}

[[nodiscard]] constexpr bool Covers(const Range& outer, const Range& inner) {
	if (inner.size == 0) return false;
	const uint64_t o_end = outer.address > UINT64_MAX - outer.size ? UINT64_MAX : outer.address + outer.size;
	const uint64_t i_end = inner.address > UINT64_MAX - inner.size ? UINT64_MAX : inner.address + inner.size;
	return outer.address <= inner.address && i_end <= o_end;
}

[[nodiscard]] constexpr uint64_t RangeEnd(const Range& r) {
	return r.address > UINT64_MAX - r.size ? UINT64_MAX : r.address + r.size;
}

// A sorted set of disjoint guest ranges (adjacent and overlapping ranges merge). It is capped: past
// kMaxRanges disjoint ranges the set is "saturated" and answers true to every overlap query, which
// is the safe direction for a set of ranges a skipped draw must not write.
class RangeSet {
public:
	static constexpr size_t kMaxRanges = 4096;

	// Adds `r`; true when some part of it was not covered before.
	bool Add(const Range& r) {
		if (r.size == 0) return false;
		const uint64_t b = r.address, e = RangeEnd(r);
		auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), b,
		                           [](const Range& x, uint64_t v) { return RangeEnd(x) < v; });
		if (it != m_ranges.end() && it->address <= b && RangeEnd(*it) >= e) return false;
		uint64_t nb = b, ne = e;
		const auto first = it;
		while (it != m_ranges.end() && it->address <= e) {
			nb = std::min(nb, it->address);
			ne = std::max(ne, RangeEnd(*it));
			++it;
		}
		it = m_ranges.erase(first, it);
		m_ranges.insert(it, Range {nb, ne - nb});
		if (m_ranges.size() > kMaxRanges) m_saturated = true;
		return true;
	}

	[[nodiscard]] bool Overlaps(const Range& r) const {
		if (r.size == 0) return false;
		if (m_saturated) return true;
		const uint64_t b = r.address;
		const auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), b,
		                                 [](const Range& x, uint64_t v) { return RangeEnd(x) <= v; });
		return it != m_ranges.end() && it->address < RangeEnd(r);
	}

	[[nodiscard]] size_t Size() const { return m_ranges.size(); }
	[[nodiscard]] bool   Saturated() const { return m_saturated; }

private:
	std::vector<Range> m_ranges;
	bool               m_saturated = false;
};

enum class ReadKind : uint8_t {
	SampledImage,  // an image sampled or loaded by a draw or dispatch
	BufferRead,    // a buffer a shader reads
	ScalarWalk,    // the SRT walk reads a descriptor table or pointer from this memory
	IndirectArgs,  // draw / dispatch arguments
	IndexOrVertex, // index or vertex data
	Dma,           // CP DMA / copy source
	CpuRead,       // the guest CPU reads the range (readback)
	Count
};

inline constexpr size_t kReadKinds = static_cast<size_t>(ReadKind::Count);

// How bad it is when a skipped draw's output is read: Visual = the pixels of one frame are wrong
// (what skip already accepts), Data = buffer / DMA / geometry data derives from it, Cpu = the guest
// CPU reads it back (game logic can change), Table = descriptors, pointers or draw arguments derive
// from it (the abort class: garbage V#/T#). Ordered: a reader of a higher class counts for the lower
// ones too.
enum class Sink : uint8_t { Visual = 0, Data = 1, Cpu = 2, Table = 3 };
inline constexpr size_t kSinks = 4;

[[nodiscard]] constexpr Sink SinkOf(ReadKind kind) {
	switch (kind) {
		case ReadKind::SampledImage: return Sink::Visual;
		case ReadKind::ScalarWalk:
		case ReadKind::IndirectArgs: return Sink::Table;
		case ReadKind::CpuRead: return Sink::Cpu;
		default: return Sink::Data;
	}
}

// The readers --skip-guard learns from: the data they read decides what later passes do (descriptor
// tables, draw arguments, geometry, DMA'd or CPU-read data), so a stale value is not just a wrong
// pixel. Sampled images (the visual effect skip accepts) and buffer bindings (binding extents
// over-approximate; a table built from such a read is caught through the taint) are not hard.
[[nodiscard]] constexpr bool IsHardRead(ReadKind kind) {
	switch (kind) {
		case ReadKind::ScalarWalk:
		case ReadKind::IndirectArgs:
		case ReadKind::IndexOrVertex:
		case ReadKind::Dma:
		case ReadKind::CpuRead: return true;
		default: return false;
	}
}

[[nodiscard]] constexpr const char* ReadKindName(ReadKind kind) {
	switch (kind) {
		case ReadKind::SampledImage: return "image";
		case ReadKind::BufferRead: return "buffer";
		case ReadKind::ScalarWalk: return "srt";
		case ReadKind::IndirectArgs: return "indirect";
		case ReadKind::IndexOrVertex: return "geometry";
		case ReadKind::Dma: return "dma";
		case ReadKind::CpuRead: return "cpu";
		default: return "?";
	}
}

[[nodiscard]] constexpr const char* SinkName(Sink sink) {
	switch (sink) {
		case Sink::Visual: return "visual";
		case Sink::Data: return "data";
		case Sink::Cpu: return "cpu";
		case Sink::Table: return "table";
	}
	return "?";
}

inline constexpr size_t kMaxEntries = 8192;
inline constexpr size_t kMaxRoots   = 6;

// One report line's worth of a reader hitting poison.
struct Hit {
	uint32_t entry   = 0;
	bool     derived = false; // the entry is a taint made by a pass that read poison
	bool     first   = false; // first time this reader (kind, tag) hit this entry: worth a log line
	uint32_t root_count = 0;
	std::array<uint32_t, kMaxRoots> roots {};
	uint64_t root_vs = 0, root_ps = 0; // shader pair of the first root
	uint32_t depth = 0;                // 0 = direct output of a skipped draw
	// --skip-guard learning: this hit was a hard-class read.
	bool     hard         = false;
	bool     newly_unsafe = false; // it made at least one range unsafe (first hard hit of that range)
	uint32_t unsafe_added = 0;     // ranges newly unsafe because of it
	Range    unsafe_first {};      // the first of them
	uint32_t new_misses   = 0;     // skip events newly known to have been read by a hard reader
};

struct RootStats {
	uint32_t id = 0;
	uint64_t key = 0, vs = 0, ps = 0;
	uint32_t skips = 0;
	std::array<uint32_t, kSinks> dependent {}; // skip events that had a reader of at least this sink
	std::array<uint32_t, kReadKinds> reads {};
	std::array<uint32_t, kReadKinds> transitive_reads {};
	uint32_t misses = 0; // skip events a hard-class reader read before the pipeline was ready
	int64_t first_ns = 0, ready_ns = 0;
	bool resolved = false;
};

struct Totals {
	uint64_t skip_events = 0;
	uint64_t entries     = 0;
	uint64_t resolved    = 0;
	uint64_t dropped_entries = 0; // taints / skips not tracked because the table was full
	std::array<uint64_t, kSinks> dependent {}; // skip events with a reader of at least this sink
};

// --skip-guard bookkeeping.
struct GuardCounts {
	uint64_t hard_ranges     = 0; // ranges made unsafe by a hard-class reader
	uint64_t bda_ranges      = 0; // ... by a shader with 64-bit loads that read poison
	uint64_t misses          = 0; // skip events a hard reader read afterwards (correctness misses)
	uint64_t misses_derived  = 0; // ... of which seen through a taint (a pass derived data from them)
	uint64_t hard_roots      = 0; // roots with at least one miss
	std::array<uint64_t, kReadKinds> hard_hits {}; // hard reads that hit poison, by kind
};

class Tracker {
public:
	// --skip-guard: learn unsafe ranges. `bda` also treats a shader with 64-bit address loads that
	// reads poison as a possible hard reader (the blind class: its loads are invisible).
	void SetGuard(bool enabled, bool bda) {
		std::lock_guard lock(m_mutex);
		m_guard     = enabled;
		m_guard_bda = enabled && bda;
	}
	[[nodiscard]] bool GuardEnabled() const {
		std::lock_guard lock(m_mutex);
		return m_guard;
	}

	// True when a draw writing `writes` must wait for its pipeline instead of being skipped.
	[[nodiscard]] bool MustWait(std::span<const Range> writes) const {
		std::lock_guard lock(m_mutex);
		if (!m_guard) return false;
		return std::ranges::any_of(writes, [&](const Range& w) { return m_unsafe.Overlaps(w); });
	}

	[[nodiscard]] GuardCounts GetGuardCounts() const {
		std::lock_guard lock(m_mutex);
		return m_guard_counts;
	}
	[[nodiscard]] size_t UnsafeRangeCount() const {
		std::lock_guard lock(m_mutex);
		return m_unsafe.Size();
	}
	[[nodiscard]] bool UnsafeSaturated() const {
		std::lock_guard lock(m_mutex);
		return m_unsafe.Saturated();
	}

	[[nodiscard]] bool Active() const { return m_active.load(std::memory_order_relaxed) != 0; }

	// A draw of pipeline `key` (shader pair vs/ps) was dropped; `writes` is what it would have written.
	// Draws of the same key writing the same ranges share one entry. Returns the root entry id (0 when
	// the table is full).
	uint32_t NoteSkip(uint64_t key, uint64_t vs, uint64_t ps, std::span<const Range> writes,
	                  int64_t now_ns) {
		std::lock_guard lock(m_mutex);
		m_totals.skip_events++;
		for (auto& e: m_entries) {
			if (e.root && !e.stats.resolved && e.stats.key == key && SameRanges(e.ranges, writes)) {
				e.stats.skips++;
				return e.stats.id;
			}
		}
		if (m_entries.size() >= kMaxEntries) {
			m_totals.dropped_entries++;
			return 0;
		}
		Entry e;
		e.root         = true;
		e.stats.id     = m_next_id++;
		e.stats.key    = key;
		e.stats.vs     = vs;
		e.stats.ps     = ps;
		e.stats.skips  = 1;
		e.stats.first_ns = now_ns;
		e.ranges.assign(writes.begin(), writes.end());
		e.roots.push_back(e.stats.id);
		m_totals.entries++;
		m_entries.push_back(std::move(e));
		m_active.store(static_cast<uint32_t>(m_entries.size()), std::memory_order_relaxed);
		return m_entries.back().stats.id;
	}

	// A draw of pipeline `key` ran (the pipeline is ready): its skipped outputs stop being poison.
	// Returns the stats of the roots that were resolved.
	std::vector<RootStats> NoteReady(uint64_t key, int64_t now_ns) {
		std::vector<RootStats> done;
		std::lock_guard lock(m_mutex);
		std::vector<uint32_t> gone;
		for (auto& e: m_entries) {
			if (e.root && e.stats.key == key) {
				e.stats.resolved = true;
				e.stats.ready_ns = now_ns;
				done.push_back(e.stats);
				gone.push_back(e.stats.id);
				m_totals.resolved++;
				for (size_t k = 0; k < kSinks; k++) m_totals.dependent[k] += e.stats.dependent[k];
			}
		}
		if (gone.empty()) return done;
		// Roots that stay: a taint survives while any of its roots does.
		std::vector<uint32_t> alive;
		for (const auto& e: m_entries) {
			if (e.root && !e.stats.resolved) alive.push_back(e.stats.id);
		}
		std::erase_if(m_entries, [&](const Entry& e) {
			if (e.root) return e.stats.resolved;
			return std::ranges::none_of(e.roots, [&](uint32_t root) {
				return std::ranges::find(alive, root) != alive.end();
			});
		});
		m_active.store(static_cast<uint32_t>(m_entries.size()), std::memory_order_relaxed);
		return done;
	}

	// A reader of `range`. `tag` identifies the reader (shader hash or an op id) for the first-hit
	// dedupe. `hits` is appended to. Returns true when the range hit poison.
	bool NoteRead(const Range& range, ReadKind kind, uint64_t tag, std::vector<Hit>& hits) {
		if (range.size == 0) return false;
		std::lock_guard lock(m_mutex);
		bool any = false;
		for (auto& e: m_entries) {
			if (!OverlapsAny(e.ranges, range)) continue;
			any = true;
			Hit hit;
			hit.entry   = e.stats.id;
			hit.derived = !e.root;
			hit.depth   = e.depth;
			hit.first   = m_seen.emplace(e.stats.id, static_cast<uint8_t>(kind), tag).second;
			hit.root_count = static_cast<uint32_t>(std::min(e.roots.size(), kMaxRoots));
			for (uint32_t i = 0; i < hit.root_count; i++) hit.roots[i] = e.roots[i];
			for (const auto root: e.roots) {
				auto* r = FindRootLocked(root);
				if (r == nullptr) continue;
				if (hit.root_vs == 0 && hit.root_ps == 0) {
					hit.root_vs = r->stats.vs;
					hit.root_ps = r->stats.ps;
				}
				(e.root ? r->stats.reads : r->stats.transitive_reads)[static_cast<size_t>(kind)]++;
				MarkDependent(*r, SinkOf(kind));
			}
			if (m_guard && IsHardRead(kind)) LearnLocked(e, range, kind, hit);
			hits.push_back(hit);
		}
		return any;
	}

	// --skip-guard, the blind class: a shader with 64-bit address loads (or computed stores) read the
	// poison `hits`, and what it dereferences through addresses is invisible to this tracker. The
	// outputs of the roots it hit become unsafe. Not a miss: nothing is known to have been read.
	// Returns the ranges newly made unsafe.
	std::vector<Range> NoteBdaSuspect(std::span<const Hit> hits) {
		std::vector<Range> added;
		std::lock_guard lock(m_mutex);
		if (!m_guard_bda) return added;
		std::vector<uint32_t> roots;
		for (const auto& h: hits) {
			for (uint32_t i = 0; i < h.root_count; i++) {
				if (std::ranges::find(roots, h.roots[i]) == roots.end()) roots.push_back(h.roots[i]);
			}
		}
		for (const auto id: roots) {
			const auto* r = FindRootLocked(id);
			if (r == nullptr) continue;
			for (const auto& range: r->ranges) {
				if (m_unsafe.Add(range)) {
					added.push_back(range);
					m_guard_counts.bda_ranges++;
				}
			}
		}
		return added;
	}

	// A pass that read poison wrote `writes`: they derive from the same roots (a taint entry per
	// distinct range set). `sources` are the entry ids the pass hit.
	void NoteTaint(std::span<const Hit> sources, std::span<const Range> writes) {
		if (sources.empty() || writes.empty()) return;
		std::lock_guard lock(m_mutex);
		std::vector<uint32_t> roots;
		uint32_t              depth = 0;
		for (const auto& s: sources) {
			for (uint32_t i = 0; i < s.root_count; i++) {
				if (std::ranges::find(roots, s.roots[i]) == roots.end() && roots.size() < kMaxRoots)
					roots.push_back(s.roots[i]);
			}
			depth = std::max(depth, s.depth + 1);
		}
		if (depth > kMaxDepth) return;
		for (const auto& w: writes) {
			if (w.size == 0) continue;
			// Writing a range that is itself poison from the same roots adds nothing.
			Entry* existing = nullptr;
			for (auto& e: m_entries) {
				if (!e.root && e.ranges.size() == 1 && e.ranges[0] == w) {
					existing = &e;
					break;
				}
			}
			if (existing != nullptr) {
				for (const auto root: roots) {
					if (std::ranges::find(existing->roots, root) == existing->roots.end() &&
					    existing->roots.size() < kMaxRoots)
						existing->roots.push_back(root);
				}
				existing->depth = std::min(existing->depth, depth);
				continue;
			}
			if (m_entries.size() >= kMaxEntries) {
				m_totals.dropped_entries++;
				continue;
			}
			Entry e;
			e.root         = false;
			e.stats.id     = m_next_id++;
			e.depth        = depth;
			e.ranges.push_back(w);
			e.roots = roots;
			m_entries.push_back(std::move(e));
		}
		m_active.store(static_cast<uint32_t>(m_entries.size()), std::memory_order_relaxed);
	}

	// Someone (the guest CPU, a DMA fill) wrote `range` completely: poison entirely inside it is gone.
	// Only taints are dropped; a root stays until its pipeline is ready (partial overwrites are not
	// provable).
	void NoteOverwrite(const Range& range) {
		std::lock_guard lock(m_mutex);
		std::erase_if(m_entries, [&](const Entry& e) {
			return !e.root && std::ranges::all_of(e.ranges, [&](const Range& r) { return Covers(range, r); });
		});
		m_active.store(static_cast<uint32_t>(m_entries.size()), std::memory_order_relaxed);
	}

	[[nodiscard]] Totals GetTotals() const {
		std::lock_guard lock(m_mutex);
		auto totals = m_totals;
		// Roots still waiting for their pipeline count with what they have so far.
		for (const auto& e: m_entries) {
			if (e.root) {
				for (size_t k = 0; k < kSinks; k++) totals.dependent[k] += e.stats.dependent[k];
			}
		}
		return totals;
	}

	// Roots still unresolved (end of run summary).
	[[nodiscard]] std::vector<RootStats> OpenRoots() const {
		std::vector<RootStats> out;
		std::lock_guard lock(m_mutex);
		for (const auto& e: m_entries) {
			if (e.root) out.push_back(e.stats);
		}
		return out;
	}

	[[nodiscard]] size_t EntryCount() const {
		std::lock_guard lock(m_mutex);
		return m_entries.size();
	}

private:
	static constexpr uint32_t kMaxDepth = 8;

	struct Entry {
		bool                  root  = true;
		uint32_t              depth = 0;
		RootStats             stats; // taints use only .id
		std::vector<Range>    ranges;
		std::vector<uint32_t> roots;
		std::array<uint32_t, kSinks> marked {};
		uint32_t              hard_marked = 0; // skip events already counted as misses
	};

	static bool SameRanges(const std::vector<Range>& a, std::span<const Range> b) {
		return a.size() == b.size() && std::ranges::equal(a, b);
	}

	static bool OverlapsAny(const std::vector<Range>& ranges, const Range& range) {
		return std::ranges::any_of(ranges, [&](const Range& r) { return Overlaps(r, range); });
	}

	Entry* FindRootLocked(uint32_t id) {
		for (auto& e: m_entries) {
			if (e.root && e.stats.id == id) return &e;
		}
		return nullptr;
	}

	// A hard-class read hit entry `e`: the roots' output ranges it touched (all of them, for a
	// taint: the read range is not the draw's output) become unsafe, and skip events that had not
	// yet been counted as read by a hard reader become misses.
	void LearnLocked(const Entry& e, const Range& range, ReadKind kind, Hit& hit) {
		hit.hard = true;
		m_guard_counts.hard_hits[static_cast<size_t>(kind)]++;
		const auto add = [&](const Range& r) {
			if (m_unsafe.Add(r)) {
				if (hit.unsafe_added++ == 0) hit.unsafe_first = r;
				m_guard_counts.hard_ranges++;
			}
		};
		if (e.root) {
			for (const auto& r: e.ranges) {
				if (Overlaps(r, range)) add(r);
			}
		}
		for (const auto id: e.roots) {
			auto* root = FindRootLocked(id);
			if (root == nullptr) continue;
			if (!e.root) {
				for (const auto& r: root->ranges) add(r);
			}
			const uint32_t fresh = root->stats.skips - root->hard_marked;
			if (fresh != 0) {
				if (root->hard_marked == 0) m_guard_counts.hard_roots++;
				root->hard_marked = root->stats.skips;
				root->stats.misses += fresh;
				hit.new_misses += fresh;
				m_guard_counts.misses += fresh;
				if (!e.root) m_guard_counts.misses_derived += fresh;
			}
		}
		hit.newly_unsafe = hit.unsafe_added != 0;
	}

	// Every skip event so far had a reader of this sink class (and the classes below it).
	static void MarkDependent(Entry& root, Sink sink) {
		for (size_t k = 0; k <= static_cast<size_t>(sink); k++) {
			root.stats.dependent[k] += root.stats.skips - root.marked[k];
			root.marked[k] = root.stats.skips;
		}
	}

	mutable std::mutex                                     m_mutex;
	std::vector<Entry>                                     m_entries;
	std::set<std::tuple<uint32_t, uint8_t, uint64_t>>      m_seen;
	Totals                                                 m_totals;
	bool                                                   m_guard     = false;
	bool                                                   m_guard_bda = false;
	RangeSet                                               m_unsafe;
	GuardCounts                                            m_guard_counts;
	uint32_t                                               m_next_id = 1;
	std::atomic<uint32_t>                                  m_active {0};
};

} // namespace Libs::Graphics::SkipDeps

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SKIPDEPENDENCIES_H_ */
