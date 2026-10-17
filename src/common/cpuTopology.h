#ifndef KYTY_COMMON_CPUTOPOLOGY_H_
#define KYTY_COMMON_CPUTOPOLOGY_H_

// Host CPU topology and the placement plan of --thread-placement (common/threadPlacement.h).
//
// Everything here is pure (no system calls), so it is unit-tested with fake file trees and fake
// Windows records (tests/ThreadPlacementTests.cpp). The system-facing half reads the real files
// (Linux: /sys/devices/system/cpu) or calls GetLogicalProcessorInformationEx (Windows) and hands the
// result to the functions below.
//
// Why a plan: the GPU thread (kyGpu) and the look-ahead sequencer are serial and their speed is the
// frame time. Left to the scheduler, kyGpu hops over every logical CPU and shares its physical core
// with whatever runs on the SMT sibling. The plan gives each of them a physical core of its own (all
// its logical CPUs, so the thread may still move between siblings at no cache cost) and keeps every
// other thread of the process off those cores.
//
// Nothing is tuned to one CPU. The plan is derived from what the machine reports:
//   * physical cores = groups of logical CPUs that share a core (SMT siblings);
//   * the number of dedicated cores scales with the core count: one per four physical cores, at
//     most two (one per serial thread), and at least three cores always stay for everything else;
//     fewer than four physical cores: no plan;
//   * dedicated cores come from one last-level-cache domain (the one with most fast cores), the
//     fastest first (capacity, CPPC highest performance or maximum frequency, whichever the kernel
//     reports for every CPU; hybrid parts: cores below 60 % of the best are never dedicated), and
//     away from the core of the lowest CPU number, where interrupts and housekeeping land by default.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Common::CpuTopology {

// Largest logical CPU index the code handles (Linux cpu_set_t size). Windows indexes a CPU as
// group * 64 + bit.
constexpr uint32_t kMaxCpus = 1024;

// Fewer physical cores than this: no plan (a dedicated core would be a quarter of the machine or more).
constexpr uint32_t kMinPhysicalCores = 4;
// Cores that always stay for the rest of the process.
constexpr uint32_t kMinRestCores = 3;
// A core below this share (percent) of the best core's performance is a small core of a hybrid CPU.
constexpr uint32_t kSlowCorePercent = 60;
// Dedicated cores: kyGpu and the look-ahead sequencer.
constexpr uint32_t kMaxDedicatedCores = 2;

// One physical core: its logical CPUs (SMT siblings), ascending.
struct Core {
	std::vector<uint32_t> cpus;
	uint32_t              cache_group = 0; // cores that share a last-level cache have the same value
	uint64_t              perf        = 0; // 0 = unknown; only the ratios matter
};

struct Topology {
	std::vector<Core> cores;
	[[nodiscard]] bool Empty() const { return cores.empty(); }
};

// ---- parsing ---------------------------------------------------------------------------------

// A kernel CPU list: "0", "0,8", "0-3,8-11". Ascending, duplicates removed. False on garbage
// (the output is then unspecified). An empty list ("" or "\n") is valid and gives no CPUs.
[[nodiscard]] inline bool ParseCpuList(std::string_view text, std::vector<uint32_t>& out) {
	out.clear();
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
	size_t pos = 0;
	auto   number = [&](uint32_t& value) {
		const size_t start = pos;
		uint64_t     v     = 0;
		while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
			v = v * 10 + static_cast<uint64_t>(text[pos] - '0');
			if (v >= kMaxCpus) return false;
			pos++;
		}
		value = static_cast<uint32_t>(v);
		return pos > start;
	};
	while (pos < text.size()) {
		uint32_t first = 0, last = 0;
		if (!number(first)) return false;
		last = first;
		if (pos < text.size() && text[pos] == '-') {
			pos++;
			if (!number(last) || last < first) return false;
		}
		for (uint32_t c = first; c <= last; c++) out.push_back(c);
		if (pos < text.size()) {
			if (text[pos] != ',') return false;
			pos++;
			if (pos == text.size()) return false; // a trailing comma
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return true;
}

[[nodiscard]] inline std::optional<uint64_t> ParseNumber(std::string_view text) {
	size_t i = 0;
	while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) i++;
	uint64_t v = 0;
	size_t   start = i;
	while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
		v = v * 10 + static_cast<uint64_t>(text[i] - '0');
		i++;
	}
	if (i == start) return std::nullopt;
	return v;
}

// Reads a file of /sys/devices/system/cpu by its path relative to that directory
// ("online", "cpu3/topology/thread_siblings_list"). nullopt when it does not exist.
using FileReader = std::function<std::optional<std::string>(const std::string& relative_path)>;

namespace detail {

[[nodiscard]] inline std::optional<std::vector<uint32_t>> ReadList(const FileReader& read, const std::string& path) {
	auto text = read(path);
	if (!text) return std::nullopt;
	std::vector<uint32_t> list;
	if (!ParseCpuList(*text, list)) return std::nullopt;
	return list;
}

[[nodiscard]] inline std::string Key(const std::vector<uint32_t>& list) {
	std::string key;
	for (uint32_t c: list) key += std::to_string(c) + ",";
	return key;
}

// Per-CPU performance measure, in the order of preference. The first source that gives a value for
// every online CPU wins (mixing sources would mix scales); none: unknown (0) for all.
[[nodiscard]] inline std::map<uint32_t, uint64_t> ReadPerf(const FileReader& read, const std::vector<uint32_t>& online) {
	static const char* const kSources[] = {"cpu_capacity", "acpi_cppc/highest_perf", "cpufreq/cpuinfo_max_freq"};
	for (const char* source: kSources) {
		std::map<uint32_t, uint64_t> values;
		for (uint32_t cpu: online) {
			auto text = read("cpu" + std::to_string(cpu) + "/" + source);
			auto v    = text ? ParseNumber(*text) : std::nullopt;
			if (!v) break;
			values[cpu] = *v;
		}
		if (values.size() == online.size()) return values;
	}
	std::map<uint32_t, uint64_t> unknown;
	for (uint32_t cpu: online) unknown[cpu] = 0;
	return unknown;
}

} // namespace detail

// Reads the topology through `read`. Online CPUs only. Missing files degrade gracefully: no sibling
// information makes every CPU its own core, no cache information puts all cores in one cache group,
// no performance information makes all cores equal. Empty result when even the online list is missing.
[[nodiscard]] inline Topology ReadSysfs(const FileReader& read) {
	Topology topology;
	auto     online = detail::ReadList(read, "online");
	if (!online) online = detail::ReadList(read, "present");
	if (!online || online->empty()) return topology;
	const std::set<uint32_t> online_set(online->begin(), online->end());
	const auto               perf = detail::ReadPerf(read, *online);

	std::map<std::string, size_t> core_of_key; // core key -> index in topology.cores
	std::map<std::string, uint32_t> group_of_key;
	for (uint32_t cpu: *online) {
		const std::string base = "cpu" + std::to_string(cpu) + "/";

		// The core: the CPUs that list each other as thread siblings (core_cpus_list is the newer name).
		std::vector<uint32_t> siblings;
		auto                  list = detail::ReadList(read, base + "topology/thread_siblings_list");
		if (!list) list = detail::ReadList(read, base + "topology/core_cpus_list");
		if (list) {
			for (uint32_t s: *list) {
				if (online_set.count(s) != 0) siblings.push_back(s);
			}
		}
		std::string core_key;
		if (!siblings.empty()) {
			core_key = detail::Key(siblings);
		} else {
			auto package = read(base + "topology/physical_package_id");
			auto core_id = read(base + "topology/core_id");
			auto p       = package ? ParseNumber(*package) : std::nullopt;
			auto c       = core_id ? ParseNumber(*core_id) : std::nullopt;
			if (p && c) {
				core_key = "p" + std::to_string(*p) + "c" + std::to_string(*c);
			} else {
				core_key = "cpu" + std::to_string(cpu);
			}
		}

		// The cache domain: the largest data or unified cache the CPU reports.
		std::string group_key = "all";
		int         best_level = -1;
		for (int index = 0; index < 8; index++) {
			const std::string cache = base + "cache/index" + std::to_string(index) + "/";
			auto              level = read(cache + "level");
			auto              type  = read(cache + "type");
			if (!level) break;
			auto lv = ParseNumber(*level);
			if (!lv || (type && type->rfind("Instruction", 0) == 0)) continue;
			auto shared = detail::ReadList(read, cache + "shared_cpu_list");
			if (!shared || static_cast<int>(*lv) <= best_level) continue;
			std::vector<uint32_t> in_set;
			for (uint32_t s: *shared) {
				if (online_set.count(s) != 0) in_set.push_back(s);
			}
			best_level = static_cast<int>(*lv);
			group_key  = detail::Key(in_set);
		}
		// No cache information: the package is the next best domain.
		if (best_level < 0) {
			auto package = read(base + "topology/physical_package_id");
			auto p       = package ? ParseNumber(*package) : std::nullopt;
			if (p) group_key = "package" + std::to_string(*p);
		}

		auto it = core_of_key.find(core_key);
		if (it == core_of_key.end()) {
			it = core_of_key.emplace(core_key, topology.cores.size()).first;
			topology.cores.emplace_back();
		}
		Core& core = topology.cores[it->second];
		core.cpus.push_back(cpu);
		core.perf = std::max(core.perf, perf.at(cpu));
		auto git = group_of_key.find(group_key);
		if (git == group_of_key.end()) git = group_of_key.emplace(group_key, static_cast<uint32_t>(group_of_key.size())).first;
		core.cache_group = git->second;
	}
	for (auto& core: topology.cores) std::sort(core.cpus.begin(), core.cpus.end());
	std::sort(topology.cores.begin(), topology.cores.end(), [](const Core& a, const Core& b) { return a.cpus.front() < b.cpus.front(); });
	return topology;
}

// ---- Windows records -------------------------------------------------------------------------
// GetLogicalProcessorInformationEx(RelationAll) gives one record per core (RelationProcessorCore)
// and per cache (RelationCache). The Windows code copies the fields it needs into these and
// BuildFromWindows turns them into a Topology, so this part is tested on every host.

struct WindowsCoreRecord {
	uint16_t group           = 0;
	uint64_t mask            = 0; // the core's logical processors inside the group
	uint8_t  efficiency_class = 0; // higher = faster (hybrid CPUs); equal on all cores of a uniform CPU
};

struct WindowsCacheRecord {
	uint8_t  level = 0;
	bool     instruction = false;
	uint16_t group = 0;
	uint64_t mask  = 0; // the logical processors that share this cache, inside the group
};

[[nodiscard]] inline Topology BuildFromWindows(const std::vector<WindowsCoreRecord>& cores, const std::vector<WindowsCacheRecord>& caches) {
	Topology topology;
	auto     cpus_of = [](uint16_t group, uint64_t mask) {
        std::vector<uint32_t> cpus;
        for (uint32_t bit = 0; bit < 64; bit++) {
            if ((mask >> bit) & 1u) {
                const uint32_t id = static_cast<uint32_t>(group) * 64u + bit;
                if (id < kMaxCpus) cpus.push_back(id);
            }
        }
        return cpus;
	};
	int best_level = -1;
	for (const auto& cache: caches) {
		if (!cache.instruction) best_level = std::max(best_level, static_cast<int>(cache.level));
	}
	std::map<std::string, uint32_t> group_ids;
	for (const auto& record: cores) {
		Core core;
		core.cpus = cpus_of(record.group, record.mask);
		if (core.cpus.empty()) continue;
		core.perf = (static_cast<uint64_t>(record.efficiency_class) + 1) * 100;
		std::string key = "none";
		for (const auto& cache: caches) {
			if (cache.instruction || static_cast<int>(cache.level) != best_level) continue;
			if (cache.group == record.group && (cache.mask & record.mask) != 0) {
				key = detail::Key(cpus_of(cache.group, cache.mask));
				break;
			}
		}
		auto it = group_ids.find(key);
		if (it == group_ids.end()) it = group_ids.emplace(key, static_cast<uint32_t>(group_ids.size())).first;
		core.cache_group = it->second;
		topology.cores.push_back(std::move(core));
	}
	std::sort(topology.cores.begin(), topology.cores.end(), [](const Core& a, const Core& b) { return a.cpus.front() < b.cpus.front(); });
	return topology;
}

// ---- the plan --------------------------------------------------------------------------------

struct Options {
	// 0 = automatic (by core count); 1 = kyGpu only; 2 = kyGpu and the look-ahead sequencer.
	uint32_t dedicated_cores = 0;
};

struct Plan {
	bool                  ok = false;
	std::string           note; // why there is no plan, or a one-line description of it
	uint32_t              physical_cores  = 0;
	uint32_t              dedicated_cores = 0;
	std::vector<uint32_t> gpu;       // kyGpu's logical CPUs: all of one physical core
	std::vector<uint32_t> sequencer; // the look-ahead sequencer's core; empty when it has none
	std::vector<uint32_t> rest;      // every other thread: the allowed CPUs minus the dedicated cores
};

[[nodiscard]] inline std::string CpuListString(const std::vector<uint32_t>& cpus) {
	std::string text;
	for (uint32_t c: cpus) text += (text.empty() ? "" : "+") + std::to_string(c);
	return text.empty() ? "-" : text;
}

// Number of dedicated cores for a machine with `physical_cores`: one per four, at most one per
// serial thread, and kMinRestCores always stay for the rest. 0 = no plan.
[[nodiscard]] inline uint32_t AutomaticDedicatedCores(uint32_t physical_cores) {
	if (physical_cores < kMinPhysicalCores) return 0;
	uint32_t d = std::clamp<uint32_t>(physical_cores / 4, 1, kMaxDedicatedCores);
	while (d > 1 && physical_cores - d < kMinRestCores) d--;
	return d;
}

// `allowed`: the logical CPUs the process may run on (its affinity at start, so taskset and cpusets
// are respected). Cores keep only their allowed CPUs.
[[nodiscard]] inline Plan MakePlan(const Topology& topology, const std::vector<uint32_t>& allowed, const Options& options) {
	Plan plan;
	const std::set<uint32_t> allowed_set(allowed.begin(), allowed.end());
	std::vector<Core>        cores;
	for (const auto& core: topology.cores) {
		Core kept = core;
		kept.cpus.clear();
		for (uint32_t c: core.cpus) {
			if (allowed_set.count(c) != 0) kept.cpus.push_back(c);
		}
		if (!kept.cpus.empty()) cores.push_back(std::move(kept));
	}
	plan.physical_cores = static_cast<uint32_t>(cores.size());
	if (cores.size() < kMinPhysicalCores) {
		plan.note = std::to_string(cores.size()) + " physical cores usable (need " + std::to_string(kMinPhysicalCores) + ")";
		return plan;
	}

	uint64_t best = 0;
	for (const auto& core: cores) best = std::max(best, core.perf);
	auto fast = [&](const Core& core) { return best == 0 || core.perf == 0 || core.perf * 100 >= best * kSlowCorePercent; };

	uint32_t wanted = options.dedicated_cores != 0 ? std::min(options.dedicated_cores, kMaxDedicatedCores) : AutomaticDedicatedCores(plan.physical_cores);
	while (wanted > 1 && plan.physical_cores - wanted < kMinRestCores) wanted--;

	// The cache domain with the most fast cores (then the most performance, then the lowest number).
	std::map<uint32_t, std::pair<uint32_t, uint64_t>> groups; // group -> (fast cores, perf sum)
	for (const auto& core: cores) {
		if (!fast(core)) continue;
		auto& g = groups[core.cache_group];
		g.first++;
		g.second += core.perf;
	}
	if (groups.empty()) {
		plan.note = "no fast core";
		return plan;
	}
	uint32_t best_group = groups.begin()->first;
	std::pair<uint32_t, uint64_t> best_stats = groups.begin()->second;
	for (const auto& [id, stats]: groups) {
		if (stats.first > best_stats.first || (stats.first == best_stats.first && stats.second > best_stats.second)) {
			best_group = id;
			best_stats = stats;
		}
	}

	// Candidates: fast cores of that group first, then the other fast cores. Within each: the best
	// performance, then not the core of the lowest allowed CPU (interrupts), then the highest CPU.
	const uint32_t lowest_cpu = *allowed_set.begin();
	auto           has_lowest = [&](const Core& core) { return core.cpus.front() == lowest_cpu; };
	std::vector<const Core*> candidates;
	for (const auto& core: cores) {
		if (fast(core)) candidates.push_back(&core);
	}
	std::sort(candidates.begin(), candidates.end(), [&](const Core* a, const Core* b) {
		const bool a_in = a->cache_group == best_group, b_in = b->cache_group == best_group;
		if (a_in != b_in) return a_in;
		if (a->perf != b->perf) return a->perf > b->perf;
		if (has_lowest(*a) != has_lowest(*b)) return !has_lowest(*a);
		return a->cpus.front() > b->cpus.front();
	});
	wanted = std::min<uint32_t>(wanted, static_cast<uint32_t>(candidates.size()));
	if (wanted == 0) {
		plan.note = "no fast core";
		return plan;
	}
	plan.dedicated_cores = wanted;
	plan.gpu             = candidates[0]->cpus;
	if (wanted >= 2) plan.sequencer = candidates[1]->cpus;
	for (uint32_t c: allowed_set) {
		if (std::find(plan.gpu.begin(), plan.gpu.end(), c) == plan.gpu.end() && std::find(plan.sequencer.begin(), plan.sequencer.end(), c) == plan.sequencer.end()) {
			plan.rest.push_back(c);
		}
	}
	plan.ok   = true;
	plan.note = std::to_string(plan.physical_cores) + " physical cores: kyGpu on cpu " + CpuListString(plan.gpu) + ", sequencer on " +
	            (plan.sequencer.empty() ? std::string("the shared cores") : "cpu " + CpuListString(plan.sequencer)) + ", the rest on " + std::to_string(plan.rest.size()) +
	            " logical CPUs";
	return plan;
}

// ---- priority --------------------------------------------------------------------------------

// May an unprivileged thread move from nice `current` to nice `target`? Raising the nice value is
// always allowed; lowering it only down to 20 - RLIMIT_NICE. (RLIMIT_NICE is 0 on most
// distributions, so the answer is usually no; the placement then skips the priority change.)
[[nodiscard]] inline bool NiceAllowed(int current, int target, uint64_t rlimit_nice) {
	if (target >= current) return true;
	const int floor = 20 - static_cast<int>(std::min<uint64_t>(rlimit_nice, 40));
	return target >= floor;
}

} // namespace Common::CpuTopology

#endif /* KYTY_COMMON_CPUTOPOLOGY_H_ */
