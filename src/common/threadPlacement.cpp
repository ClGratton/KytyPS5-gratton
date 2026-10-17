#include "common/threadPlacement.h"

#include "common/config.h"
#include "common/cpuTopology.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__linux__)
#define KYTY_PLACEMENT_LINUX 1
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define KYTY_PLACEMENT_WINDOWS 1
#include <windows.h>
// tlhelp32.h needs windows.h first
#include <tlhelp32.h>
#endif

namespace Common::ThreadPlacement {

namespace {

using Cpus = std::bitset<CpuTopology::kMaxCpus>;

// ---- platform layer --------------------------------------------------------------------------
// ThreadId(), ListThreads(), GetAffinity(), SetAffinity(), ReadTopology(), GetNice(), SetNice(),
// NiceLimit(), ProcessAffinity(): everything else in this file is portable.

#if defined(KYTY_PLACEMENT_LINUX)

constexpr bool kSupported = true;

uint32_t ThreadId() {
	return static_cast<uint32_t>(::syscall(SYS_gettid));
}

std::vector<uint32_t> ListThreads() {
	std::vector<uint32_t> tids;
	DIR*                  dir = ::opendir("/proc/self/task");
	if (dir == nullptr) return tids;
	while (const dirent* entry = ::readdir(dir)) {
		char* end = nullptr;
		const unsigned long v = std::strtoul(entry->d_name, &end, 10);
		if (end != entry->d_name && *end == '\0') tids.push_back(static_cast<uint32_t>(v));
	}
	::closedir(dir);
	return tids;
}

bool GetAffinity(uint32_t tid, Cpus& out) {
	cpu_set_t set;
	CPU_ZERO(&set);
	if (::sched_getaffinity(static_cast<pid_t>(tid), sizeof(set), &set) != 0) return false;
	out.reset();
	for (uint32_t c = 0; c < CpuTopology::kMaxCpus && c < CPU_SETSIZE; c++) {
		if (CPU_ISSET(c, &set)) out.set(c);
	}
	return true;
}

bool SetAffinity(uint32_t tid, const Cpus& cpus) {
	cpu_set_t set;
	CPU_ZERO(&set);
	for (uint32_t c = 0; c < CpuTopology::kMaxCpus && c < CPU_SETSIZE; c++) {
		if (cpus.test(c)) CPU_SET(c, &set);
	}
	return ::sched_setaffinity(static_cast<pid_t>(tid), sizeof(set), &set) == 0;
}

Cpus ProcessAffinity() {
	Cpus cpus;
	GetAffinity(ThreadId(), cpus);
	return cpus;
}

CpuTopology::Topology ReadTopology() {
	return CpuTopology::ReadSysfs([](const std::string& relative) -> std::optional<std::string> {
		std::ifstream in("/sys/devices/system/cpu/" + relative);
		if (!in) return std::nullopt;
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	});
}

bool GetNice(uint32_t tid, int& nice) {
	errno = 0;
	const int value = ::getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
	if (value == -1 && errno != 0) return false;
	nice = value;
	return true;
}

bool SetNice(uint32_t tid, int nice) {
	return ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0;
}

uint64_t NiceLimit() {
	rlimit limit {};
	if (::getrlimit(RLIMIT_NICE, &limit) != 0) return 0;
	return limit.rlim_cur == RLIM_INFINITY ? 40 : static_cast<uint64_t>(limit.rlim_cur);
}

void NameCurrentThread(const char* name) {
	::pthread_setname_np(::pthread_self(), name);
}

#elif defined(KYTY_PLACEMENT_WINDOWS)

// Windows (not tested): a thread belongs to one processor group of at most 64 logical processors.
// CPU index = group * 64 + bit. Machines with more than one group are not handled (kSupported stays
// true but ProcessAffinity() is empty there, which makes the plan refuse).

constexpr bool kSupported = true;

uint32_t ThreadId() {
	return static_cast<uint32_t>(::GetCurrentThreadId());
}

std::vector<uint32_t> ListThreads() {
	std::vector<uint32_t> tids;
	HANDLE                snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return tids;
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	const DWORD pid = ::GetCurrentProcessId();
	for (BOOL ok = ::Thread32First(snapshot, &entry); ok != FALSE; ok = ::Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID == pid) tids.push_back(static_cast<uint32_t>(entry.th32ThreadID));
	}
	::CloseHandle(snapshot);
	return tids;
}

HANDLE OpenForAffinity(uint32_t tid) {
	return ::OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, static_cast<DWORD>(tid));
}

bool GetAffinity(uint32_t tid, Cpus& out) {
	HANDLE handle = OpenForAffinity(tid);
	if (handle == nullptr) return false;
	GROUP_AFFINITY affinity {};
	const BOOL     ok = ::GetThreadGroupAffinity(handle, &affinity);
	::CloseHandle(handle);
	if (ok == FALSE) return false;
	out.reset();
	for (uint32_t bit = 0; bit < 64; bit++) {
		const uint32_t id = static_cast<uint32_t>(affinity.Group) * 64u + bit;
		if (((static_cast<uint64_t>(affinity.Mask) >> bit) & 1u) != 0 && id < CpuTopology::kMaxCpus) out.set(id);
	}
	return true;
}

bool SetAffinity(uint32_t tid, const Cpus& cpus) {
	// One group only: the group of the lowest CPU in the set.
	uint32_t first = CpuTopology::kMaxCpus;
	for (uint32_t c = 0; c < CpuTopology::kMaxCpus; c++) {
		if (cpus.test(c)) {
			first = c;
			break;
		}
	}
	if (first == CpuTopology::kMaxCpus) return false;
	GROUP_AFFINITY affinity {};
	affinity.Group = static_cast<WORD>(first / 64);
	uint64_t mask  = 0;
	for (uint32_t bit = 0; bit < 64; bit++) {
		const uint32_t id = static_cast<uint32_t>(affinity.Group) * 64u + bit;
		if (id < CpuTopology::kMaxCpus && cpus.test(id)) mask |= (uint64_t {1} << bit);
	}
	affinity.Mask = static_cast<KAFFINITY>(mask);
	HANDLE handle = OpenForAffinity(tid);
	if (handle == nullptr) return false;
	const BOOL ok = ::SetThreadGroupAffinity(handle, &affinity, nullptr);
	::CloseHandle(handle);
	return ok != FALSE;
}

Cpus ProcessAffinity() {
	Cpus cpus;
	if (::GetActiveProcessorGroupCount() != 1) return cpus; // more than 64 logical processors: not handled
	DWORD_PTR process = 0, system = 0;
	if (::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system) == FALSE) return cpus;
	for (uint32_t bit = 0; bit < 64; bit++) {
		if (((static_cast<uint64_t>(process) >> bit) & 1u) != 0) cpus.set(bit);
	}
	return cpus;
}

CpuTopology::Topology ReadTopology() {
	DWORD length = 0;
	::GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
	if (length == 0) return {};
	std::vector<char> buffer(length);
	if (::GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) == FALSE) return {};
	std::vector<CpuTopology::WindowsCoreRecord>  cores;
	std::vector<CpuTopology::WindowsCacheRecord> caches;
	for (size_t offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
		if (info->Size == 0) break;
		if (info->Relationship == RelationProcessorCore) {
			CpuTopology::WindowsCoreRecord record;
			record.group            = info->Processor.GroupMask[0].Group;
			record.mask             = static_cast<uint64_t>(info->Processor.GroupMask[0].Mask);
			record.efficiency_class = info->Processor.EfficiencyClass;
			cores.push_back(record);
		} else if (info->Relationship == RelationCache) {
			CpuTopology::WindowsCacheRecord record;
			record.level       = info->Cache.Level;
			record.instruction = info->Cache.Type == CacheInstruction;
			record.group       = info->Cache.GroupMask.Group;
			record.mask        = static_cast<uint64_t>(info->Cache.GroupMask.Mask);
			caches.push_back(record);
		}
		offset += info->Size;
	}
	return CpuTopology::BuildFromWindows(cores, caches);
}

bool GetNice(uint32_t, int&) {
	return false;
}

bool SetNice(uint32_t, int) {
	return false;
}

uint64_t NiceLimit() {
	return 0;
}

void NameCurrentThread(const char*) {}

#else

constexpr bool kSupported = false;

uint32_t              ThreadId() { return 0; }
std::vector<uint32_t> ListThreads() { return {}; }
bool                  GetAffinity(uint32_t, Cpus&) { return false; }
bool                  SetAffinity(uint32_t, const Cpus&) { return false; }
Cpus                  ProcessAffinity() { return {}; }
CpuTopology::Topology ReadTopology() { return {}; }
bool                  GetNice(uint32_t, int&) { return false; }
bool                  SetNice(uint32_t, int) { return false; }
uint64_t              NiceLimit() { return 0; }
void                  NameCurrentThread(const char*) {}

#endif

// ---- portable part ---------------------------------------------------------------------------

constexpr auto kPollPeriod = std::chrono::milliseconds(250);
// kyGpu's nice value when the user's limits allow it (RLIMIT_NICE; usually they do not).
constexpr int kGpuNice = -5;

struct State {
	std::mutex              mutex;
	std::condition_variable cv;
	std::thread             thread;
	bool                    kick    = false;
	bool                    stop    = false;
	bool                    running = false; // the background thread exists

	// Everything below is guarded by `mutex`.
	Cpus                                    base; // the affinity at start-up: what "not placed" means
	std::unordered_map<uint32_t, Role>      roles;
	std::unordered_map<uint32_t, Cpus>      original;     // threads we moved: the mask they had
	std::unordered_map<uint32_t, int>       original_nice; // threads whose nice value we lowered
	std::unordered_set<uint32_t>            nice_checked;
	std::unordered_set<uint32_t>            foreign;       // threads with somebody else's mask: left alone
	bool                                    topology_read = false;
	CpuTopology::Topology                   topology;
	CpuTopology::Plan                       plan;
	Cpus                                    gpu_mask, sequencer_mask, rest_mask;
	bool                                    active  = false; // the plan is applied
	bool                                    refused = false; // the plan was refused in this "on" period (logged)
	uint64_t                                passes = 0, moved = 0, restored = 0, failed = 0;
};

State& St() {
	static State* state = new State(); // never destroyed: a thread may still call SetCurrentThreadRole at exit
	return *state;
}

void Say(const std::string& text) {
	Log::WriteToConsoleAndLog("[placement] " + text + "\n");
}

Cpus ToBits(const std::vector<uint32_t>& cpus) {
	Cpus bits;
	for (uint32_t c: cpus) {
		if (c < CpuTopology::kMaxCpus) bits.set(c);
	}
	return bits;
}

std::vector<uint32_t> ToList(const Cpus& bits) {
	std::vector<uint32_t> list;
	for (uint32_t c = 0; c < CpuTopology::kMaxCpus; c++) {
		if (bits.test(c)) list.push_back(c);
	}
	return list;
}

const Cpus& MaskFor(const State& s, Role role) {
	switch (role) {
		case Role::Gpu: return s.gpu_mask;
		case Role::Sequencer: return s.sequencer_mask.any() ? s.sequencer_mask : s.rest_mask;
		case Role::Other: break;
	}
	return s.rest_mask;
}

bool IsPlanMask(const State& s, const Cpus& cpus) {
	return cpus == s.gpu_mask || cpus == s.sequencer_mask || cpus == s.rest_mask;
}

void RaiseGpuPriority(State& s, uint32_t tid) {
	if (!s.nice_checked.insert(tid).second) return;
	int current = 0;
	if (!GetNice(tid, current) || current <= kGpuNice) return;
	const uint64_t limit = NiceLimit();
	if (!CpuTopology::NiceAllowed(current, kGpuNice, limit)) {
		Say("kyGpu priority not raised: RLIMIT_NICE is " + std::to_string(limit) + " (a nice value of " + std::to_string(kGpuNice) + " needs " +
		    std::to_string(20 - kGpuNice) + ")");
		return;
	}
	if (SetNice(tid, kGpuNice)) {
		s.original_nice[tid] = current;
		Say("kyGpu nice " + std::to_string(current) + " -> " + std::to_string(kGpuNice));
	} else {
		Say("kyGpu priority not raised: setpriority refused");
	}
}

// One thread, plan active. The thread's role decides the mask; a mask somebody else chose is left alone.
void ApplyOne(State& s, uint32_t tid) {
	Role role = Role::Other;
	if (auto it = s.roles.find(tid); it != s.roles.end()) role = it->second;
	const Cpus& desired = MaskFor(s, role);
	Cpus        current;
	if (!GetAffinity(tid, current)) return; // the thread is gone
	if (current != desired) {
		const bool plan_mask = IsPlanMask(s, current);
		auto       original  = s.original.find(tid);
		const bool ours      = current == s.base || plan_mask || (original != s.original.end() && current == original->second);
		if (!ours) {
			s.foreign.insert(tid);
			return;
		}
		if (original == s.original.end()) s.original[tid] = plan_mask ? s.base : current;
		if (SetAffinity(tid, desired)) {
			s.moved++;
			if (role != Role::Other) {
				Say(std::string("thread ") + std::to_string(tid) + (role == Role::Gpu ? " (kyGpu)" : " (look-ahead sequencer)") + " -> cpu " +
				    CpuTopology::CpuListString(ToList(desired)));
			}
		} else {
			s.failed++;
		}
	}
	if (role == Role::Gpu) RaiseGpuPriority(s, tid);
}

void RestoreOne(State& s, uint32_t tid) {
	Cpus current;
	if (GetAffinity(tid, current) && IsPlanMask(s, current)) {
		auto original = s.original.find(tid);
		if (SetAffinity(tid, original != s.original.end() ? original->second : s.base)) {
			s.restored++;
		} else {
			s.failed++;
		}
	}
	if (auto n = s.original_nice.find(tid); n != s.original_nice.end()) SetNice(tid, n->second);
}

bool Activate(State& s) {
	if (!kSupported) {
		Say("not applied: thread placement is not implemented for this host");
		return false;
	}
	if (s.base.none()) {
		Say("not applied: the process affinity could not be read");
		return false;
	}
	if (!s.topology_read) {
		s.topology      = ReadTopology();
		s.topology_read = true;
	}
	if (s.topology.Empty()) {
		Say("not applied: the CPU topology could not be read");
		return false;
	}
	CpuTopology::Options options;
	options.dedicated_cores = Config::ThreadPlacementCores();
	s.plan                  = CpuTopology::MakePlan(s.topology, ToList(s.base), options);
	if (!s.plan.ok) {
		Say("not applied: " + s.plan.note);
		return false;
	}
	s.gpu_mask       = ToBits(s.plan.gpu);
	s.sequencer_mask = ToBits(s.plan.sequencer);
	s.rest_mask      = ToBits(s.plan.rest);
	s.passes = s.moved = s.restored = s.failed = 0;
	s.foreign.clear();
	Say("on: " + s.plan.note);
	return true;
}

void Deactivate(State& s) {
	Say("off: restored " + std::to_string(s.restored) + " threads (moved " + std::to_string(s.moved) + " in " + std::to_string(s.passes) + " scans, " +
	    std::to_string(s.foreign.size()) + " left alone, " + std::to_string(s.failed) + " failed)");
	s.original.clear();
	s.original_nice.clear();
	s.nice_checked.clear();
	s.foreign.clear();
	s.active = false;
}

// One scan; the mutex is held.
void Pass(State& s) {
	const bool want = Config::ThreadPlacementEnabled();
	if (!want) s.refused = false;
	if (!want && !s.active) return;
	if (want && !s.active) {
		if (s.refused) return;
		if (!Activate(s)) {
			s.refused = true;
			return;
		}
		s.active = true;
	}
	const auto tids = ListThreads();
	{
		// Forget threads that ended (their ids may be reused).
		const std::unordered_set<uint32_t> live(tids.begin(), tids.end());
		auto                               prune = [&](auto& map) {
            for (auto it = map.begin(); it != map.end();) it = live.count(it->first) != 0 ? std::next(it) : map.erase(it);
		};
		prune(s.roles);
		prune(s.original);
		prune(s.original_nice);
		for (auto it = s.nice_checked.begin(); it != s.nice_checked.end();) it = live.count(*it) != 0 ? std::next(it) : s.nice_checked.erase(it);
		for (auto it = s.foreign.begin(); it != s.foreign.end();) it = live.count(*it) != 0 ? std::next(it) : s.foreign.erase(it);
	}
	s.passes++;
	if (want) {
		for (uint32_t tid: tids) ApplyOne(s, tid);
	} else {
		for (uint32_t tid: tids) RestoreOne(s, tid);
		Deactivate(s);
	}
}

void Loop(State* s) {
	NameCurrentThread("kyPlacement");
	std::unique_lock lock(s->mutex);
	while (!s->stop) {
		Pass(*s);
		s->cv.wait_for(lock, kPollPeriod, [s] { return s->stop || s->kick; });
		s->kick = false;
	}
	// Leave the process the way we found it.
	if (s->active) {
		for (uint32_t tid: ListThreads()) RestoreOne(*s, tid);
		Deactivate(*s);
	}
}

} // namespace

void Start(bool toggleable) {
	Stop();
	State& s = St();
	std::lock_guard lock(s.mutex);
	s.base  = ProcessAffinity();
	s.stop  = false;
	s.kick  = false;
	s.active  = false;
	s.refused = false;
	if (!Config::ThreadPlacementEnabled() && !toggleable) return;
	Pass(s); // placed before the first worker threads exist
	s.running = true;
	s.thread  = std::thread(Loop, &s);
}

void Stop() {
	State& s = St();
	{
		std::lock_guard lock(s.mutex);
		if (!s.running) return;
		s.stop = true;
	}
	s.cv.notify_all();
	if (s.thread.joinable()) s.thread.join();
	std::lock_guard lock(s.mutex);
	s.running = false;
}

void SetCurrentThreadRole(Role role) {
	State&          s   = St();
	const uint32_t  tid = ThreadId();
	std::lock_guard lock(s.mutex);
	s.roles[tid] = role;
	if (s.active && Config::ThreadPlacementEnabled()) ApplyOne(s, tid);
}

void Refresh() {
	State& s = St();
	{
		std::lock_guard lock(s.mutex);
		if (!s.running) return;
		s.kick = true;
	}
	s.cv.notify_all();
}

} // namespace Common::ThreadPlacement
