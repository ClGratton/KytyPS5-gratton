#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_PROBE_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_PROBE_H_

// Reads the host's memory setup (Linux): /proc/meminfo, /proc/swaps, the zswap parameters in /sys,
// and the files the launcher's own tuning installs. Plain C++ with no Qt; every path is relative to
// a root so the tests can run it on a made-up tree.

#include "hostMemoryPolicy.h"

#include <cstdint>
#include <functional>
#include <string>

namespace HostMemory {

// Where the launcher's tuning puts things (the helper script uses the same paths).
constexpr char kUnitPath[]       = "/etc/systemd/system/kyty-host-memory.service";
constexpr char kZramDropinPath[] = "/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf";
constexpr char kStateDir[]       = "/var/lib/kyty-host-memory";
// A new swapfile goes here (a nested btrfs subvolume on btrfs, a plain directory elsewhere).
constexpr char kSwapDir[] = "/var/lib/kyty-host-memory/swap";

struct ProbeEnv {
	std::string root;  // prefix of /proc, /sys, /etc, /run and /usr ("" = the running system)
	// Free space in MiB of the filesystem holding the real path (default: statvfs).
	std::function<uint64_t(const std::string& real_path)> free_mib;
};

LinuxFacts ProbeLinux(const ProbeEnv& env);

} // namespace HostMemory

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_PROBE_H_ */
