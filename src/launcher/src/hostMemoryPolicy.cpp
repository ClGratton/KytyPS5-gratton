#include "hostMemoryPolicy.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace HostMemory {

namespace {

std::string Gib(uint64_t mib) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(mib) / 1024.0);
	return buffer;
}

std::string Trim(const std::string& text) {
	size_t first = 0;
	size_t last  = text.size();
	while (first < last && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
		first++;
	}
	while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
		last--;
	}
	return text.substr(first, last - first);
}

std::vector<std::string> SplitSpaces(const std::string& line) {
	std::vector<std::string> out;
	std::istringstream       stream(line);
	std::string              word;
	while (stream >> word) {
		out.push_back(word);
	}
	return out;
}

uint64_t ToU64(const std::string& text) {
	char* end   = nullptr;
	auto  value = std::strtoull(text.c_str(), &end, 10);
	return end == text.c_str() ? 0 : static_cast<uint64_t>(value);
}

// mountinfo escapes space, tab, newline and backslash as \040, \011, \012, \134.
std::string UnescapeMountPath(const std::string& text) {
	std::string out;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 3 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0 &&
		    std::isdigit(static_cast<unsigned char>(text[i + 2])) != 0 &&
		    std::isdigit(static_cast<unsigned char>(text[i + 3])) != 0) {
			out.push_back(static_cast<char>(((text[i + 1] - '0') << 6) | ((text[i + 2] - '0') << 3) |
			                                (text[i + 3] - '0')));
			i += 3;
		} else {
			out.push_back(text[i]);
		}
	}
	return out;
}

} // namespace

uint32_t ZswapPoolPercent(uint64_t ram_mib) {
	if (ram_mib == 0) {
		return kZswapPoolMinPercent;
	}
	const uint64_t percent = (kZswapPoolTargetMiB * 100 + ram_mib - 1) / ram_mib;
	return static_cast<uint32_t>(std::clamp<uint64_t>(percent, kZswapPoolMinPercent, kZswapPoolMaxPercent));
}

uint64_t SwapfileSizeMiB(uint64_t ram_mib, uint64_t free_disk_mib) {
	const uint64_t target    = std::clamp<uint64_t>(ram_mib / 2, kSwapfileMinMiB, kSwapfileMaxMiB);
	const uint64_t available = free_disk_mib > kDiskReserveMiB ? free_disk_mib - kDiskReserveMiB : 0;
	const uint64_t size      = std::min(target, available);
	return size >= kMinUsefulDiskSwapMiB ? size : 0;
}

const char* VerdictName(Verdict verdict) {
	switch (verdict) {
		case Verdict::Ok: return "ok";
		case Verdict::Recommended: return "recommended";
		case Verdict::Optional: return "optional";
		case Verdict::Tuned: return "tuned";
		case Verdict::NotRecommended: return "not-recommended";
		case Verdict::Unsupported: return "unsupported";
	}
	return "?";
}

const char* ReasonName(Reason reason) {
	switch (reason) {
		case Reason::EnoughRam: return "enough-ram";
		case Reason::NoZswap: return "no-zswap";
		case Reason::NoSystemd: return "no-systemd";
		case Reason::NoDiskSwap: return "no-disk-swap";
		case Reason::SmallDiskSwap: return "small-disk-swap";
		case Reason::AlreadyTuned: return "already-tuned";
		case Reason::ZramInFront: return "zram-in-front";
		case Reason::ZswapOff: return "zswap-off";
		case Reason::RotationalSwap: return "rotational-swap";
		case Reason::NoSwapfileSpace: return "no-swapfile-space";
		case Reason::NoSwapfileFs: return "no-swapfile-fs";
		case Reason::CommitLimit: return "commit-limit";
		case Reason::CompressionOff: return "compression-off";
		case Reason::Unknown: return "unknown";
	}
	return "?";
}

uint64_t DiskSwapMiB(const LinuxFacts& facts) {
	uint64_t total = 0;
	for (const auto& swap: facts.swaps) {
		if (swap.kind != SwapKind::Zram) {
			total += swap.size_mib;
		}
	}
	return total;
}

uint64_t ActiveZramCount(const LinuxFacts& facts) {
	uint64_t count = 0;
	for (const auto& swap: facts.swaps) {
		if (swap.kind == SwapKind::Zram) {
			count++;
		}
	}
	return count;
}

bool SwapfileFsSupported(const std::string& fstype) {
	return fstype == "ext4" || fstype == "xfs" || fstype == "btrfs";
}

Advice DecideLinux(const LinuxFacts& facts) {
	Advice advice;
	advice.undo_available = facts.applied_unit || facts.applied_dropin;
	advice.blocked        = facts.emulator_running;
	advice.pool_percent   = ZswapPoolPercent(facts.ram_mib);

	if (!facts.zswap_supported) {
		advice.verdict = Verdict::Unsupported;
		advice.reason  = Reason::NoZswap;
		return advice;
	}
	if (!facts.has_systemd) {
		advice.verdict = Verdict::Unsupported;
		advice.reason  = Reason::NoSystemd;
		return advice;
	}
	if (facts.ram_mib >= RequiredTotalMiB()) {
		advice.verdict = Verdict::Ok;
		advice.reason  = Reason::EnoughRam;
		return advice;
	}

	const uint64_t disk_swap = DiskSwapMiB(facts);
	const uint64_t zram      = ActiveZramCount(facts);

	if (disk_swap >= kMinUsefulDiskSwapMiB) {
		for (const auto& swap: facts.swaps) {
			if (swap.kind != SwapKind::Zram && swap.rotational) {
				advice.verdict = Verdict::NotRecommended;
				advice.reason  = Reason::RotationalSwap;
				return advice;
			}
		}
		if (facts.zswap_enabled && facts.zswap_pool_percent >= kZswapPoolMinPercent && zram == 0) {
			advice.verdict = Verdict::Tuned;
			advice.reason  = Reason::AlreadyTuned;
			return advice;
		}
		advice.verdict      = Verdict::Recommended;
		advice.reason       = zram > 0 ? Reason::ZramInFront : Reason::ZswapOff;
		advice.action       = Action::EnableZswap;
		advice.disable_zram = zram > 0;
		return advice;
	}

	// No disk swap worth the name: keep zram. A swapfile is offered when the disk can take one.
	advice.reason = disk_swap == 0 ? Reason::NoDiskSwap : Reason::SmallDiskSwap;
	if (!SwapfileFsSupported(facts.swap_dir_fstype)) {
		advice.verdict = Verdict::NotRecommended;
		advice.reason  = Reason::NoSwapfileFs;
		return advice;
	}
	const uint64_t swapfile = SwapfileSizeMiB(facts.ram_mib, facts.swap_dir_free_mib);
	if (swapfile == 0) {
		advice.verdict = Verdict::NotRecommended;
		advice.reason  = Reason::NoSwapfileSpace;
		return advice;
	}
	// With no swap at all, any swap is an improvement; with zram in place the swapfile is a choice.
	advice.verdict      = facts.swaps.empty() ? Verdict::Recommended : Verdict::Optional;
	advice.action       = Action::CreateSwapfile;
	advice.swapfile_mib = swapfile;
	advice.disable_zram = zram > 0;
	return advice;
}

std::string DescribeLinux(const LinuxFacts& facts) {
	std::ostringstream out;
	out << "RAM: " << Gib(facts.ram_mib) << " (" << Gib(facts.mem_available_mib) << " available now)\n";
	out << "The emulator needs about " << Gib(ExpectedEmulatorPeakMiB()) << " at its peak; "
	    << "this host is left alone from " << Gib(RequiredTotalMiB()) << " of RAM\n";
	if (facts.swaps.empty()) {
		out << "Swap: none\n";
	}
	for (const auto& swap: facts.swaps) {
		const char* kind = swap.kind == SwapKind::Zram        ? "zram (compressed, in RAM)"
		                   : swap.kind == SwapKind::Partition ? "disk partition"
		                   : swap.kind == SwapKind::File      ? "disk file"
		                                                      : "other";
		out << "Swap: " << swap.name << ", " << kind << ", " << Gib(swap.size_mib) << ", " << Gib(swap.used_mib)
		    << " used, priority " << swap.priority << (swap.rotational ? ", spinning disk" : "") << "\n";
	}
	if (!facts.zswap_supported) {
		out << "zswap: not in this kernel\n";
	} else if (facts.zswap_enabled) {
		out << "zswap: on, " << (facts.zswap_compressor.empty() ? "?" : facts.zswap_compressor) << ", pool "
		    << facts.zswap_pool_percent << "% of RAM" << (facts.zswap_shrinker ? ", shrinker on" : "") << "\n";
	} else {
		out << "zswap: off\n";
	}
	out << "systemd: " << (facts.has_systemd ? "yes" : "no") << ", pkexec: " << (facts.has_pkexec ? "yes" : "no")
	    << (facts.applied_unit || facts.applied_dropin ? ", Kyty tuning installed" : "") << "\n";
	if (!facts.swap_dir_fstype.empty()) {
		out << "Swapfile location: " << facts.swap_dir_fstype << ", " << Gib(facts.swap_dir_free_mib) << " free\n";
	}
	return out.str();
}

std::string ExplainLinux(const LinuxFacts& facts, const Advice& advice) {
	std::ostringstream out;
	switch (advice.reason) {
		case Reason::EnoughRam:
			out << "This computer has " << Gib(facts.ram_mib) << " of RAM, enough for the emulator and a desktop. "
			    << "Nothing needs changing.";
			break;
		case Reason::NoZswap: out << "This kernel has no zswap, so the launcher has nothing to tune."; break;
		case Reason::NoSystemd:
			out << "The launcher keeps the setting across reboots with a systemd service, and this system "
			       "does not run systemd. Nothing is changed.";
			break;
		case Reason::AlreadyTuned:
			out << "Compressed swap with writeback to disk is already on. Nothing needs changing.";
			break;
		case Reason::ZramInFront:
			out << "This computer has " << Gib(facts.ram_mib) << " of RAM and the emulator can use " << Gib(ExpectedEmulatorPeakMiB())
			    << ". Swap on zram lives in RAM too, so under pressure it only moves the problem: the "
			       "level load was measured down to 0.1 GB free. zswap with your disk swap compresses the "
			       "same pages and writes the cold ones out to disk.";
			break;
		case Reason::ZswapOff:
			out << "This computer has " << Gib(facts.ram_mib) << " of RAM and the emulator can use " << Gib(ExpectedEmulatorPeakMiB())
			    << ". zswap keeps swapped pages compressed in RAM and writes the cold ones to your disk "
			       "swap instead of letting the system stall.";
			break;
		case Reason::RotationalSwap:
			out << "Your swap is on a spinning disk; writing compressed pages to it would stall the game. "
			       "Nothing is changed.";
			break;
		case Reason::NoDiskSwap:
			out << "This computer has " << Gib(facts.ram_mib) << " of RAM and no swap on disk";
			if (ActiveZramCount(facts) > 0) {
				out << ", only zram (compressed swap in RAM), which is kept as it is";
			}
			out << ". A swapfile on disk of " << Gib(advice.swapfile_mib)
			    << " lets zswap write cold pages out of RAM.";
			break;
		case Reason::SmallDiskSwap:
			out << "The swap on disk is only " << Gib(DiskSwapMiB(facts)) << ", too small to take what the "
			    << "emulator pushes out. A swapfile on disk of " << Gib(advice.swapfile_mib) << " fixes that.";
			break;
		case Reason::NoSwapfileSpace:
			out << "This computer has little RAM and no usable swap on disk, but the disk is too full to add "
			       "a swapfile and still keep "
			    << Gib(kDiskReserveMiB) << " free. Nothing is changed.";
			break;
		case Reason::NoSwapfileFs:
			out << "This computer has little RAM and no usable swap on disk, but the launcher can only create "
			       "a swapfile on ext4, xfs or btrfs. Nothing is changed.";
			break;
		default: break;
	}
	return out.str();
}

std::string DrawbacksLinux(const Advice& advice) {
	if (advice.action == Action::None) {
		return {};
	}
	std::ostringstream out;
	out << "Costs: compressing pages uses some CPU time while memory is tight (zstd, a few percent of one core). "
	       "When the compressed pool fills up, the coldest pages are written to the disk swap, so the SSD sees "
	       "some writes (a few hundred MB per session was measured).";
	if (advice.action == Action::CreateSwapfile) {
		out << " The swapfile takes " << Gib(advice.swapfile_mib) << " of disk space.";
	}
	out << " Undo removes everything the launcher installed.";
	return out.str();
}

std::vector<std::string> ApplyArguments(const Advice& advice) {
	std::vector<std::string> args;
	if (advice.action == Action::None) {
		return args;
	}
	args = {"apply", "--zswap-percent", std::to_string(advice.pool_percent), "--compressor", "zstd"};
	if (advice.disable_zram) {
		args.emplace_back("--disable-zram");
	}
	if (advice.action == Action::CreateSwapfile) {
		args.emplace_back("--swapfile-mib");
		args.push_back(std::to_string(advice.swapfile_mib));
	}
	return args;
}

MeminfoValues ParseMeminfo(const std::string& text) {
	MeminfoValues values;
	std::istringstream stream(text);
	std::string        line;
	while (std::getline(stream, line)) {
		const auto words = SplitSpaces(line);
		if (words.size() >= 2 && words[0] == "MemTotal:") {
			values.total_kib = ToU64(words[1]);
		} else if (words.size() >= 2 && words[0] == "MemAvailable:") {
			values.available_kib = ToU64(words[1]);
		}
	}
	return values;
}

std::vector<SwapDevice> ParseProcSwaps(const std::string& text) {
	std::vector<SwapDevice> swaps;
	std::istringstream      stream(text);
	std::string             line;
	bool                    first = true;
	while (std::getline(stream, line)) {
		if (first) {  // "Filename Type Size Used Priority"
			first = false;
			if (line.rfind("Filename", 0) == 0) {
				continue;
			}
		}
		const auto words = SplitSpaces(line);
		if (words.size() < 5) {
			continue;
		}
		SwapDevice swap;
		swap.name     = UnescapeMountPath(words[0]);
		swap.size_mib = ToU64(words[2]) / 1024;
		swap.used_mib = ToU64(words[3]) / 1024;
		swap.priority = std::atoi(words[4].c_str());
		if (swap.name.rfind("/dev/zram", 0) == 0) {
			swap.kind = SwapKind::Zram;
		} else if (words[1] == "partition") {
			swap.kind = SwapKind::Partition;
		} else if (words[1] == "file") {
			swap.kind = SwapKind::File;
		}
		swaps.push_back(swap);
	}
	return swaps;
}

MountEntry FindMount(const std::string& mountinfo_text, const std::string& path) {
	MountEntry         best;
	std::istringstream stream(mountinfo_text);
	std::string        line;
	while (std::getline(stream, line)) {
		const auto dash = line.find(" - ");
		if (dash == std::string::npos) {
			continue;
		}
		const auto left  = SplitSpaces(line.substr(0, dash));
		const auto right = SplitSpaces(line.substr(dash + 3));
		if (left.size() < 5 || right.empty()) {
			continue;
		}
		const std::string mount_point = UnescapeMountPath(left[4]);
		const bool        contains    = mount_point == "/" || path == mount_point ||
		                         (path.size() > mount_point.size() && path.compare(0, mount_point.size(), mount_point) == 0 &&
		                          path[mount_point.size()] == '/');
		// Later lines of the same mount point are stacked on top of earlier ones.
		if (contains && mount_point.size() >= best.mount_point.size()) {
			best.mount_point = mount_point;
			best.device      = left[2];
			best.fstype      = right[0];
		}
	}
	return best;
}

// ---- Windows ----

WindowsAdvice DecideWindows(const WindowsFacts& facts) {
	WindowsAdvice advice;
	advice.undo_available = facts.applied;
	advice.blocked        = facts.emulator_running;

	const uint64_t headroom = facts.commit_limit_mib > facts.commit_charged_mib
	                              ? facts.commit_limit_mib - facts.commit_charged_mib
	                              : 0;
	// The emulator's guest memory is a pagefile-backed section: its whole size is charged to the
	// commit limit when it is created, resident or not. A system-managed pagefile grows to make room;
	// a fixed or missing one does not.
	const bool pagefile_can_grow = facts.pagefile == PagefileMode::SystemManaged;
	if (facts.commit_limit_mib > 0 && !pagefile_can_grow && headroom < RequiredCommitHeadroomMiB()) {
		advice.system_managed_pagefile = true;
	}
	if (facts.memory_compression == Tristate::No && facts.ram_mib > 0 && facts.ram_mib < RequiredTotalMiB()) {
		advice.enable_memory_compression = true;
	}

	if (advice.system_managed_pagefile || advice.enable_memory_compression) {
		advice.verdict = Verdict::Recommended;
		advice.reason  = advice.system_managed_pagefile ? Reason::CommitLimit : Reason::CompressionOff;
	} else if (facts.applied) {
		advice.verdict = Verdict::Tuned;
		advice.reason  = Reason::AlreadyTuned;
	} else {
		advice.verdict = Verdict::Ok;
		advice.reason  = Reason::EnoughRam;
	}
	return advice;
}

WindowsFacts ParseWindowsFacts(const std::string& text) {
	WindowsFacts       facts;
	std::istringstream stream(text);
	std::string        line;
	while (std::getline(stream, line)) {
		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key   = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));
		if (key == "ram_mib") {
			facts.ram_mib = ToU64(value);
		} else if (key == "commit_limit_mib") {
			facts.commit_limit_mib = ToU64(value);
		} else if (key == "commit_charged_mib") {
			facts.commit_charged_mib = ToU64(value);
		} else if (key == "pagefile_mode") {
			facts.pagefile = value == "system"  ? PagefileMode::SystemManaged
			                 : value == "fixed" ? PagefileMode::Fixed
			                 : value == "none"  ? PagefileMode::None
			                                    : PagefileMode::Unknown;
		} else if (key == "pagefile_mib") {
			facts.pagefile_mib = ToU64(value);
		} else if (key == "memory_compression") {
			facts.memory_compression = value == "true" ? Tristate::Yes : value == "false" ? Tristate::No : Tristate::Unknown;
		} else if (key == "emulator_running") {
			facts.emulator_running = value == "1";
		} else if (key == "applied") {
			facts.applied = value == "1";
		}
	}
	return facts;
}

std::string DescribeWindows(const WindowsFacts& facts) {
	std::ostringstream out;
	out << "RAM: " << Gib(facts.ram_mib) << "\n";
	out << "Commit limit: " << Gib(facts.commit_limit_mib) << ", in use " << Gib(facts.commit_charged_mib) << ", free "
	    << Gib(facts.commit_limit_mib > facts.commit_charged_mib ? facts.commit_limit_mib - facts.commit_charged_mib : 0)
	    << "; the emulator needs " << Gib(RequiredCommitHeadroomMiB()) << " free\n";
	out << "Pagefile: "
	    << (facts.pagefile == PagefileMode::SystemManaged ? "system managed"
	        : facts.pagefile == PagefileMode::Fixed       ? "fixed size, " + Gib(facts.pagefile_mib)
	        : facts.pagefile == PagefileMode::None        ? "none"
	                                                      : "unknown")
	    << "\n";
	out << "Memory compression: "
	    << (facts.memory_compression == Tristate::Yes ? "on" : facts.memory_compression == Tristate::No ? "off" : "unknown")
	    << "\n";
	return out.str();
}

std::string ExplainWindows(const WindowsFacts& facts, const WindowsAdvice& advice) {
	std::ostringstream out;
	if (advice.system_managed_pagefile) {
		out << "The emulator's guest memory is charged to the system commit limit in full ("
		    << Gib(kGuestBackingMiB) << ") when it starts, even though only part of it is used. Your commit limit "
		    << "leaves " << Gib(facts.commit_limit_mib > facts.commit_charged_mib ? facts.commit_limit_mib - facts.commit_charged_mib : 0)
		    << " free, so the emulator would fail to start with a \"paging file is too small\" error. "
		       "A system-managed pagefile grows when needed.";
	}
	if (advice.enable_memory_compression) {
		if (advice.system_managed_pagefile) {
			out << " ";
		}
		out << "Windows memory compression is off. It is on by default and keeps idle memory compressed in RAM "
		       "instead of paging it out; with "
		    << Gib(facts.ram_mib) << " of RAM the emulator benefits from it.";
	}
	if (!advice.system_managed_pagefile && !advice.enable_memory_compression) {
		out << (advice.verdict == Verdict::Tuned ? "The launcher's changes are in place." : "Nothing needs changing.");
	}
	return out.str();
}

std::string DrawbacksWindows(const WindowsAdvice& advice) {
	std::ostringstream out;
	if (advice.system_managed_pagefile) {
		out << "The pagefile can grow on the system drive (up to three times RAM, only when needed). "
		       "A restart is needed for the new pagefile setting.";
	}
	if (advice.enable_memory_compression) {
		out << (advice.system_managed_pagefile ? " " : "")
		    << "Memory compression uses some CPU time while memory is tight. It takes effect after a restart.";
	}
	if (advice.system_managed_pagefile || advice.enable_memory_compression) {
		out << " Undo restores the previous settings.";
	}
	return out.str();
}

} // namespace HostMemory
