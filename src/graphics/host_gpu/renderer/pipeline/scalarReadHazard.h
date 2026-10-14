#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SCALARREADHAZARD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SCALARREADHAZARD_H_

// The scalar-read hazard check of RenderExecutor::CommitBindings: a draw captures the guest memory
// ranges its shaders read while the resource tables are materialised (specialization reads, about
// 50 one-dword ranges per draw), and those ranges must not overlap anything the same draw writes
// (images and attachments it binds for writing, buffers its shaders write). The first version tested
// every read against every bound image and every written buffer, looking each image up again for
// every read: about 7 us per commit, 19 % of the GPU thread in a level, for a check that never fires.
//
// Here the written ranges are collected once into a sorted index with a running maximum of the range
// ends; one read is then answered with a binary search. The answer is exactly the one the nested
// loops gave (overlap is `read.address < w.end && w.address < read.end`), including which kind of
// write is reported first (images before buffers, reads in capture order) and the abort on an
// invalid range (zero-sized or wrapping read, wrapping written range) as long as a written range is
// present. tests/ScalarReadHazardTests.cpp checks the index against the nested loops on random sets.
//
// Pure data structure, no locking: one owner thread (the GPU thread).

#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// One scalar read: guest address and size in bytes (the form ResourceSnapshot::specialization_reads has).
using ScalarRead = std::pair<uint64_t, uint64_t>;

class WrittenRangeIndex {
public:
	// Forget the ranges; keeps the storage so a per-commit rebuild does not allocate.
	void Clear() {
		m_entries.clear();
		m_prefix_end.clear();
		m_invalid = false;
	}

	// Empty ranges are ignored, as the nested loops skipped `written.size == 0`. A range that wraps
	// around the address space is remembered as invalid.
	void Add(uint64_t address, uint64_t size) {
		if (size == 0) return;
		if (address > UINT64_MAX - size) {
			m_invalid = true;
			return;
		}
		m_entries.push_back({address, address + size});
	}

	// Must be called after the last Add and before any query.
	void Build() {
		std::sort(m_entries.begin(), m_entries.end(),
		          [](const Entry& left, const Entry& right) { return left.start < right.start; });
		m_prefix_end.resize(m_entries.size());
		uint64_t end = 0;
		for (size_t i = 0; i < m_entries.size(); ++i) {
			end             = std::max(end, m_entries[i].end);
			m_prefix_end[i] = end;
		}
	}

	[[nodiscard]] bool Empty() const { return m_entries.empty() && !m_invalid; }
	// The non-empty ranges added (sorted by start after Build).
	template <class Visit>
	[[nodiscard]] bool AnyEntry(Visit&& visit) const {
		for (const auto& entry: m_entries) {
			if (visit(entry.start, entry.end)) return true;
		}
		return false;
	}
	[[nodiscard]] bool HasInvalid() const { return m_invalid; }
	[[nodiscard]] size_t Count() const { return m_entries.size(); }

	// True when [address, address + size) overlaps a range. `size` must be non-zero and the read must
	// not wrap (see ReadIsValid).
	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size) const {
		const uint64_t read_end = address + size;
		// Ranges starting at or after the read's end cannot overlap it; among the others the one that
		// ends last decides.
		const auto first_after = std::partition_point(
		    m_entries.begin(), m_entries.end(),
		    [read_end](const Entry& entry) { return entry.start < read_end; });
		const auto count = static_cast<size_t>(first_after - m_entries.begin());
		return count != 0 && m_prefix_end[count - 1] > address;
	}

private:
	struct Entry {
		uint64_t start;
		uint64_t end;
	};
	std::vector<Entry>    m_entries;
	std::vector<uint64_t> m_prefix_end;
	bool                  m_invalid = false;
};

[[nodiscard]] inline bool ScalarReadIsValid(const ScalarRead& read) {
	return read.second != 0 && read.first <= UINT64_MAX - read.second;
}

enum class ScalarReadHazardKind {
	None,
	ImageWrite,   // a read overlaps an image or attachment the draw writes
	BufferWrite,  // a read overlaps a buffer a shader of the draw writes
	InvalidRange, // a zero-sized or wrapping range met a written range (the nested loops aborted)
};

struct ScalarReadHazard {
	ScalarReadHazardKind kind       = ScalarReadHazardKind::None;
	size_t               read_index = 0; // the first read, in capture order, that found one
};

// The first read (capture order) that overlaps an image write, else a buffer write, or an invalid
// range met a non-empty set. An empty index matches nothing, and also never validates the read: the
// nested loops only checked a range when there was a written range to compare it with.
[[nodiscard]] inline ScalarReadHazard FindScalarReadHazard(std::span<const ScalarRead> reads,
                                                           const WrittenRangeIndex& image_writes,
                                                           const WrittenRangeIndex& buffer_writes) {
	if (image_writes.Empty() && buffer_writes.Empty()) return {};
	for (size_t i = 0; i < reads.size(); ++i) {
		const bool valid = ScalarReadIsValid(reads[i]);
		for (const auto* set: {&image_writes, &buffer_writes}) {
			const auto kind = set == &image_writes ? ScalarReadHazardKind::ImageWrite
			                                       : ScalarReadHazardKind::BufferWrite;
			if (set->Empty()) continue;
			if (set->HasInvalid() || !valid) return {ScalarReadHazardKind::InvalidRange, i};
			if (set->Overlaps(reads[i].first, reads[i].second)) return {kind, i};
		}
	}
	return {};
}

// ---- resolved draw: the read hull -------------------------------------------------------------------

// The union of a snapshot's scalar reads as sorted disjoint intervals. A zero-sized or wrapping read
// makes the hull unusable (`has_invalid`), exactly the reads FindScalarReadHazard reports as invalid.
inline void BuildReadHull(std::span<const ScalarRead> reads, ShaderRecompiler::IR::ReadHull& hull) {
	hull.intervals.clear();
	hull.has_invalid = false;
	hull.valid       = true;
	hull.intervals.reserve(reads.size());
	for (const auto& read: reads) {
		if (!ScalarReadIsValid(read)) {
			hull.has_invalid = true;
			hull.intervals.clear();
			return;
		}
		hull.intervals.push_back({read.first, read.first + read.second});
	}
	std::sort(hull.intervals.begin(), hull.intervals.end(),
	          [](const auto& a, const auto& b) { return a.begin < b.begin; });
	size_t out = 0;
	for (size_t i = 0; i < hull.intervals.size(); ++i) {
		if (out != 0 && hull.intervals[i].begin <= hull.intervals[out - 1].end) {
			hull.intervals[out - 1].end = std::max(hull.intervals[out - 1].end, hull.intervals[i].end);
		} else {
			hull.intervals[out++] = hull.intervals[i];
		}
	}
	hull.intervals.resize(out);
}

// True when the hull proves that FindScalarReadHazard(reads, image_writes, buffer_writes) returns
// None: no written range overlaps any read, and neither written set nor the reads are invalid.
// False means "no proof" (an overlap, an invalid range or no hull): the caller runs the full check,
// which also reports which read and which kind. Exact: a read overlaps [a, b) exactly when the union
// of the reads does.
[[nodiscard]] inline bool HullProvesNoHazard(const ShaderRecompiler::IR::ReadHull& hull,
                                             const WrittenRangeIndex&               image_writes,
                                             const WrittenRangeIndex&               buffer_writes) {
	if (!hull.valid || hull.has_invalid || image_writes.HasInvalid() || buffer_writes.HasInvalid()) {
		return false;
	}
	const auto overlaps = [&hull](uint64_t start, uint64_t end) {
		// First interval ending after `start`; it overlaps when it begins before `end`.
		const auto it = std::partition_point(hull.intervals.begin(), hull.intervals.end(),
		                                     [start](const auto& i) { return i.end <= start; });
		return it != hull.intervals.end() && it->begin < end;
	};
	return !image_writes.AnyEntry(overlaps) && !buffer_writes.AnyEntry(overlaps);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SCALARREADHAZARD_H_
