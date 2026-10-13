#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_POLICY_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_POLICY_H_

// Host memory tuning: what the launcher detects, and what it proposes (docs/host-memory.md).
//
// Everything in this header is plain C++ with no Qt and no system calls: the facts about the host
// are collected elsewhere (hostMemoryProbe.h) and the decision functions below only turn facts into
// advice, so they are unit-tested on made-up inputs (tests/HostMemoryPolicyTests.cpp).
//
// The Windows half is written without a Windows machine at hand and has not been run on real
// Windows: the policy is covered by unit tests, the probe and the elevated helper are not.

#include <cstdint>
#include <string>
#include <vector>

namespace HostMemory {

// ---- What the emulator needs (named constants; the numbers are measured, see docs/host-memory.md) ----

// The guest ("direct memory") backing store. kernel/memory.cpp: PhysicalMemory::TotalSize().
// On Linux it is a memfd of this size, on Windows a pagefile-backed section of this size.
constexpr uint64_t kGuestBackingMiB = 13824;
// Share of that backing that is resident at the worst point of a level load. Measured 2026-10-13:
// 8.2 GiB of 13.5 GiB resident at peak (notes/mem-footprint.md). Other titles can use more; the
// slack below absorbs that.
constexpr uint32_t kGuestResidentPermille = 610;
// Everything else the emulator process holds at the same point: Vulkan driver, shader compilation,
// staging buffers, the emulator itself. Measured 9.6 GiB Pss in total minus the 8.2 GiB guest part.
constexpr uint64_t kHostOverheadMiB = 1400;
// What a desktop session (compositor, browser, chat) keeps resident next to a game.
constexpr uint64_t kDesktopReserveMiB = 4096;
// Extra room for the page cache of the game's files and for transient peaks, in percent of
// (emulator + desktop). A host whose RAM covers all of that needs no tuning.
constexpr uint32_t kFitSlackPercent = 25;

constexpr uint64_t ExpectedEmulatorPeakMiB() {
	return kGuestBackingMiB * kGuestResidentPermille / 1000 + kHostOverheadMiB;
}
// Total RAM above which the host is left alone (about 17 GiB, so 16 GB machines are tuned and
// 24 GB machines are not).
constexpr uint64_t RequiredTotalMiB() {
	return (ExpectedEmulatorPeakMiB() + kDesktopReserveMiB) * (100 + kFitSlackPercent) / 100;
}

// ---- Tuning parameters ----

// The compressed pool of zswap (zstd compresses about 3:1) should be able to take the pages the
// emulator pushes out during a level load: 3 GiB of pool holds about 9 GiB of pages, twice the
// 4.3 GiB measured. The percentage is derived from RAM, so bigger hosts get a smaller share.
constexpr uint64_t kZswapPoolTargetMiB = 3072;
constexpr uint32_t kZswapPoolMinPercent = 10;
constexpr uint32_t kZswapPoolMaxPercent = 25;
// A disk swap smaller than this cannot take the cold tail that zswap writes back (the measured
// level-load spill is 4.3 GiB before compression).
constexpr uint64_t kMinUsefulDiskSwapMiB = 4096;
// A swapfile created by the launcher: half of RAM, between these two sizes, and never so large that
// less than kDiskReserveMiB stays free on the disk.
constexpr uint64_t kSwapfileMinMiB = 8192;
constexpr uint64_t kSwapfileMaxMiB = 16384;
constexpr uint64_t kDiskReserveMiB = 16384;
// Free memory needed on top of the contents of the zram device to swap it off at run time.
constexpr uint64_t kSwapoffMarginMiB = 1024;

// Windows: the commit charge of the emulator is the whole backing section plus its own overhead;
// the system needs that much headroom between commit limit and current charge, plus a margin.
constexpr uint64_t kCommitMarginMiB = 1024;
constexpr uint64_t RequiredCommitHeadroomMiB() {
	return kGuestBackingMiB + kHostOverheadMiB + kCommitMarginMiB;
}

uint32_t ZswapPoolPercent(uint64_t ram_mib);
uint64_t SwapfileSizeMiB(uint64_t ram_mib, uint64_t free_disk_mib);  // 0: too little disk space

enum class Tristate { Unknown, No, Yes };

// ---- Verdicts ----

enum class Verdict {
	Ok,              // nothing to do
	Recommended,     // an action helps and has no real drawback on this host
	Optional,        // an action is possible but the host works without it
	Tuned,           // the host is already in the proposed state
	NotRecommended,  // the host needs help but the usual fix has a drawback here
	Unsupported,     // this host cannot be tuned by the launcher
};

const char* VerdictName(Verdict verdict);

enum class Reason {
	EnoughRam,
	NoZswap,
	NoSystemd,
	NoDiskSwap,
	SmallDiskSwap,
	AlreadyTuned,
	ZramInFront,
	ZswapOff,
	RotationalSwap,
	NoSwapfileSpace,
	NoSwapfileFs,
	NotPersistent,
	CommitLimit,
	CompressionOff,
	Unknown,
};

const char* ReasonName(Reason reason);

// ---- Linux ----

enum class SwapKind { Zram, Partition, File, Other };

struct SwapDevice {
	std::string name;
	SwapKind    kind       = SwapKind::Other;
	uint64_t    size_mib   = 0;
	uint64_t    used_mib   = 0;
	int         priority   = 0;
	bool        rotational = false;  // best effort; false when unknown
};

struct LinuxFacts {
	uint64_t ram_mib           = 0;
	uint64_t mem_available_mib = 0;

	bool        zswap_supported   = false;  // /sys/module/zswap exists
	bool        zswap_enabled     = false;
	std::string zswap_compressor;
	uint32_t    zswap_pool_percent = 0;
	bool        zswap_shrinker     = false;
	// Whether zswap comes back on after a reboot: the kernel command line, a tmpfiles.d line or Kyty's
	// own unit says so. A runtime-only `echo 1 > .../enabled` does not count.
	bool        zswap_persistent   = false;

	std::vector<SwapDevice> swaps;
	uint64_t                zram_orig_mib = 0;  // data held by the zram devices before compression
	uint32_t                zram_at_boot  = 0;  // zram devices zram-generator would create at the next boot

	bool has_systemd    = false;
	bool has_pkexec     = false;
	bool zram_generator = false;  // the zram-generator program is installed

	bool emulator_running = false;

	// What the launcher itself installed earlier.
	bool applied_unit   = false;  // /etc/systemd/system/kyty-host-memory.service
	bool applied_dropin = false;  // /etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf

	// Where a swapfile would go.
	std::string swap_dir_fstype;       // empty: unknown
	uint64_t    swap_dir_free_mib = 0;
};

enum class Action {
	None,
	EnableZswap,     // zswap on; zram swap off when a disk swap exists
	CreateSwapfile,  // add a swapfile, then zswap on and zram swap off
};

struct Advice {
	Verdict  verdict      = Verdict::Ok;
	Reason   reason       = Reason::Unknown;
	Action   action       = Action::None;
	bool     disable_zram = false;
	uint32_t pool_percent = 0;
	uint64_t swapfile_mib = 0;
	bool     blocked      = false;  // the action cannot run now (emulator running)
	bool     undo_available = false;
};

uint64_t DiskSwapMiB(const LinuxFacts& facts);
uint64_t ActiveZramCount(const LinuxFacts& facts);
bool     SwapfileFsSupported(const std::string& fstype);

Advice DecideLinux(const LinuxFacts& facts);

// One line per fact for the dialog and for `launcher --host-memory`.
std::string DescribeLinux(const LinuxFacts& facts);
// A sentence or two: why this verdict.
std::string ExplainLinux(const LinuxFacts& facts, const Advice& advice);
// The drawbacks of the proposed action (empty when there is no action).
std::string DrawbacksLinux(const Advice& advice);
// What Apply does, in plain words (empty when there is no action).
std::string ActionSummaryLinux(const Advice& advice);
// Arguments of `kyty-host-memory apply` for this advice (empty when there is no action).
std::vector<std::string> ApplyArguments(const Advice& advice);

// ---- Parsers of /proc and /sys text (used by the probe, tested on fake files) ----

struct MeminfoValues {
	uint64_t total_kib     = 0;
	uint64_t available_kib = 0;
};
MeminfoValues           ParseMeminfo(const std::string& text);
// zswap.enabled=... on the kernel command line.
Tristate ParseCmdlineZswapEnabled(const std::string& cmdline);
// The last `w` line for /sys/module/zswap/parameters/enabled in the given tmpfiles.d files (in the order systemd
// applies them).
Tristate ParseTmpfilesZswapEnabled(const std::vector<std::string>& files_in_order);
// Number of zram devices zram-generator creates from these config texts (main file first, then drop-ins by file
// name): [zramN] sections whose last zram-size is not 0.
uint32_t ZramDevicesAtBoot(const std::vector<std::string>& files_in_order);
std::vector<SwapDevice> ParseProcSwaps(const std::string& text);
// Filesystem type of the mount that contains `path` (longest mount point), plus its "major:minor".
struct MountEntry {
	std::string fstype;
	std::string device;  // major:minor
	std::string mount_point;
};
MountEntry FindMount(const std::string& mountinfo_text, const std::string& path);

// ---- Windows ----

enum class PagefileMode { Unknown, SystemManaged, Fixed, None };

struct WindowsFacts {
	uint64_t     ram_mib            = 0;
	uint64_t     commit_limit_mib   = 0;
	uint64_t     commit_charged_mib = 0;
	PagefileMode pagefile           = PagefileMode::Unknown;
	uint64_t     pagefile_mib       = 0;  // size of the configured/current pagefile
	Tristate     memory_compression = Tristate::Unknown;
	bool         emulator_running   = false;
	bool         applied            = false;  // the launcher's state file exists
};

struct WindowsAdvice {
	Verdict verdict                  = Verdict::Ok;
	Reason  reason                   = Reason::Unknown;
	bool    enable_memory_compression = false;
	bool    system_managed_pagefile   = false;
	bool    blocked                  = false;
	bool    undo_available           = false;
};

WindowsAdvice DecideWindows(const WindowsFacts& facts);
// "key=value" lines printed by kyty-host-memory.ps1 -Action Status.
WindowsFacts ParseWindowsFacts(const std::string& text);
std::string  DescribeWindows(const WindowsFacts& facts);
std::string  ExplainWindows(const WindowsFacts& facts, const WindowsAdvice& advice);
std::string  DrawbacksWindows(const WindowsAdvice& advice);
std::string  ActionSummaryWindows(const WindowsAdvice& advice);

} // namespace HostMemory

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_POLICY_H_ */
