#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCYRANGES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCYRANGES_H_

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// Interval helpers of --mem-residency: lists of (address, size) ranges.
namespace Libs::Graphics::MemResidency {

using Range     = std::pair<uint64_t, uint64_t>; // address, size
using RangeList = std::vector<Range>;            // sorted and disjoint after Merge

// Rounds every range outwards to `granule`, sorts and joins touching or overlapping ranges.
inline RangeList Merge(RangeList ranges, uint64_t granule) {
	for (auto& [address, size]: ranges) {
		const uint64_t begin = address / granule * granule;
		const uint64_t end   = (address + size + granule - 1) / granule * granule;
		address              = begin;
		size                 = end - begin;
	}
	std::sort(ranges.begin(), ranges.end());
	RangeList out;
	for (const auto& [address, size]: ranges) {
		if (size == 0) {
			continue;
		}
		if (!out.empty() && address <= out.back().first + out.back().second) {
			const uint64_t end = std::max(out.back().first + out.back().second, address + size);
			out.back().second  = end - out.back().first;
		} else {
			out.emplace_back(address, size);
		}
	}
	return out;
}

inline RangeList Subtract(const RangeList& a, const RangeList& b) {
	RangeList out;
	size_t    j = 0;
	for (const auto& [address, size]: a) {
		uint64_t begin = address;
		const uint64_t end = address + size;
		while (j < b.size() && b[j].first + b[j].second <= begin) {
			j++;
		}
		size_t k = j;
		while (begin < end) {
			if (k >= b.size() || b[k].first >= end) {
				out.emplace_back(begin, end - begin);
				break;
			}
			if (b[k].first > begin) {
				out.emplace_back(begin, b[k].first - begin);
			}
			begin = std::max(begin, b[k].first + b[k].second);
			k++;
		}
	}
	return out;
}

inline RangeList Intersect(const RangeList& a, const RangeList& b) {
	RangeList out;
	size_t    i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		const uint64_t lo = std::max(a[i].first, b[j].first);
		const uint64_t hi = std::min(a[i].first + a[i].second, b[j].first + b[j].second);
		if (lo < hi) {
			out.emplace_back(lo, hi - lo);
		}
		if (a[i].first + a[i].second < b[j].first + b[j].second) {
			i++;
		} else {
			j++;
		}
	}
	return out;
}

inline RangeList Union(const RangeList& a, const RangeList& b) {
	RangeList all = a;
	all.insert(all.end(), b.begin(), b.end());
	return Merge(std::move(all), 1);
}

inline uint64_t TotalBytes(const RangeList& ranges) {
	uint64_t sum = 0;
	for (const auto& r: ranges) {
		sum += r.second;
	}
	return sum;
}

} // namespace Libs::Graphics::MemResidency

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMRESIDENCYRANGES_H_
