#include "common/enomemTrace.h"

#include <atomic>
#include <cstdio>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Common {

static std::atomic<uint64_t> g_enomem_count {0};

int NoteEnomem(int error, const char* function, int line) noexcept {
	const auto n = g_enomem_count.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n <= 64) {
		char name[32] = {};
#if defined(__linux__)
		pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
		std::fprintf(stderr, "Kernel ENOMEM #%llu: %s:%d (thread %s)\n",
		             static_cast<unsigned long long>(n), function, line, name);
		std::fflush(stderr);
	}
	return error;
}

uint64_t EnomemCount() noexcept {
	return g_enomem_count.load(std::memory_order_relaxed);
}

} // namespace Common
