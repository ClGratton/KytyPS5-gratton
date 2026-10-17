// Thread placement (common/cpuTopology.h): topology parsing and the placement plan, on fake
// /sys/devices/system/cpu trees and fake Windows processor records. The runtime half
// (common/threadPlacement.cpp) only applies the plan with system calls.

#include "common/cpuTopology.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace Common::CpuTopology;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ThreadPlacementTests: failed: %s\n", message);
		std::abort();
	}
}

using Cpus = std::vector<uint32_t>;

bool Contains(const Cpus& list, uint32_t cpu) {
	return std::find(list.begin(), list.end(), cpu) != list.end();
}

// ---- a fake /sys/devices/system/cpu ----------------------------------------------------------

struct Fake {
	std::map<std::string, std::string> files;

	FileReader Reader() const {
		return [this](const std::string& path) -> std::optional<std::string> {
			auto it = files.find(path);
			if (it == files.end()) return std::nullopt;
			return it->second;
		};
	}

	void Set(const std::string& path, const std::string& text) { files[path] = text + "\n"; }

	// One logical CPU. siblings: its thread_siblings_list; l3: the shared_cpu_list of its level-3 cache
	// (empty = no cache information); perf: cpu_capacity (0 = none); cppc / freq likewise.
	void Cpu(uint32_t id, const std::string& siblings, const std::string& l3, uint64_t capacity = 0, uint64_t cppc = 0, uint64_t freq = 0, int package = 0, int core_id = -1) {
		const std::string base = "cpu" + std::to_string(id) + "/";
		if (!siblings.empty()) Set(base + "topology/thread_siblings_list", siblings);
		if (package >= 0) Set(base + "topology/physical_package_id", std::to_string(package));
		if (core_id >= 0) Set(base + "topology/core_id", std::to_string(core_id));
		if (!l3.empty()) {
			Set(base + "cache/index0/level", "1");
			Set(base + "cache/index0/type", "Data");
			Set(base + "cache/index0/shared_cpu_list", siblings.empty() ? std::to_string(id) : siblings);
			Set(base + "cache/index1/level", "1");
			Set(base + "cache/index1/type", "Instruction");
			Set(base + "cache/index1/shared_cpu_list", siblings.empty() ? std::to_string(id) : siblings);
			Set(base + "cache/index2/level", "2");
			Set(base + "cache/index2/type", "Unified");
			Set(base + "cache/index2/shared_cpu_list", siblings.empty() ? std::to_string(id) : siblings);
			Set(base + "cache/index3/level", "3");
			Set(base + "cache/index3/type", "Unified");
			Set(base + "cache/index3/shared_cpu_list", l3);
		}
		if (capacity != 0) Set(base + "cpu_capacity", std::to_string(capacity));
		if (cppc != 0) Set(base + "acpi_cppc/highest_perf", std::to_string(cppc));
		if (freq != 0) Set(base + "cpufreq/cpuinfo_max_freq", std::to_string(freq));
	}
};

std::string Pair(uint32_t a, uint32_t b) {
	return std::to_string(a) + "," + std::to_string(b);
}

// n physical cores with SMT, siblings i and i + n (the numbering of many AMD and Intel parts), one L3.
Fake Smt(uint32_t n, uint32_t first_cpu = 0, const std::string& l3 = "") {
	Fake f;
	std::string all = l3;
	if (all.empty()) all = std::to_string(first_cpu) + "-" + std::to_string(first_cpu + n - 1) + "," + std::to_string(first_cpu + n) + "-" + std::to_string(first_cpu + 2 * n - 1);
	for (uint32_t i = 0; i < n; i++) {
		f.Cpu(first_cpu + i, Pair(first_cpu + i, first_cpu + n + i), all);
		f.Cpu(first_cpu + n + i, Pair(first_cpu + i, first_cpu + n + i), all);
	}
	return f;
}

Cpus Range(uint32_t first, uint32_t count) {
	Cpus c;
	for (uint32_t i = 0; i < count; i++) c.push_back(first + i);
	return c;
}

// ---- tests -----------------------------------------------------------------------------------

void TestParseCpuList() {
	Cpus c;
	Check(ParseCpuList("0", c) && c == Cpus {0}, "single");
	Check(ParseCpuList("0,8", c) && c == (Cpus {0, 8}), "pair");
	Check(ParseCpuList("0-3,8-11\n", c) && c == (Cpus {0, 1, 2, 3, 8, 9, 10, 11}), "ranges and a trailing newline");
	Check(ParseCpuList("8,0-1,1", c) && c == (Cpus {0, 1, 8}), "sorted, duplicates removed");
	Check(ParseCpuList("", c) && c.empty(), "empty list");
	Check(ParseCpuList("\n", c) && c.empty(), "empty list with newline");
	Check(!ParseCpuList("a", c), "garbage");
	Check(!ParseCpuList("3-1", c), "backwards range");
	Check(!ParseCpuList("1,,2", c), "empty element");
	Check(!ParseCpuList("1,", c), "trailing comma");
	Check(!ParseCpuList("1 2", c), "space inside");
	Check(!ParseCpuList("2000", c), "above the limit");
	Check(ParseNumber("  1024\n") == 1024u, "ParseNumber");
	Check(!ParseNumber("x").has_value() && !ParseNumber("").has_value(), "ParseNumber garbage");
}

// An 8-core, 16-thread part with one L3 (siblings i and i + 8).
void TestEightCoreSmt() {
	Fake f = Smt(8);
	// CPPC "highest performance" differs a little per core (preferred cores); cores 5 and 2 are the best.
	const uint64_t cppc[8] = {196, 190, 201, 188, 194, 203, 199, 186};
	for (uint32_t i = 0; i < 8; i++) {
		f.Set("cpu" + std::to_string(i) + "/acpi_cppc/highest_perf", std::to_string(cppc[i]));
		f.Set("cpu" + std::to_string(i + 8) + "/acpi_cppc/highest_perf", std::to_string(cppc[i]));
	}
	f.Set("online", "0-15");
	const Topology t = ReadSysfs(f.Reader());
	Check(t.cores.size() == 8, "eight physical cores");
	Check(t.cores[0].cpus == (Cpus {0, 8}) && t.cores[7].cpus == (Cpus {7, 15}), "siblings grouped, cores ordered");
	Check(t.cores[0].cache_group == t.cores[7].cache_group, "one L3");
	Check(t.cores[5].perf == 203, "CPPC performance read");

	const Plan p = MakePlan(t, Range(0, 16), {});
	Check(p.ok, "plan on 8 cores");
	Check(p.dedicated_cores == 2, "two dedicated cores on eight");
	Check(p.gpu == (Cpus {5, 13}), "kyGpu on the best core");
	Check(p.sequencer == (Cpus {2, 10}), "sequencer on the second best (201)");
	Check(p.rest.size() == 12, "twelve logical CPUs left");
	for (uint32_t c: p.gpu) Check(!Contains(p.rest, c) && !Contains(p.sequencer, c), "kyGpu's CPUs are exclusive");
	for (uint32_t c: p.sequencer) Check(!Contains(p.rest, c), "sequencer's CPUs are exclusive");
}

// Equal performance everywhere: the core of CPU 0 (interrupts) is avoided and the highest cores win.
void TestEqualCores() {
	Fake f = Smt(8);
	f.Set("online", "0-15");
	const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 16), {});
	Check(p.ok && p.gpu == (Cpus {7, 15}) && p.sequencer == (Cpus {6, 14}), "highest cores, away from cpu 0");
	Check(!Contains(p.gpu, 0) && !Contains(p.sequencer, 0), "never the core of cpu 0 while others exist");
}

void TestThresholdsScale() {
	Check(AutomaticDedicatedCores(0) == 0 && AutomaticDedicatedCores(2) == 0 && AutomaticDedicatedCores(3) == 0, "under four cores: none");
	Check(AutomaticDedicatedCores(4) == 1 && AutomaticDedicatedCores(6) == 1 && AutomaticDedicatedCores(7) == 1, "four to seven: one");
	Check(AutomaticDedicatedCores(8) == 2 && AutomaticDedicatedCores(12) == 2 && AutomaticDedicatedCores(64) == 2, "eight and up: two");
	for (uint32_t cores: {4u, 6u, 8u, 12u, 16u}) {
		Fake f = Smt(cores);
		f.Set("online", "0-" + std::to_string(2 * cores - 1));
		const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 2 * cores), {});
		Check(p.ok, "plan");
		Check(p.dedicated_cores == AutomaticDedicatedCores(cores), "dedicated cores follow the rule");
		Check(p.rest.size() >= kMinRestCores * 2, "at least three cores stay for the rest");
		Check(p.sequencer.empty() == (p.dedicated_cores == 1), "sequencer core only with two");
	}
	{
		Fake f = Smt(3);
		f.Set("online", "0-5");
		const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 6), {});
		Check(!p.ok && !p.note.empty(), "three cores: no plan, with a reason");
	}
	{
		Fake f = Smt(4);
		f.Set("online", "0-7");
		Options o;
		o.dedicated_cores = 2;
		const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 8), o);
		Check(p.ok && p.dedicated_cores == 1, "two requested on four cores: three must stay, so one");
	}
	{
		Fake f = Smt(8);
		f.Set("online", "0-15");
		Options o;
		o.dedicated_cores = 1;
		const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 16), o);
		Check(p.ok && p.dedicated_cores == 1 && p.sequencer.empty() && p.rest.size() == 14, "one requested on eight");
		o.dedicated_cores = 9;
		const Plan q = MakePlan(ReadSysfs(f.Reader()), Range(0, 16), o);
		Check(q.ok && q.dedicated_cores == 2, "more than two requested: two");
	}
}

// Two core complexes with their own L3: both dedicated cores come from the same one.
void TestTwoCacheDomains() {
	Fake f;
	for (uint32_t i = 0; i < 16; i++) {
		const bool second = i >= 8;
		const std::string l3 = second ? "8-15,24-31" : "0-7,16-23";
		const std::string siblings = Pair(i, i + 16);
		f.Cpu(i, siblings, l3);
		f.Cpu(i + 16, siblings, l3);
	}
	f.Set("online", "0-31");
	Topology t = ReadSysfs(f.Reader());
	Check(t.cores.size() == 16, "sixteen cores");
	Check(t.cores[0].cache_group != t.cores[8].cache_group, "two cache groups");
	Plan p = MakePlan(t, Range(0, 32), {});
	Check(p.ok && p.dedicated_cores == 2, "plan");
	// The sixteen cores tie on performance: the first group (lowest number) wins and both come from it.
	Check(p.gpu[0] < 8 && p.sequencer[0] < 8, "both dedicated cores in one cache domain");

	// The second complex is faster: it wins.
	for (uint32_t i = 8; i < 16; i++) {
		f.Set("cpu" + std::to_string(i) + "/acpi_cppc/highest_perf", "220");
		f.Set("cpu" + std::to_string(i + 16) + "/acpi_cppc/highest_perf", "220");
	}
	for (uint32_t i = 0; i < 8; i++) {
		f.Set("cpu" + std::to_string(i) + "/acpi_cppc/highest_perf", "200");
		f.Set("cpu" + std::to_string(i + 16) + "/acpi_cppc/highest_perf", "200");
	}
	t = ReadSysfs(f.Reader());
	p = MakePlan(t, Range(0, 32), {});
	Check(p.ok && p.gpu[0] >= 8 && p.sequencer[0] >= 8 && p.gpu[0] < 16 && p.sequencer[0] < 16, "the faster domain");
}

// Intel-style hybrid: six P-cores with SMT (cpus 0-11), eight E-cores without (cpus 12-19).
void TestHybrid() {
	Fake f;
	for (uint32_t i = 0; i < 6; i++) {
		const std::string s = Pair(2 * i, 2 * i + 1);
		f.Cpu(2 * i, s, "0-19", 1024);
		f.Cpu(2 * i + 1, s, "0-19", 1024);
	}
	for (uint32_t i = 12; i < 20; i++) f.Cpu(i, std::to_string(i), "0-19", 560);
	f.Set("online", "0-19");
	Topology t = ReadSysfs(f.Reader());
	Check(t.cores.size() == 14, "6 P + 8 E cores");
	Plan p = MakePlan(t, Range(0, 20), {});
	Check(p.ok && p.dedicated_cores == 2, "plan");
	for (uint32_t c: p.gpu) Check(c < 12, "kyGpu on a P-core");
	for (uint32_t c: p.sequencer) Check(c < 12, "sequencer on a P-core");
	Check(Contains(p.rest, 12) && Contains(p.rest, 19), "E-cores belong to the rest");
	// Only P-cores permitted by taskset but two of them: no plan (fewer than four physical cores).
	p = MakePlan(t, Cpus {0, 1, 2, 3}, {});
	Check(!p.ok, "two cores allowed");
	// E-cores at 70 % are fast enough to count, but a P-core is still the first choice.
	for (uint32_t i = 12; i < 20; i++) f.Set("cpu" + std::to_string(i) + "/cpu_capacity", "720");
	t = ReadSysfs(f.Reader());
	p = MakePlan(t, Range(0, 20), {});
	Check(p.ok && p.gpu[0] < 12 && p.sequencer[0] < 12, "P-cores first");
	// Four P-cores allowed with the E-cores: the third and fourth dedicated candidates would be E-cores, never asked for.
	Options o;
	o.dedicated_cores = 2;
	p = MakePlan(t, Range(0, 20), o);
	Check(p.ok && p.dedicated_cores == 2, "two");
}

// Cores are counted inside the allowed set (taskset, cpusets, containers).
void TestAllowedSubset() {
	Fake f = Smt(8);
	f.Set("online", "0-15");
	const Topology t = ReadSysfs(f.Reader());
	// Only the first hardware thread of every core: eight cores, one CPU each.
	Plan p = MakePlan(t, Range(0, 8), {});
	Check(p.ok && p.dedicated_cores == 2 && p.gpu.size() == 1 && p.rest.size() == 6, "one CPU per core");
	// Four cores allowed (both threads).
	p = MakePlan(t, Cpus {0, 1, 2, 3, 8, 9, 10, 11}, {});
	Check(p.ok && p.dedicated_cores == 1 && p.sequencer.empty() && p.rest.size() == 6, "four cores: one dedicated");
	for (uint32_t c: p.gpu) Check(Contains(Cpus {0, 1, 2, 3, 8, 9, 10, 11}, c), "inside the allowed set");
	// Two cores allowed: no plan.
	p = MakePlan(t, Cpus {0, 8, 1, 9}, {});
	Check(!p.ok && p.gpu.empty() && p.rest.empty(), "two cores: nothing planned");
	// An empty allowed set.
	p = MakePlan(t, {}, {});
	Check(!p.ok, "no CPUs");
}

void TestMissingInformation() {
	// Only the online list: every CPU is its own core, one cache group.
	Fake f;
	f.Set("online", "0-7");
	Topology t = ReadSysfs(f.Reader());
	Check(t.cores.size() == 8 && t.cores[0].cpus == Cpus {0} && t.cores[0].perf == 0, "no sibling information: eight cores");
	Plan p = MakePlan(t, Range(0, 8), {});
	Check(p.ok && p.dedicated_cores == 2 && p.gpu.size() == 1, "plan from nothing");
	// Core ids and packages without sibling lists.
	Fake g;
	g.Set("online", "0-7");
	for (uint32_t i = 0; i < 8; i++) g.Cpu(i, "", "", 0, 0, 0, 0, static_cast<int>(i % 4));
	t = ReadSysfs(g.Reader());
	Check(t.cores.size() == 4 && t.cores[0].cpus == (Cpus {0, 4}), "core ids pair the threads");
	// Nothing at all.
	Fake none;
	Check(ReadSysfs(none.Reader()).Empty(), "no online file: empty topology");
	// 'present' as a fallback.
	Fake h;
	h.Set("present", "0-3");
	Check(ReadSysfs(h.Reader()).cores.size() == 4, "present list");
	// Performance known for some CPUs only: unknown for all (the scales must not mix).
	Fake m = Smt(4);
	m.Set("online", "0-7");
	m.Set("cpu0/cpu_capacity", "900");
	t = ReadSysfs(m.Reader());
	for (const auto& core: t.cores) Check(core.perf == 0, "partial performance data is ignored");
	// Offline siblings are dropped from the core.
	Fake o = Smt(4);
	o.Set("online", "0-3,5-6");
	t = ReadSysfs(o.Reader());
	Check(t.cores.size() == 4, "online cores");
	bool found = false;
	for (const auto& core: t.cores) {
		if (core.cpus == Cpus {1, 5}) found = true;
		Check(!Contains(core.cpus, 4) && !Contains(core.cpus, 7), "offline CPUs removed");
	}
	Check(found, "core 1 keeps its online sibling");
}

void TestPerfSourcePreference() {
	Fake f = Smt(4);
	f.Set("online", "0-7");
	for (uint32_t i = 0; i < 8; i++) {
		f.Set("cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq", "5000000");
		f.Set("cpu" + std::to_string(i) + "/acpi_cppc/highest_perf", std::to_string(100 + i % 4));
	}
	Topology t = ReadSysfs(f.Reader());
	Check(t.cores[3].perf == 103, "CPPC beats the frequency");
	for (uint32_t i = 0; i < 8; i++) f.Set("cpu" + std::to_string(i) + "/cpu_capacity", std::to_string(500 + i % 4));
	t = ReadSysfs(f.Reader());
	Check(t.cores[3].perf == 503, "capacity beats CPPC");
}

// Windows: records from GetLogicalProcessorInformationEx.
void TestWindows() {
	std::vector<WindowsCoreRecord>  cores;
	std::vector<WindowsCacheRecord> caches;
	for (uint32_t i = 0; i < 8; i++) {
		WindowsCoreRecord c;
		c.group = 0;
		c.mask  = uint64_t {3} << (2 * i); // SMT siblings are adjacent bits
		c.efficiency_class = 0;
		cores.push_back(c);
		caches.push_back({1, false, 0, c.mask});
		caches.push_back({1, true, 0, c.mask});
		caches.push_back({2, false, 0, c.mask});
	}
	caches.push_back({3, false, 0, 0xFFFF});
	Topology t = BuildFromWindows(cores, caches);
	Check(t.cores.size() == 8 && t.cores[0].cpus == (Cpus {0, 1}) && t.cores[7].cpus == (Cpus {14, 15}), "cores from masks");
	for (const auto& core: t.cores) Check(core.cache_group == t.cores[0].cache_group, "the L3 is one group, not the per-core caches");
	Plan p = MakePlan(t, Range(0, 16), {});
	Check(p.ok && p.dedicated_cores == 2, "plan");
	Check(p.gpu == (Cpus {14, 15}) && p.sequencer == (Cpus {12, 13}), "highest cores");
	Check(p.rest.size() == 12, "rest");

	// Two cache domains (the L3 records split the cores) and a hybrid mix: classes 1 (P) and 0 (E).
	cores.clear();
	caches.clear();
	for (uint32_t i = 0; i < 12; i++) {
		WindowsCoreRecord c;
		c.group = 0;
		c.mask  = i < 6 ? (uint64_t {3} << (2 * i)) : (uint64_t {1} << (12 + (i - 6)));
		c.efficiency_class = i < 6 ? 1 : 0;
		cores.push_back(c);
	}
	caches.push_back({3, false, 0, 0x3FFFF});
	t = BuildFromWindows(cores, caches);
	Check(t.cores.size() == 12, "hybrid cores");
	p = MakePlan(t, Range(0, 18), {});
	Check(p.ok && p.gpu[0] < 12 && p.sequencer[0] < 12, "P-cores (class 1) chosen over E-cores (class 0)");

	// A second processor group.
	cores.clear();
	caches.clear();
	WindowsCoreRecord g1;
	g1.group = 1;
	g1.mask  = 0x3;
	cores.push_back(g1);
	t = BuildFromWindows(cores, caches);
	Check(t.cores.size() == 1 && t.cores[0].cpus == (Cpus {64, 65}), "group 1 starts at CPU 64");
	// Records without a mask are ignored.
	WindowsCoreRecord empty;
	cores.push_back(empty);
	Check(BuildFromWindows(cores, caches).cores.size() == 1, "empty record ignored");
}

void TestNiceAllowed() {
	Check(!NiceAllowed(0, -5, 0), "RLIMIT_NICE 0: no lowering");
	Check(!NiceAllowed(0, -5, 24), "24 reaches -4 only");
	Check(NiceAllowed(0, -5, 25), "25 reaches -5");
	Check(NiceAllowed(0, -5, 40), "40 reaches -20");
	Check(NiceAllowed(0, -20, 40), "-20 at 40");
	Check(!NiceAllowed(5, 0, 0), "back to 0 from 5 needs a limit too");
	Check(NiceAllowed(0, 5, 0), "raising the value is always allowed");
	Check(NiceAllowed(0, 0, 0), "no change");
	Check(NiceAllowed(0, -5, 1000), "huge limit is capped");
}

void TestPlanNotes() {
	Fake f = Smt(8);
	f.Set("online", "0-15");
	const Plan p = MakePlan(ReadSysfs(f.Reader()), Range(0, 16), {});
	Check(p.note.find("8 physical cores") != std::string::npos, "the note names the core count");
	Check(CpuListString({7, 15}) == "7+15" && CpuListString({}) == "-", "CpuListString");
}

} // namespace

int main() {
	TestParseCpuList();
	TestEightCoreSmt();
	TestEqualCores();
	TestThresholdsScale();
	TestTwoCacheDomains();
	TestHybrid();
	TestAllowedSubset();
	TestMissingInformation();
	TestPerfSourcePreference();
	TestWindows();
	TestNiceAllowed();
	TestPlanNotes();
	std::puts("ThreadPlacementTests: all passed");
	return 0;
}
