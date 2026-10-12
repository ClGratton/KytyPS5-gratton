#include "common/pcSampler.h"

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace Common::PcSampler {

#if defined(__linux__)

namespace {

constexpr size_t kMaxSamples = 1u << 20;

std::atomic<uint32_t> g_hz {0};
std::atomic<uint32_t> g_count {0};
uint64_t*             g_samples = nullptr; // kMaxSamples entries, allocated before the timer starts
timer_t               g_timer {};
bool                  g_started = false;

void OnSample(int, siginfo_t*, void* context) {
	const auto* uc = static_cast<const ucontext_t*>(context);
	const auto  i  = g_count.fetch_add(1, std::memory_order_relaxed);
	if (i < kMaxSamples) {
		g_samples[i] = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
	}
}

uint64_t ExecutableBase() {
	uint64_t base = 0;
	dl_iterate_phdr(
	    [](dl_phdr_info* info, size_t, void* out) {
		    *static_cast<uint64_t*>(out) = info->dlpi_addr; // the first object is the executable
		    return 1;
	    },
	    &base);
	return base;
}

} // namespace

void Configure(uint32_t hz) {
	g_hz.store(hz, std::memory_order_relaxed);
}

void StartOnThisThread() {
	const auto hz = g_hz.load(std::memory_order_relaxed);
	if (hz == 0 || g_started) {
		return;
	}
	g_started = true;
	g_samples = new uint64_t[kMaxSamples];
	struct sigaction action {};
	action.sa_sigaction = OnSample;
	action.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGPROF, &action, nullptr) != 0) {
		return;
	}
	sigevent event {};
	event.sigev_notify = SIGEV_THREAD_ID;
	event.sigev_signo  = SIGPROF;
	event._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &g_timer) != 0) {
		return;
	}
	const long        period_ns = 1000000000L / static_cast<long>(hz);
	itimerspec spec {};
	spec.it_interval.tv_nsec = period_ns % 1000000000L;
	spec.it_interval.tv_sec  = period_ns / 1000000000L;
	spec.it_value            = spec.it_interval;
	timer_settime(g_timer, 0, &spec, nullptr);
}

void WriteReport() noexcept {
	if (!g_started || g_samples == nullptr) {
		return;
	}
	const auto taken = std::min<uint32_t>(g_count.load(std::memory_order_relaxed), kMaxSamples);
	std::unordered_map<uint64_t, uint32_t> counts;
	for (uint32_t i = 0; i < taken; ++i) {
		++counts[g_samples[i]];
	}
	std::vector<std::pair<uint32_t, uint64_t>> order;
	order.reserve(counts.size());
	for (const auto& [pc, n]: counts) {
		order.emplace_back(n, pc);
	}
	std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a > b; });
	if (auto* file = std::fopen("_gpu_pc_samples.txt", "w")) {
		const auto base = ExecutableBase();
		std::fprintf(file, "# samples=%u hz=%u base=0x%llx\n", taken, g_hz.load(),
		             static_cast<unsigned long long>(base));
		for (const auto& [n, pc]: order) {
			std::fprintf(file, "0x%llx %u\n", static_cast<unsigned long long>(pc - base), n);
		}
		std::fclose(file);
	}
}

#else

void Configure(uint32_t) {}
void StartOnThisThread() {}
void WriteReport() noexcept {}

#endif

} // namespace Common::PcSampler
