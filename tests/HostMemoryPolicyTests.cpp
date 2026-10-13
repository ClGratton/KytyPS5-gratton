// Host memory tuning of the launcher (docs/host-memory.md): the decision table, the parsers of /proc and /sys
// text, and the probe run on made-up directory trees. No system file is read or written outside the temporary
// tree these tests create, and nothing here needs root.

#include "hostMemoryPolicy.h"
#include "hostMemoryProbe.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace HostMemory;
namespace fs = std::filesystem;

int g_failures = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HostMemoryPolicyTests: failed: %s\n", message);
		g_failures++;
	}
}

constexpr uint64_t kGiB = 1024;

SwapDevice Swap(const char* name, SwapKind kind, uint64_t size_mib, int priority, bool rotational = false) {
	SwapDevice swap;
	swap.name       = name;
	swap.kind       = kind;
	swap.size_mib   = size_mib;
	swap.priority   = priority;
	swap.rotational = rotational;
	return swap;
}

// A 16 GB laptop with the usual Linux layout: zram in front of a disk swapfile on btrfs, zswap off.
LinuxFacts Laptop16() {
	LinuxFacts facts;
	facts.ram_mib           = 15026;
	facts.mem_available_mib = 9000;
	facts.zswap_supported   = true;
	facts.has_systemd       = true;
	facts.has_pkexec        = true;
	facts.zram_generator    = true;
	facts.zram_at_boot      = 1;
	facts.swaps             = {Swap("/dev/zram0", SwapKind::Zram, 15026, 100), Swap("/swap/swapfile", SwapKind::File, 15872, 0)};
	facts.swap_dir_fstype   = "btrfs";
	facts.swap_dir_free_mib = 200 * kGiB;
	return facts;
}

void TestConstants() {
	Check(ExpectedEmulatorPeakMiB() >= 9 * kGiB + 400 && ExpectedEmulatorPeakMiB() <= 10 * kGiB,
	      "the expected peak is about 9.6 GiB (8.2 GiB guest plus overhead)");
	Check(kGuestBackingMiB == 13824, "the guest backing size matches PhysicalMemory::TotalSize()");
	// 16 GB machines (15-15.7 GiB of MemTotal) are tuned, 24 GB machines are not.
	Check(RequiredTotalMiB() > 16 * kGiB && RequiredTotalMiB() < 20 * kGiB, "the threshold sits between 16 and 20 GiB");
	Check(RequiredCommitHeadroomMiB() > kGuestBackingMiB, "the commit headroom covers the whole backing section");
}

void TestScaling() {
	Check(ZswapPoolPercent(15026) == 21, "pool percent on a 16 GB machine is about 20%");
	Check(ZswapPoolPercent(8 * kGiB) == kZswapPoolMaxPercent, "small hosts are capped at the maximum share");
	Check(ZswapPoolPercent(64 * kGiB) == kZswapPoolMinPercent, "big hosts get the minimum share");
	Check(ZswapPoolPercent(0) == kZswapPoolMinPercent, "no RAM figure gives the minimum");
	Check(SwapfileSizeMiB(15026, 200 * kGiB) == 8 * kGiB, "half of 15 GiB is under the minimum: 8 GiB");
	Check(SwapfileSizeMiB(64 * kGiB, 200 * kGiB) == 16 * kGiB, "the swapfile is capped at 16 GiB");
	Check(SwapfileSizeMiB(24 * kGiB, 200 * kGiB) == 12 * kGiB, "half of RAM in between");
	Check(SwapfileSizeMiB(15026, 22000) == 22000 - kDiskReserveMiB, "a tight disk shrinks the swapfile, keeping the reserve");
	Check(SwapfileSizeMiB(15026, kDiskReserveMiB + 1000) == 0, "too little free space: no swapfile");
	Check(SwapfileSizeMiB(15026, 0) == 0, "unknown free space: no swapfile");
}

void TestLinuxDecisionTable() {
	// Enough RAM: nothing to do, whatever the swap looks like.
	{
		auto facts    = Laptop16();
		facts.ram_mib = 24 * kGiB;
		const auto a  = DecideLinux(facts);
		Check(a.verdict == Verdict::Ok && a.reason == Reason::EnoughRam && a.action == Action::None, "24 GB: ok");
		facts.ram_mib = 32 * kGiB;
		Check(DecideLinux(facts).verdict == Verdict::Ok, "32 GB: ok");
		facts.ram_mib = RequiredTotalMiB();
		Check(DecideLinux(facts).verdict == Verdict::Ok, "exactly the threshold: ok");
		facts.ram_mib = RequiredTotalMiB() - 1;
		Check(DecideLinux(facts).verdict != Verdict::Ok, "one MiB below the threshold: tuned");
	}
	// Low RAM, disk swap, zram in front, zswap off: zswap on and zram off.
	{
		const auto a = DecideLinux(Laptop16());
		Check(a.verdict == Verdict::Recommended && a.action == Action::EnableZswap, "zram + disk swap: enable zswap");
		Check(a.disable_zram && a.reason == Reason::ZramInFront, "zram is switched off");
		Check(a.pool_percent == 21, "pool scaled to RAM");
		Check(a.swapfile_mib == 0, "no swapfile when a disk swap exists");
		const auto args = ApplyArguments(a);
		Check(args.size() == 6 && args[0] == "apply" && args[2] == "21" && args[4] == "zstd" && args[5] == "--disable-zram",
		      "apply arguments for zswap + zram off");
	}
	// Low RAM, disk swap, no zram, zswap off.
	{
		auto facts  = Laptop16();
		facts.swaps = {Swap("/dev/nvme0n1p3", SwapKind::Partition, 8 * kGiB, -2)};
		facts.zram_at_boot = 0;
		const auto a = DecideLinux(facts);
		Check(a.verdict == Verdict::Recommended && a.reason == Reason::ZswapOff && !a.disable_zram, "disk swap only: zswap on");
	}
	// Already in the proposed state.
	{
		auto facts                = Laptop16();
		facts.swaps               = {Swap("/swap/swapfile", SwapKind::File, 15872, 0)};
		facts.zram_at_boot        = 0;
		facts.zswap_persistent    = true;
		facts.zswap_enabled       = true;
		facts.zswap_pool_percent  = 20;
		facts.zswap_compressor    = "zstd";
		const auto a              = DecideLinux(facts);
		Check(a.verdict == Verdict::Tuned && a.action == Action::None, "zswap on, no zram: tuned");
		facts.applied_unit = true;
		Check(DecideLinux(facts).undo_available, "undo is offered when the launcher's unit is installed");
		facts.swaps.push_back(Swap("/dev/zram0", SwapKind::Zram, 1000, 100));
		Check(DecideLinux(facts).action == Action::EnableZswap, "zswap on but zram still active: switch zram off");
		facts.swaps.pop_back();
		// zswap on and zram off right now, but only until the next reboot (a runtime-only setup).
		facts.zswap_persistent = false;
		auto b                 = DecideLinux(facts);
		Check(b.verdict == Verdict::Recommended && b.reason == Reason::NotPersistent && b.action == Action::EnableZswap,
		      "zswap on but not kept across reboots: make it permanent");
		facts.zswap_persistent = true;
		facts.zram_at_boot     = 1;
		b                      = DecideLinux(facts);
		Check(b.verdict == Verdict::Recommended && b.action == Action::EnableZswap && b.disable_zram,
		      "zram is off now but comes back at boot: the drop-in is needed");
	}
	// Swap on a spinning disk: leave it alone.
	{
		auto facts  = Laptop16();
		facts.swaps = {Swap("/dev/sda2", SwapKind::Partition, 8 * kGiB, -2, true)};
		const auto a = DecideLinux(facts);
		Check(a.verdict == Verdict::NotRecommended && a.reason == Reason::RotationalSwap && a.action == Action::None,
		      "swap on a spinning disk: not recommended");
	}
	// No disk swap: keep zram, offer a swapfile.
	{
		auto facts  = Laptop16();
		facts.swaps = {Swap("/dev/zram0", SwapKind::Zram, 7 * kGiB, 100)};
		const auto a = DecideLinux(facts);
		Check(a.verdict == Verdict::Optional && a.action == Action::CreateSwapfile && a.reason == Reason::NoDiskSwap,
		      "zram only: a swapfile is optional");
		Check(a.swapfile_mib == 8 * kGiB && a.disable_zram, "8 GiB swapfile, zram off once it exists");
		const auto args = ApplyArguments(a);
		Check(args.size() == 8 && args[6] == "--swapfile-mib" && args[7] == "8192", "apply arguments with a swapfile");
	}
	// No swap at all: a swapfile is recommended; nothing to switch off.
	{
		auto facts  = Laptop16();
		facts.swaps = {};
		facts.zram_at_boot = 0;
		const auto a = DecideLinux(facts);
		Check(a.verdict == Verdict::Recommended && a.action == Action::CreateSwapfile && !a.disable_zram,
		      "no swap at all: swapfile recommended");
	}
	// A small disk swap counts as no disk swap.
	{
		auto facts  = Laptop16();
		facts.swaps = {Swap("/swapfile", SwapKind::File, 1024, -2)};
		const auto a = DecideLinux(facts);
		Check(a.reason == Reason::SmallDiskSwap && a.action == Action::CreateSwapfile, "a 1 GiB swap is too small");
	}
	// No disk swap, and no room or no supported filesystem for a swapfile.
	{
		auto facts              = Laptop16();
		facts.swaps             = {Swap("/dev/zram0", SwapKind::Zram, 7 * kGiB, 100)};
		facts.swap_dir_free_mib = 10 * kGiB;
		auto a                  = DecideLinux(facts);
		Check(a.verdict == Verdict::NotRecommended && a.reason == Reason::NoSwapfileSpace && a.action == Action::None,
		      "disk too full for a swapfile");
		facts.swap_dir_free_mib = 200 * kGiB;
		facts.swap_dir_fstype   = "zfs";
		a                       = DecideLinux(facts);
		Check(a.reason == Reason::NoSwapfileFs && a.action == Action::None, "swapfile on zfs is not offered");
		facts.swap_dir_fstype = "";
		Check(DecideLinux(facts).reason == Reason::NoSwapfileFs, "unknown filesystem: not offered");
	}
	// Hosts the launcher cannot tune.
	{
		auto facts            = Laptop16();
		facts.zswap_supported = false;
		Check(DecideLinux(facts).verdict == Verdict::Unsupported, "no zswap in the kernel");
		facts             = Laptop16();
		facts.has_systemd = false;
		Check(DecideLinux(facts).reason == Reason::NoSystemd, "no systemd: cannot persist");
	}
	// An emulator that is running blocks the action but not the verdict.
	{
		auto facts             = Laptop16();
		facts.emulator_running = true;
		const auto a           = DecideLinux(facts);
		Check(a.blocked && a.action == Action::EnableZswap, "running emulator blocks applying");
	}
	// Text helpers do not crash and mention the essentials.
	{
		const auto facts = Laptop16();
		const auto a     = DecideLinux(facts);
		Check(DescribeLinux(facts).find("zram") != std::string::npos, "the description lists zram");
		Check(!ExplainLinux(facts, a).empty() && !DrawbacksLinux(a).empty(), "advice has an explanation and drawbacks");
		const auto summary = ActionSummaryLinux(a);
		Check(summary.find("zswap") != std::string::npos && summary.find("zram") != std::string::npos && summary.find("21%") != std::string::npos,
		      "the summary says what Apply does");
		Check(ActionSummaryLinux(Advice {}).empty(), "no summary without an action");
		Check(DrawbacksLinux(DecideLinux([] { auto f = Laptop16(); f.ram_mib = 32 * kGiB; return f; }())).empty(),
		      "no drawbacks text without an action");
	}
}

void TestParsers() {
	const auto mem = ParseMeminfo("MemTotal:       15387372 kB\nMemFree:         1000 kB\nMemAvailable:    9437184 kB\n");
	Check(mem.total_kib == 15387372 && mem.available_kib == 9437184, "meminfo");

	const auto swaps = ParseProcSwaps("Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
	                                  "/dev/zram0                              partition\t15387368\t1024\t\t100\n"
	                                  "/swap/swapfile                          file\t\t16252928\t0\t\t-2\n"
	                                  "/dev/nvme0n1p3                          partition\t8388604\t0\t\t-3\n");
	Check(swaps.size() == 3, "three swap devices");
	Check(swaps[0].kind == SwapKind::Zram && swaps[0].size_mib == 15026 && swaps[0].used_mib == 1 && swaps[0].priority == 100,
	      "zram swap parsed (partition type, zram name)");
	Check(swaps[1].kind == SwapKind::File && swaps[1].priority == -2, "file swap with negative priority");
	Check(swaps[2].kind == SwapKind::Partition, "partition swap");
	Check(ParseProcSwaps("Filename Type Size Used Priority\n").empty(), "no swap devices");

	// The kernel command line, tmpfiles.d and zram-generator configuration.
	Check(ParseCmdlineZswapEnabled("quiet zswap.enabled=0 splash") == Tristate::No, "cmdline: zswap off");
	Check(ParseCmdlineZswapEnabled("zswap.enabled=1 zswap.enabled=0") == Tristate::No, "cmdline: the last one wins");
	Check(ParseCmdlineZswapEnabled("zswap.enabled=Y") == Tristate::Yes && ParseCmdlineZswapEnabled("quiet") == Tristate::Unknown,
	      "cmdline: on, and not mentioned");
	Check(ParseTmpfilesZswapEnabled({"# omarchy\nw! /sys/module/zswap/parameters/enabled - - - - N\n"}) == Tristate::No,
	      "tmpfiles: zswap off");
	Check(ParseTmpfilesZswapEnabled({"w! /sys/module/zswap/parameters/enabled - - - - N\n", "w /sys/module/zswap/parameters/enabled - - - - Y\n"}) ==
	          Tristate::Yes,
	      "tmpfiles: a later file wins");
	Check(ParseTmpfilesZswapEnabled({"w /sys/kernel/mm/transparent_hugepage/enabled - - - - madvise\n"}) == Tristate::Unknown,
	      "tmpfiles: other lines do not count");
	Check(ZramDevicesAtBoot({"[zram0]\nzram-size = ram\nswap-priority = 100\n"}) == 1, "zram-generator: one device");
	Check(ZramDevicesAtBoot({"[zram0]\nzram-size = ram\n", "[zram0]\nzram-size = 0\n"}) == 0, "zram-generator: the drop-in disables it");
	Check(ZramDevicesAtBoot({"[zram0]\nzram-size = ram\n[zram1]\nzram-size = 2048\n"}) == 2, "zram-generator: two devices");
	Check(ZramDevicesAtBoot({"[zram0]\nzram-size = 0\n", "[zram0]\nzram-size = ram / 2\n"}) == 1, "zram-generator: the later file re-enables it");
	Check(ZramDevicesAtBoot({"# nothing\n"}) == 0 && ZramDevicesAtBoot({}) == 0, "zram-generator: no devices");

	const std::string mountinfo =
	    "26 1 0:25 /@ / rw,relatime shared:1 - btrfs /dev/mapper/root rw,ssd,subvolid=256\n"
	    "27 26 0:6 / /dev rw,nosuid shared:2 - devtmpfs dev rw\n"
	    "40 26 259:1 / /boot rw,relatime shared:9 - vfat /dev/nvme0n1p1 rw\n"
	    "41 26 8:17 / /mnt/my\\040disk rw,relatime - ext4 /dev/sdb1 rw\n"
	    "42 26 0:30 / /home rw,relatime - btrfs /dev/mapper/root rw\n";
	Check(FindMount(mountinfo, "/var/lib/kyty-host-memory").fstype == "btrfs", "root filesystem");
	Check(FindMount(mountinfo, "/boot/loader").fstype == "vfat", "longest mount point wins");
	Check(FindMount(mountinfo, "/bootx").fstype == "btrfs", "a prefix that is not a path component does not match");
	const auto spaced = FindMount(mountinfo, "/mnt/my disk/swap");
	Check(spaced.fstype == "ext4" && spaced.device == "8:17", "escaped space in a mount point");
	Check(FindMount("", "/x").fstype.empty(), "no mountinfo: unknown");
}

// ---- The probe on fake trees ----

void Put(const fs::path& path, const std::string& text) {
	fs::create_directories(path.parent_path());
	std::ofstream(path) << text;
}

struct FakeRoot {
	fs::path dir;
	FakeRoot() {
		dir = fs::temp_directory_path() / ("kyty-hm-test-" + std::to_string(static_cast<unsigned long>(std::rand())) + "-" +
		                                   std::to_string(reinterpret_cast<uintptr_t>(this) & 0xffffff));
		fs::remove_all(dir);
		fs::create_directories(dir);
	}
	~FakeRoot() {
		std::error_code error;
		fs::remove_all(dir, error);
	}
	void Write(const std::string& relative, const std::string& text) const { Put(dir / relative, text); }
	[[nodiscard]] ProbeEnv Env(uint64_t free_mib) const {
		ProbeEnv env;
		env.root     = dir.string();
		env.free_mib = [free_mib](const std::string&) { return free_mib; };
		return env;
	}
};

void FillLaptop(const FakeRoot& r) {
	r.Write("proc/meminfo", "MemTotal: 15387372 kB\nMemAvailable: 9437184 kB\n");
	r.Write("proc/swaps", "Filename Type Size Used Priority\n/dev/zram0 partition 15387368 1024 100\n"
	                      "/swap/swapfile file 16252928 0 -2\n");
	r.Write("proc/self/mountinfo", "26 1 0:25 /@ / rw - btrfs /dev/mapper/root rw\n");
	r.Write("sys/module/zswap/parameters/enabled", "N\n");
	r.Write("sys/module/zswap/parameters/compressor", "zstd\n");
	r.Write("sys/module/zswap/parameters/max_pool_percent", "20\n");
	r.Write("sys/module/zswap/parameters/shrinker_enabled", "N\n");
	r.Write("sys/block/zram0/mm_stat", "2147483648 700000000 800000000 0 800000000 0 0 0 0\n");
	r.Write("run/systemd/system/.keep", "");
	r.Write("usr/bin/pkexec", "");
	r.Write("usr/lib/systemd/system-generators/zram-generator", "");
	r.Write("proc/cmdline", "quiet splash zswap.enabled=0\n");
	r.Write("usr/lib/tmpfiles.d/omarchy-zswap.conf", "w! /sys/module/zswap/parameters/enabled - - - - N\n");
	r.Write("usr/lib/systemd/zram-generator.conf.d/90-omarchy.conf", "[zram0]\nzram-size = ram\nswap-priority = 100\n");
	r.Write("proc/1/cmdline", std::string("/sbin/init\0", 11));
	r.Write("proc/self/cmdline", std::string("launcher\0", 9));
	fs::create_directories(r.dir / "var/lib");
}

void TestProbe() {
	{
		FakeRoot r;
		FillLaptop(r);
		const auto facts = ProbeLinux(r.Env(200 * kGiB));
		Check(facts.ram_mib == 15026 && facts.mem_available_mib == 9216, "probe: memory");
		Check(facts.zswap_supported && !facts.zswap_enabled && facts.zswap_compressor == "zstd" && facts.zswap_pool_percent == 20 &&
		          !facts.zswap_shrinker,
		      "probe: zswap parameters");
		Check(facts.swaps.size() == 2 && ActiveZramCount(facts) == 1 && DiskSwapMiB(facts) == 15872, "probe: swaps");
		Check(facts.zram_orig_mib == 2048, "probe: zram holds 2 GiB of pages");
		Check(facts.has_systemd && facts.has_pkexec && facts.zram_generator, "probe: systemd, pkexec and zram-generator");
		Check(!facts.zswap_persistent && facts.zram_at_boot == 1, "probe: zswap is turned off at boot, zram0 is created at boot");
		Check(!facts.emulator_running && !facts.applied_unit && !facts.applied_dropin, "probe: nothing running or installed");
		Check(facts.swap_dir_fstype == "btrfs" && facts.swap_dir_free_mib == 200 * kGiB, "probe: swapfile location");
		const auto a = DecideLinux(facts);
		Check(a.verdict == Verdict::Recommended && a.action == Action::EnableZswap && a.disable_zram, "probe + decide: the usual laptop");
	}
	{
		FakeRoot r;
		FillLaptop(r);
		r.Write("proc/4242/cmdline", std::string("/home/u/.local/opt/kyty-gratton/kyty_emulator\0--game\0x\0", 49));
		r.Write("etc/systemd/system/kyty-host-memory.service", "[Unit]\n");
		r.Write("etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf", "[zram0]\nzram-size = 0\n");
		r.Write("sys/module/zswap/parameters/enabled", "Y\n");
		const auto facts = ProbeLinux(r.Env(200 * kGiB));
		Check(facts.emulator_running, "probe: a kyty_emulator process is seen");
		Check(facts.applied_unit && facts.applied_dropin && facts.zswap_enabled, "probe: installed files and zswap on");
		Check(facts.zswap_persistent && facts.zram_at_boot == 0, "probe: Kyty's unit keeps zswap on, its drop-in removes zram0 at boot");
		Check(DecideLinux(facts).undo_available && DecideLinux(facts).blocked, "decide: undo offered, apply blocked");
	}
	{
		// A host with partition swap on a spinning disk, and a launcher-less install (no systemd).
		FakeRoot r;
		FillLaptop(r);
		r.Write("proc/swaps", "Filename Type Size Used Priority\n/dev/sda2 partition 8388604 0 -2\n");
		r.Write("sys/class/block/sda/queue/rotational", "1\n");
		fs::create_directories(r.dir / "sys/class/block/sda/sda2");
		fs::create_directory_symlink(r.dir / "sys/class/block/sda/sda2", r.dir / "sys/class/block/sda2");
		fs::create_directories(r.dir / "dev");
		Put(r.dir / "dev/sda2", "");
		const auto facts = ProbeLinux(r.Env(200 * kGiB));
		Check(facts.swaps.size() == 1 && facts.swaps[0].rotational, "probe: swap on a spinning disk");
		Check(DecideLinux(facts).reason == Reason::RotationalSwap, "decide: spinning disk");
	}
	{
		FakeRoot r;  // an empty tree: nothing readable, must not crash and must say unsupported
		const auto facts = ProbeLinux(r.Env(0));
		Check(!facts.zswap_supported && facts.ram_mib == 0 && DecideLinux(facts).verdict == Verdict::Unsupported,
		      "probe: an empty tree is unsupported");
	}
}

// ---- Windows (decision only; the probe and the elevated helper are not run here) ----

WindowsFacts Pc16(PagefileMode mode, uint64_t commit_limit, uint64_t charged) {
	WindowsFacts facts;
	facts.ram_mib            = 16 * kGiB;
	facts.commit_limit_mib   = commit_limit;
	facts.commit_charged_mib = charged;
	facts.pagefile           = mode;
	facts.memory_compression = Tristate::Yes;
	return facts;
}

void TestWindows() {
	{
		const auto a = DecideWindows(Pc16(PagefileMode::SystemManaged, 24 * kGiB, 6 * kGiB));
		Check(a.verdict == Verdict::Ok && !a.system_managed_pagefile && !a.enable_memory_compression, "windows: system managed pagefile is fine");
	}
	{
		// 16 GB RAM with a fixed 2 GB pagefile: limit about 18 GB, 7 GB charged, headroom 11 GB < 15.4 GB.
		const auto a = DecideWindows(Pc16(PagefileMode::Fixed, 18 * kGiB, 7 * kGiB));
		Check(a.verdict == Verdict::Recommended && a.system_managed_pagefile && a.reason == Reason::CommitLimit,
		      "windows: a small fixed pagefile leaves too little commit");
	}
	{
		const auto a = DecideWindows(Pc16(PagefileMode::None, 16 * kGiB, 5 * kGiB));
		Check(a.system_managed_pagefile, "windows: no pagefile at all");
	}
	{
		// A big fixed pagefile is fine.
		const auto a = DecideWindows(Pc16(PagefileMode::Fixed, 48 * kGiB, 7 * kGiB));
		Check(a.verdict == Verdict::Ok, "windows: a large fixed pagefile is left alone");
	}
	{
		auto facts               = Pc16(PagefileMode::SystemManaged, 40 * kGiB, 6 * kGiB);
		facts.memory_compression = Tristate::No;
		auto a                   = DecideWindows(facts);
		Check(a.enable_memory_compression && a.reason == Reason::CompressionOff && !a.system_managed_pagefile,
		      "windows: compression off on a 16 GB machine");
		facts.ram_mib = 32 * kGiB;
		Check(DecideWindows(facts).verdict == Verdict::Ok, "windows: compression off but 32 GB: fine");
		facts.ram_mib            = 16 * kGiB;
		facts.memory_compression = Tristate::Unknown;
		Check(DecideWindows(facts).verdict == Verdict::Ok, "windows: unknown compression state: no action");
	}
	{
		auto facts                = Pc16(PagefileMode::Fixed, 18 * kGiB, 7 * kGiB);
		facts.memory_compression  = Tristate::No;
		facts.applied             = true;
		facts.emulator_running    = true;
		const auto a              = DecideWindows(facts);
		Check(a.system_managed_pagefile && a.enable_memory_compression && a.blocked && a.undo_available, "windows: both problems, blocked while running, undo available");
		Check(!ExplainWindows(facts, a).empty() && !DrawbacksWindows(a).empty() && !DescribeWindows(facts).empty(), "windows: texts");
		Check(ActionSummaryWindows(a).find("pagefile") != std::string::npos && ActionSummaryWindows(a).find("compression") != std::string::npos,
		      "windows: the summary names both changes");
		Check(ActionSummaryWindows(WindowsAdvice {}).empty(), "windows: no summary without an action");
	}
	{
		auto facts = Pc16(PagefileMode::SystemManaged, 40 * kGiB, 6 * kGiB);
		facts.applied = true;
		Check(DecideWindows(facts).verdict == Verdict::Tuned, "windows: applied and fine is tuned");
	}
	{
		const auto facts = ParseWindowsFacts("ram_mib=16384\ncommit_limit_mib=18432\ncommit_charged_mib=7168\n"
		                                     "pagefile_mode=fixed\npagefile_mib=2048\nmemory_compression=false\n"
		                                     "emulator_running=0\napplied=1\njunk line\n");
		Check(facts.ram_mib == 16384 && facts.commit_limit_mib == 18432 && facts.commit_charged_mib == 7168, "windows: parsed numbers");
		Check(facts.pagefile == PagefileMode::Fixed && facts.pagefile_mib == 2048 && facts.memory_compression == Tristate::No &&
		          !facts.emulator_running && facts.applied,
		      "windows: parsed modes");
		const auto empty = ParseWindowsFacts("");
		Check(empty.pagefile == PagefileMode::Unknown && empty.memory_compression == Tristate::Unknown, "windows: empty status is unknown");
		Check(DecideWindows(empty).verdict == Verdict::Ok, "windows: unknown host: no action");
	}
}

} // namespace

int main() {
	TestConstants();
	TestScaling();
	TestLinuxDecisionTable();
	TestParsers();
	TestProbe();
	TestWindows();
	if (g_failures != 0) {
		std::fprintf(stderr, "HostMemoryPolicyTests: %d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("HostMemoryPolicyTests: all checks passed\n");
	return 0;
}
