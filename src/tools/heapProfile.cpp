// Sampling heap profiler, linked into kyty_emulator only with -DKYTY_HEAP_PROFILE=ON (a diagnostic
// build, never a release one). It replaces the C allocator entry points of the executable, so it
// sees every malloc of the process, including C++ operator new and the GPU driver's host
// allocations, and forwards to the glibc allocator (__libc_malloc and friends). No LD_PRELOAD.
//
// Sampling follows tcmalloc: a per-thread byte countdown, drawn from an exponential distribution
// with mean KYTY_HEAP_PROFILE_BYTES (default 128 KiB). An allocation of s bytes is sampled with
// probability 1 - exp(-s / mean); a live sample stands for s / that probability bytes, so the sums
// estimate the live heap per allocation stack. A sampled allocation records its stack
// (backtrace()) and is found again on free through a counting filter plus an exact table.
//
// Output: heap_profile_<seconds>.txt in the working directory every KYTY_HEAP_PROFILE_INTERVAL
// seconds (default 30) and when SIGUSR2 arrives: malloc_info() (per-arena sizes), mallinfo2() and
// the top stacks by estimated live bytes as module+offset (symbolize with llvm-symbolizer or
// addr2line on an unstripped build).
#include <dlfcn.h>
#include <execinfo.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
void* __libc_malloc(size_t);
void  __libc_free(void*);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
}

namespace {

constexpr int      MAX_FRAMES  = 24;
constexpr int      SKIP_FRAMES = 2; // Record and the allocator wrapper
constexpr size_t   FILTER_SIZE = size_t {1} << 20;

thread_local bool     t_in_hook   = false;
thread_local int64_t  t_countdown = -1;
thread_local uint64_t t_rng       = 0;

std::atomic<uint8_t>  g_filter[FILTER_SIZE];
std::atomic<bool>     g_ready {false};
std::atomic<bool>     g_dump_requested {false};
double                g_mean_bytes = 128.0 * 1024.0;

struct Stack {
	void*    frames[MAX_FRAMES];
	int      depth       = 0;
	double   est_bytes   = 0; // estimated live bytes
	uint64_t live_samples = 0;
	uint64_t total_samples = 0;
};
struct Sample {
	Stack* stack  = nullptr;
	size_t size   = 0;
	double weight = 0;
};

std::mutex                                   g_mutex;
std::unordered_map<uint64_t, Stack*>*        g_stacks  = nullptr;
std::unordered_map<void*, Sample>*           g_samples = nullptr;

struct HookScope {
	bool was;
	HookScope(): was(t_in_hook) { t_in_hook = true; }
	~HookScope() { t_in_hook = was; }
};

uint64_t NextRandom() {
	if (t_rng == 0) {
		t_rng = (reinterpret_cast<uintptr_t>(&t_rng) * 0x9E3779B97F4A7C15ull) | 1u;
	}
	t_rng ^= t_rng << 13;
	t_rng ^= t_rng >> 7;
	t_rng ^= t_rng << 17;
	return t_rng;
}

int64_t DrawCountdown() {
	const double u = (static_cast<double>(NextRandom() >> 11) + 1.0) / 9007199254740993.0;
	return static_cast<int64_t>(-std::log(u) * g_mean_bytes) + 1;
}

size_t FilterIndex(const void* p) {
	return (reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 44 & (FILTER_SIZE - 1);
}

bool ShouldSample(size_t size) {
	if (!g_ready.load(std::memory_order_relaxed) || t_in_hook) {
		return false;
	}
	if (t_countdown < 0) {
		t_countdown = DrawCountdown();
	}
	if (static_cast<int64_t>(size) < t_countdown) {
		t_countdown -= static_cast<int64_t>(size);
		return false;
	}
	t_countdown = DrawCountdown();
	return true;
}

void Record(void* p, size_t size) {
	HookScope scope;
	void*     frames[MAX_FRAMES + SKIP_FRAMES];
	const int n = backtrace(frames, MAX_FRAMES + SKIP_FRAMES);
	const int depth = std::max(0, n - SKIP_FRAMES);
	uint64_t  h = 1469598103934665603ull;
	for (int i = 0; i < depth; i++) {
		h = (h ^ reinterpret_cast<uintptr_t>(frames[SKIP_FRAMES + i])) * 1099511628211ull;
	}
	const double prob   = 1.0 - std::exp(-static_cast<double>(size) / g_mean_bytes);
	const double weight = static_cast<double>(size) / (prob > 1e-12 ? prob : 1e-12);
	std::lock_guard<std::mutex> lock(g_mutex);
	auto& slot = (*g_stacks)[h];
	if (slot == nullptr) {
		slot = new Stack();
		slot->depth = depth;
		std::memcpy(slot->frames, frames + SKIP_FRAMES, sizeof(void*) * static_cast<size_t>(depth));
	}
	slot->est_bytes += weight;
	slot->live_samples++;
	slot->total_samples++;
	(*g_samples)[p] = Sample {slot, size, weight};
	g_filter[FilterIndex(p)].fetch_add(1, std::memory_order_relaxed);
}

void Forget(void* p) {
	if (p == nullptr || t_in_hook || !g_ready.load(std::memory_order_relaxed)) {
		return;
	}
	if (g_filter[FilterIndex(p)].load(std::memory_order_relaxed) == 0) {
		return;
	}
	HookScope scope;
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = g_samples->find(p);
	if (it == g_samples->end()) {
		return;
	}
	it->second.stack->est_bytes -= it->second.weight;
	it->second.stack->live_samples--;
	g_samples->erase(it);
	g_filter[FilterIndex(p)].fetch_sub(1, std::memory_order_relaxed);
}

void Dump(const char* reason) {
	HookScope scope;
	static const auto start = std::chrono::steady_clock::now();
	const long seconds = std::chrono::duration_cast<std::chrono::seconds>(
	                         std::chrono::steady_clock::now() - start).count();
	char name[96];
	std::snprintf(name, sizeof(name), "heap_profile_%05ld.txt", seconds);
	FILE* f = std::fopen(name, "w");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "# heap profile (%s), t=%lds since the first dump call, mean sample distance %.0f bytes\n", reason,
	             seconds, g_mean_bytes);
	const auto mi = mallinfo2();
	std::fprintf(f, "# mallinfo2: arena=%zu (non-mmapped heap) hblkhd=%zu (mmapped chunks) uordblks=%zu (in use) fordblks=%zu (free) keepcost=%zu\n",
	             mi.arena, mi.hblkhd, mi.uordblks, mi.fordblks, mi.keepcost);
	std::fprintf(f, "# malloc_info:\n");
	malloc_info(0, f);
	std::vector<Stack*> stacks;
	double total = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (auto& [h, s]: *g_stacks) {
			if (s->live_samples != 0 && s->est_bytes > 0) {
				stacks.push_back(s);
				total += s->est_bytes;
			}
		}
		std::sort(stacks.begin(), stacks.end(), [](const Stack* a, const Stack* b) { return a->est_bytes > b->est_bytes; });
		std::fprintf(f, "# estimated live bytes over all sampled stacks: %.1f MiB in %zu stacks\n", total / 1048576.0, stacks.size());
		size_t shown = 0;
		for (const auto* s: stacks) {
			if (shown++ == 60) {
				break;
			}
			std::fprintf(f, "STACK est_mib=%.2f samples=%llu\n", s->est_bytes / 1048576.0,
			             static_cast<unsigned long long>(s->live_samples));
			for (int i = 0; i < s->depth; i++) {
				Dl_info info {};
				if (dladdr(s->frames[i], &info) != 0 && info.dli_fname != nullptr) {
					std::fprintf(f, "  %s+0x%lx\n", info.dli_fname,
					             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(s->frames[i]) -
					                                        reinterpret_cast<uintptr_t>(info.dli_fbase)));
				} else {
					std::fprintf(f, "  ?+%p\n", s->frames[i]);
				}
			}
		}
	}
	std::fclose(f);
}

void OnSignal(int) {
	g_dump_requested.store(true);
}

void DumpThread() {
	HookScope scope;
	double interval = 30.0;
	if (const char* env = std::getenv("KYTY_HEAP_PROFILE_INTERVAL")) {
		interval = std::max(1.0, std::atof(env));
	}
	auto next = std::chrono::steady_clock::now() + std::chrono::duration<double>(interval);
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool requested = g_dump_requested.exchange(false);
		if (requested || std::chrono::steady_clock::now() >= next) {
			Dump(requested ? "SIGUSR2" : "periodic");
			next = std::chrono::steady_clock::now() + std::chrono::duration<double>(interval);
		}
	}
}

__attribute__((constructor(101))) void Init() {
	HookScope scope;
	if (const char* env = std::getenv("KYTY_HEAP_PROFILE_BYTES")) {
		g_mean_bytes = std::max(4096.0, std::atof(env));
	}
	g_stacks  = new std::unordered_map<uint64_t, Stack*>();
	g_samples = new std::unordered_map<void*, Sample>();
	g_stacks->reserve(4096);
	g_samples->reserve(65536);
	void* warm[4];
	(void)backtrace(warm, 4); // loads the unwinder now, not inside an allocation
	::signal(SIGUSR2, OnSignal);
	std::thread(DumpThread).detach();
	g_ready.store(true);
}

} // namespace

extern "C" {

void* malloc(size_t size) {
	void* p = __libc_malloc(size);
	if (p != nullptr && ShouldSample(size)) {
		Record(p, size);
	}
	return p;
}

void free(void* p) {
	Forget(p);
	__libc_free(p);
}

void* calloc(size_t n, size_t size) {
	void* p = __libc_calloc(n, size);
	if (p != nullptr && n != 0 && size <= SIZE_MAX / n && ShouldSample(n * size)) {
		Record(p, n * size);
	}
	return p;
}

void* realloc(void* old, size_t size) {
	Forget(old);
	void* p = __libc_realloc(old, size);
	if (p != nullptr && size != 0 && ShouldSample(size)) {
		Record(p, size);
	}
	return p;
}

void* memalign(size_t alignment, size_t size) {
	void* p = __libc_memalign(alignment, size);
	if (p != nullptr && ShouldSample(size)) {
		Record(p, size);
	}
	return p;
}

void* aligned_alloc(size_t alignment, size_t size) {
	return memalign(alignment, size);
}

int posix_memalign(void** out, size_t alignment, size_t size) {
	if (alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0) {
		return 22; // EINVAL
	}
	void* p = memalign(alignment, size);
	if (p == nullptr) {
		return 12; // ENOMEM
	}
	*out = p;
	return 0;
}

void* valloc(size_t size) {
	return memalign(static_cast<size_t>(sysconf(_SC_PAGESIZE)), size);
}

void* pvalloc(size_t size) {
	const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
	return memalign(page, (size + page - 1) / page * page);
}

} // extern "C"
