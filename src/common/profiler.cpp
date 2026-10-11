#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <tracy/Tracy.hpp>

namespace Profiler {

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
	if (tracy::ProfilerAvailable()) {
		tracy::SetThreadName(name);
	}
#if defined(__linux__)
	// Make the thread visible by name to top/htop/perf and /proc/<pid>/task/<tid>/comm. The
	// kernel limit is 15 characters. The main thread is left alone: renaming it would change the
	// process name (/proc/<pid>/comm).
	if (::syscall(SYS_gettid) != ::getpid()) {
		char short_name[16] {};
		std::strncpy(short_name, name, sizeof(short_name) - 1);
		::pthread_setname_np(::pthread_self(), short_name);
	}
#endif
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
