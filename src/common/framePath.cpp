#include "common/framePath.h"

#include "common/hleTrace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::FramePath {

std::atomic<bool> detail::g_on {false};

#if defined(__linux__)

namespace {

struct Event {
	uint64_t t;
	uint32_t dur;
	uint16_t kind;
	uint16_t thread;
	uint64_t a;
	uint64_t b;
};
static_assert(sizeof(Event) == 32);

constexpr uint32_t RING_EVENTS = 1u << 19; // 16 MiB of address space per thread, touched lazily
constexpr uint32_t MAX_RINGS   = 1024;

struct Ring {
	std::atomic<uint64_t> head {0};
	uint32_t              index = 0;
	int                   tid   = 0;
	char                  name[32] {};
	Event                 events[RING_EVENTS];
};

std::atomic<Ring*>    g_rings[MAX_RINGS];
std::atomic<uint32_t> g_ring_count {0};
thread_local Ring*    t_ring        = nullptr;
thread_local bool     t_ring_failed = false;

std::string                                     g_path;
std::atomic<bool>                               g_requested {false};
std::once_flag                                  g_started;
double                                          g_window_s = 8.0;
double                                          g_ns_per_tsc = 0.0;

Ring* GetRing() noexcept {
	if (t_ring != nullptr) {
		return t_ring;
	}
	if (t_ring_failed) {
		return nullptr;
	}
	const auto slot = g_ring_count.fetch_add(1, std::memory_order_relaxed);
	if (slot >= MAX_RINGS) {
		t_ring_failed = true;
		return nullptr;
	}
	void* p = mmap(nullptr, sizeof(Ring), PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) {
		t_ring_failed = true;
		return nullptr;
	}
	auto* ring  = new (p) Ring;
	ring->index = slot;
	ring->tid   = static_cast<int>(syscall(SYS_gettid));
	pthread_getname_np(pthread_self(), ring->name, sizeof(ring->name));
	t_ring = ring;
	g_rings[slot].store(ring, std::memory_order_release);
	return ring;
}

std::string ReadComm(int tid, const char* fallback) {
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
	if (FILE* f = std::fopen(path, "r")) {
		char buf[64] = {};
		if (std::fgets(buf, sizeof(buf), f) != nullptr) {
			std::fclose(f);
			std::string s(buf);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
				s.pop_back();
			}
			return s;
		}
		std::fclose(f);
	}
	return fallback;
}

uint64_t MainBase() {
	uint64_t base = 0;
	dl_iterate_phdr(
	    [](dl_phdr_info* info, size_t, void* data) -> int {
		    *static_cast<uint64_t*>(data) = info->dlpi_addr;
		    return 1; // the first object is the main executable
	    },
	    &base);
	return base;
}

void JsonEscape(std::string& out, const std::string& s) {
	for (const char c: s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			out += '?';
		} else {
			out += c;
		}
	}
}

void Dump(unsigned seq) {
	detail::g_on.store(false, std::memory_order_relaxed);
	std::this_thread::sleep_for(std::chrono::milliseconds(60)); // writers in flight finish
	const uint64_t now    = NowNs();
	const uint64_t cutoff = now - static_cast<uint64_t>(g_window_s * 1e9);
	const auto     rings  = std::min<uint32_t>(g_ring_count.load(), MAX_RINGS);

	std::vector<Event> out;
	std::string        threads;
	for (uint32_t i = 0; i < rings; i++) {
		Ring* ring = g_rings[i].load(std::memory_order_acquire);
		if (ring == nullptr) {
			continue;
		}
		const uint64_t head  = ring->head.load(std::memory_order_acquire);
		const uint64_t count = std::min<uint64_t>(head, RING_EVENTS);
		size_t         kept  = 0;
		for (uint64_t k = head - count; k < head; k++) {
			const Event& e = ring->events[k % RING_EVENTS];
			if (e.t + e.dur < cutoff || e.t > now) {
				continue;
			}
			out.push_back(e);
			kept++;
		}
		if (!threads.empty()) {
			threads += ',';
		}
		threads += "[" + std::to_string(i) + "," + std::to_string(ring->tid) + ",\"";
		JsonEscape(threads, ReadComm(ring->tid, ring->name));
		threads += "\"," + std::to_string(kept) + "," + std::to_string(head > RING_EVENTS ? 1 : 0) + "]";
	}
	std::string names;
	{
		// `a` of these events points at a name that lives for the whole run (HleTrace record).
		std::unordered_map<uint64_t, bool> mark;
		for (const auto& e: out) {
			if ((e.kind == MutexWait || e.kind == CondWait || e.kind == Sleep || e.kind == Submit ||
			     e.kind == SuspendWait || e.kind == CmdSyncWait || e.kind == WaitIdle) &&
			    e.a != 0) {
				mark[e.a] = true;
			}
		}
		for (const auto& [key, flag]: mark) {
			if (!names.empty()) {
				names += ',';
			}
			names += "\"" + std::to_string(key) + "\":\"";
			JsonEscape(names, reinterpret_cast<const char*>(key));
			names += '"';
		}
	}
	char file[1024];
	std::snprintf(file, sizeof(file), "%s.%u.kfp", g_path.c_str(), seq);
	if (FILE* f = std::fopen(file, "wb")) {
		std::fprintf(f, "{\"magic\":\"KFP1\",\"base\":%llu,\"t_dump\":%llu,\"window_s\":%.3f,\"events\":%zu,"
		                "\"threads\":[%s],\"names\":{%s}}\n",
		             static_cast<unsigned long long>(MainBase()), static_cast<unsigned long long>(now),
		             g_window_s, out.size(), threads.c_str(), names.c_str());
		std::fwrite(out.data(), sizeof(Event), out.size(), f);
		std::fclose(f);
		std::fprintf(stderr, "FramePath: wrote %s (%zu events, %.1f s window)\n", file, out.size(),
		             g_window_s);
	} else {
		std::fprintf(stderr, "FramePath: cannot write %s\n", file);
	}
	detail::g_on.store(true, std::memory_order_relaxed);
}

void DumpThread() {
	pthread_setname_np(pthread_self(), "kyFramePath");
	const std::string go = g_path + ".go";
	unsigned          seq = 0;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (access(go.c_str(), F_OK) == 0) {
			unlink(go.c_str());
			Dump(seq++);
		}
	}
}

} // namespace

uint64_t NowNs() noexcept {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

bool Requested() noexcept {
	static const bool requested = std::getenv("KYTY_FRAME_PATH") != nullptr &&
	                              std::getenv("KYTY_FRAME_PATH")[0] != '\0';
	return requested;
}

void Start() {
	std::call_once(g_started, [] {
		if (!Requested()) {
			return;
		}
		g_path = std::getenv("KYTY_FRAME_PATH");
		if (const char* w = std::getenv("KYTY_FRAME_PATH_SECONDS")) {
			g_window_s = std::max(0.5, std::atof(w));
		}
		g_requested.store(true);
#if defined(__x86_64__)
		{
			const uint64_t t0 = NowNs();
			const uint64_t c0 = __builtin_ia32_rdtsc();
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
			const uint64_t t1 = NowNs();
			const uint64_t c1 = __builtin_ia32_rdtsc();
			g_ns_per_tsc      = c1 > c0 ? static_cast<double>(t1 - t0) / static_cast<double>(c1 - c0) : 0.0;
		}
#endif
		detail::g_on.store(true, std::memory_order_relaxed);
		std::thread(DumpThread).detach();
		std::fprintf(stderr, "FramePath: logging to %s.<n>.kfp on %s.go, window %.1f s\n",
		             g_path.c_str(), g_path.c_str(), g_window_s);
	});
}

void Log(Kind kind, uint64_t t_ns, uint64_t dur_ns, uint64_t a, uint64_t b) noexcept {
	if (!On()) {
		return;
	}
	Ring* ring = GetRing();
	if (ring == nullptr) {
		return;
	}
	const auto head = ring->head.load(std::memory_order_relaxed);
	if ((head & 1023u) == 0) { // the guest names its threads after they start
		pthread_getname_np(pthread_self(), ring->name, sizeof(ring->name));
	}
	Event& e = ring->events[head % RING_EVENTS];
	e.t      = t_ns;
	e.dur    = static_cast<uint32_t>(std::min<uint64_t>(dur_ns, 0xffffffffull));
	e.kind   = kind;
	e.thread = static_cast<uint16_t>(ring->index);
	e.a      = a;
	e.b      = b;
	ring->head.store(head + 1, std::memory_order_release);
}

double NsPerTsc() noexcept {
	return g_ns_per_tsc;
}

Blame CurrentBlame(uint64_t now_ns) noexcept {
	const auto call = HleTrace::CurrentCall();
	if (call.name == nullptr) {
		return {0, 0};
	}
	const uint64_t age_us = call.ns <= now_ns ? (now_ns - call.ns) / 1000 : 0;
	return {reinterpret_cast<uint64_t>(call.name), std::min<uint64_t>(age_us, 0xffffu) << 48};
}

#else // !__linux__

uint64_t NowNs() noexcept {
	return 0;
}
bool Requested() noexcept {
	return false;
}
void Start() {}
void Log(Kind, uint64_t, uint64_t, uint64_t, uint64_t) noexcept {}
double NsPerTsc() noexcept {
	return 0.0;
}
Blame CurrentBlame(uint64_t) noexcept {
	return {0, 0};
}

#endif

} // namespace Common::FramePath
