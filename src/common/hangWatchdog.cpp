#include "common/hangWatchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace Common::Hang {

std::atomic<bool> detail::g_enabled {false};

namespace {

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

// The phase tags stamp their entry time on every scope (tens of thousands per frame on the GPU thread), and a vDSO clock_gettime
// there cost 0.6 ms of a 40 ms kyGpu frame. The stamp is only read by the stall report, so it is a raw cycle counter where there is
// one (invariant TSC on x86-64) and the report scales it with the ratio measured since program start.
inline int64_t Ticks() {
#if defined(__x86_64__) || defined(_M_X64)
	return static_cast<int64_t>(__rdtsc());
#else
	return NowNs();
#endif
}

const int64_t g_start_ns    = NowNs();
const int64_t g_start_ticks = Ticks();

// Nanoseconds per tick since program start (1 where the tick is a nanosecond).
double NsPerTick() {
	const auto ticks = Ticks() - g_start_ticks;
	const auto ns    = NowNs() - g_start_ns;
	return ticks > 0 && ns > 0 ? static_cast<double>(ns) / static_cast<double>(ticks) : 1.0;
}

// One per thread that ever published a phase. Leaked on purpose: the watchdog may read it after
// the thread is gone.
struct ThreadSlot {
	std::atomic<uint64_t>    tid {0};
	std::atomic<const char*> what {nullptr};
	std::atomic<uint64_t>    a {0};
	std::atomic<uint64_t>    b {0};
	std::atomic<int64_t>     since {0};
};

std::mutex               g_slots_mutex;
std::vector<ThreadSlot*> g_slots;

ThreadSlot& MySlot() {
	thread_local ThreadSlot* slot = [] {
		auto* created = new ThreadSlot;
#if defined(__linux__)
		created->tid.store(static_cast<uint64_t>(syscall(SYS_gettid)));
#endif
		std::lock_guard lock(g_slots_mutex);
		g_slots.push_back(created);
		return created;
	}();
	return *slot;
}

std::mutex                                      g_providers_mutex;
std::vector<std::pair<std::string, std::function<std::string()>>> g_providers;

std::atomic<uint64_t> g_progress {0};
std::atomic<int64_t>  g_last_progress_ns {0};
std::atomic<bool>     g_armed {false};
uint32_t              g_seconds = 0;

#if defined(__linux__)
const int kBacktraceSignal = SIGRTMIN + 4;
int           g_report_fd      = -1;

void BacktraceHandler(int) {
	void* frames[40];
	const int n = backtrace(frames, 40);
	if (g_report_fd >= 0) {
		backtrace_symbols_fd(frames, n, g_report_fd);
	}
}

struct CpuSample {
	std::string name;
	char        state = '?';
	uint64_t    ticks = 0;
	std::string wchan;
};

std::map<uint64_t, CpuSample> SampleThreads() {
	std::map<uint64_t, CpuSample> result;
	DIR* dir = opendir("/proc/self/task");
	if (dir == nullptr) {
		return result;
	}
	while (auto* entry = readdir(dir)) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		const uint64_t tid = std::strtoull(entry->d_name, nullptr, 10);
		char           path[96];
		std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
		CpuSample sample;
		if (FILE* file = std::fopen(path, "r")) {
			char line[1024] = {};
			if (std::fgets(line, sizeof(line), file) != nullptr) {
				// pid (comm) state ... ; comm may contain spaces/parentheses: use the last ')'.
				const char* open  = std::strchr(line, '(');
				const char* close = std::strrchr(line, ')');
				if (open != nullptr && close != nullptr && close > open) {
					sample.name.assign(open + 1, close);
					char     state = '?';
					uint64_t utime = 0, stime = 0;
					unsigned long long dummy = 0;
					int      ppid = 0, pgrp = 0, session = 0, tty = 0, tpgid = 0;
					unsigned flags = 0;
					unsigned long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
					if (std::sscanf(close + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu",
					                &state, &ppid, &pgrp, &session, &tty, &tpgid, &flags, &minflt,
					                &cminflt, &majflt, &cmajflt, &utime, &stime) >= 13) {
						sample.state = state;
						sample.ticks = utime + stime;
					}
					(void)dummy;
				}
			}
			std::fclose(file);
		}
		std::snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", entry->d_name);
		if (FILE* file = std::fopen(path, "r")) {
			char buffer[128] = {};
			if (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
				sample.wchan = buffer;
			}
			std::fclose(file);
		}
		result[tid] = std::move(sample);
	}
	closedir(dir);
	return result;
}
#endif

void Emit(const std::string& text) {
	std::fwrite(text.data(), 1, text.size(), stderr);
	std::fflush(stderr);
#if defined(__linux__)
	if (g_report_fd >= 0) {
		(void)!write(g_report_fd, text.data(), text.size());
	}
#endif
}

void Report(unsigned index, double stalled_s) {
#if defined(__linux__)
	const auto hz = static_cast<double>(sysconf(_SC_CLK_TCK));
	std::string out = "=== hang watchdog report #" + std::to_string(index + 1) + ": no frame for " +
	                  std::to_string(stalled_s) + " s, frames so far " +
	                  std::to_string(g_progress.load()) + " ===\n";
	const auto first = SampleThreads();
	const auto t0    = NowNs();
	std::this_thread::sleep_for(std::chrono::seconds(1));
	const auto second = SampleThreads();
	const auto dt     = static_cast<double>(NowNs() - t0) / 1e9;

	std::map<uint64_t, std::vector<std::string>> phases;
	{
		std::lock_guard lock(g_slots_mutex);
		const auto      now_ticks = Ticks();
		const auto      ns_per_tick = NsPerTick();
		for (auto* slot: g_slots) {
			const char* what = slot->what.load();
			if (what == nullptr) {
				continue;
			}
			char text[200];
			std::snprintf(text, sizeof(text), "%s(%llu,%llu) for %.1f s", what,
			              static_cast<unsigned long long>(slot->a.load()),
			              static_cast<unsigned long long>(slot->b.load()),
			              static_cast<double>(now_ticks - slot->since.load()) * ns_per_tick / 1e9);
			phases[slot->tid.load()].push_back(text);
		}
	}
	std::vector<uint64_t> busy;
	out += "-- threads (tid name state cpu% wchan phase)\n";
	for (const auto& [tid, sample]: second) {
		double cpu = 0;
		if (const auto prev = first.find(tid); prev != first.end() && hz > 0 && dt > 0) {
			cpu = static_cast<double>(sample.ticks - prev->second.ticks) / hz / dt * 100.0;
		}
		// The threads that move work between the guests and the GPU are shown even when they sleep:
		// in a deadlock they are the ones blocked.
		if (cpu >= 30.0 || sample.name == "kyty-lookahead" || sample.name == "kyGpu" ||
		    sample.name == "kyPrioOps" || sample.name == "kyGuestMain") {
			busy.push_back(tid);
		}
		std::string wchan = sample.wchan;
		while (!wchan.empty() && (wchan.back() == '\n' || wchan.back() == '\0')) wchan.pop_back();
		char line[256];
		std::snprintf(line, sizeof(line), "  %llu %-16s %c %5.1f%% %s",
		              static_cast<unsigned long long>(tid), sample.name.c_str(), sample.state, cpu,
		              wchan.c_str());
		out += line;
		if (const auto found = phases.find(tid); found != phases.end()) {
			for (const auto& text: found->second) out += "  [" + text + "]";
		}
		out += "\n";
	}
	out += "-- providers\n";
	{
		std::lock_guard lock(g_providers_mutex);
		for (const auto& [name, provider]: g_providers) {
			out += "  [" + name + "] " + provider() + "\n";
		}
	}
	Emit(out);
	out.clear();
	// Native backtraces of the busy threads (and the lookahead thread). Raw frames: resolve with
	// the linker map file.
	for (const auto tid: busy) {
		char header[160];
		std::snprintf(header, sizeof(header), "-- backtrace tid %llu (%s)\n",
		              static_cast<unsigned long long>(tid), second.at(tid).name.c_str());
		Emit(header);
		syscall(SYS_tgkill, getpid(), static_cast<pid_t>(tid), kBacktraceSignal);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	}
	// Module base, to turn the raw addresses into file offsets.
	if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
		char line[512];
		std::string base_lines = "-- maps (executable segments of the main image)\n";
		int         count      = 0;
		while (std::fgets(line, sizeof(line), maps) != nullptr && count < 8) {
			if (std::strstr(line, "kyty_emulator") != nullptr) {
				base_lines += line;
				count++;
			}
		}
		std::fclose(maps);
		Emit(base_lines);
	}
	Emit("=== end of report ===\n");
#else
	(void)index;
	(void)stalled_s;
#endif
}

void WatchdogLoop() {
#if defined(__linux__)
	pthread_setname_np(pthread_self(), "kyty-watchdog");
#endif
	unsigned reports       = 0;
	int64_t  last_report   = 0;
	uint64_t seen_progress = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (!g_armed.load(std::memory_order_acquire)) {
			continue;
		}
		const auto now      = NowNs();
		const auto progress = g_progress.load(std::memory_order_relaxed);
		if (progress != seen_progress) {
			seen_progress = progress;
			reports       = 0;
			last_report   = 0;
			continue;
		}
		const auto stalled = static_cast<double>(now - g_last_progress_ns.load()) / 1e9;
		if (stalled >= g_seconds && reports < 5 &&
		    (last_report == 0 || static_cast<double>(now - last_report) / 1e9 >= g_seconds)) {
			Report(reports, stalled);
			reports++;
			last_report = NowNs();
		}
	}
}

} // namespace

void Start(uint32_t seconds) {
	static std::once_flag once;
	if (seconds == 0) {
		return;
	}
	std::call_once(once, [seconds] {
		g_seconds = seconds;
#if defined(__linux__)
		g_report_fd = ::open("_hang_watchdog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
		struct sigaction action {};
		action.sa_handler = BacktraceHandler;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESTART;
		sigaction(kBacktraceSignal, &action, nullptr);
		void* warm[2];
		(void)backtrace(warm, 2); // load libgcc now, not inside the signal handler
#endif
		g_last_progress_ns.store(NowNs());
		detail::g_enabled.store(true);
		std::thread(WatchdogLoop).detach();
		std::fprintf(stderr, "Hang watchdog: armed after the first frame, report after %u s without a frame\n",
		             seconds);
	});
}

void NoteProgress() noexcept {
	if (!Enabled()) {
		return;
	}
	g_progress.fetch_add(1, std::memory_order_relaxed);
	g_last_progress_ns.store(NowNs(), std::memory_order_relaxed);
	g_armed.store(true, std::memory_order_release);
}

void RegisterProvider(const char* name, std::function<std::string()> provider) {
	std::lock_guard lock(g_providers_mutex);
	for (auto& entry: g_providers) {
		if (entry.first == name) {
			entry.second = std::move(provider);
			return;
		}
	}
	g_providers.emplace_back(name, std::move(provider));
}

void UnregisterProvider(const char* name) {
	std::lock_guard lock(g_providers_mutex);
	std::erase_if(g_providers, [&](const auto& entry) { return entry.first == name; });
}

void detail::PhaseEnter(const char* what, uint64_t a, uint64_t b, const char*& prev_what,
                        uint64_t& prev_a, uint64_t& prev_b, int64_t& prev_since) {
	auto& slot = MySlot();
	prev_what  = slot.what.load(std::memory_order_relaxed);
	prev_a     = slot.a.load(std::memory_order_relaxed);
	prev_b     = slot.b.load(std::memory_order_relaxed);
	prev_since = slot.since.load(std::memory_order_relaxed);
	slot.a.store(a, std::memory_order_relaxed);
	slot.b.store(b, std::memory_order_relaxed);
	slot.since.store(Ticks(), std::memory_order_relaxed);
	slot.what.store(what, std::memory_order_release);
}

void detail::PhaseLeave(const char* prev_what, uint64_t prev_a, uint64_t prev_b,
                        int64_t prev_since) {
	auto& slot = MySlot();
	slot.a.store(prev_a, std::memory_order_relaxed);
	slot.b.store(prev_b, std::memory_order_relaxed);
	slot.since.store(prev_since, std::memory_order_relaxed);
	slot.what.store(prev_what, std::memory_order_release);
}

} // namespace Common::Hang
