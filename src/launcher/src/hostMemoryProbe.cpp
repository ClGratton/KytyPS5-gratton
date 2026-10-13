#include "hostMemoryProbe.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#if !defined(_WIN32)
#include <sys/statvfs.h>
#endif

namespace HostMemory {

namespace {

namespace fs = std::filesystem;

std::string ReadText(const fs::path& path) {
	std::ifstream file(path);
	if (!file) {
		return {};
	}
	std::ostringstream out;
	out << file.rdbuf();
	return out.str();
}

std::string FirstLine(const fs::path& path) {
	std::ifstream file(path);
	std::string   line;
	if (file) {
		std::getline(file, line);
	}
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())) != 0) {
		line.pop_back();
	}
	return line;
}

bool Exists(const fs::path& path) {
	std::error_code error;
	return fs::exists(path, error);
}

uint64_t StatvfsFreeMiB(const std::string& real_path) {
#if defined(_WIN32)
	(void)real_path;
	return 0;
#else
	struct statvfs info {};
	if (statvfs(real_path.c_str(), &info) != 0) {
		return 0;
	}
	// f_bavail: what an unprivileged writer can use; root may use a little more.
	return static_cast<uint64_t>(info.f_bavail) * info.f_frsize / (1024 * 1024);
#endif
}

// "1" means a spinning disk. `queue` is the block device's sysfs directory, or its parent for a partition.
bool IsRotational(const fs::path& block_dir) {
	for (const char* rel: {"queue/rotational", "../queue/rotational"}) {
		const fs::path file = block_dir / rel;
		if (Exists(file)) {
			return FirstLine(file) == "1";
		}
	}
	return false;
}

std::string BaseName(const fs::path& path) {
	std::error_code error;
	const auto      resolved = fs::canonical(path, error);
	return (error ? path : resolved).filename().string();
}

bool EmulatorRunning(const fs::path& proc) {
	std::error_code error;
	for (fs::directory_iterator it(proc, error), end; !error && it != end; it.increment(error)) {
		const std::string name = it->path().filename().string();
		if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) == 0) {
			continue;
		}
		std::string cmdline = ReadText(it->path() / "cmdline");
		const auto  nul     = cmdline.find('\0');
		if (nul != std::string::npos) {
			cmdline.resize(nul);
		}
		const auto slash = cmdline.rfind('/');
		const auto base  = slash == std::string::npos ? cmdline : cmdline.substr(slash + 1);
		if (base.rfind("kyty_emulator", 0) == 0) {
			return true;
		}
	}
	return false;
}

// Config files of the given directories as systemd reads them: a file name in a later directory replaces the
// same name in an earlier one, and the result is ordered by file name.
std::vector<std::string> DropinTexts(const fs::path& root, const std::vector<std::string>& dirs, const char* suffix) {
	std::map<std::string, std::string> by_name;
	for (const auto& dir: dirs) {
		std::error_code error;
		for (fs::directory_iterator it(fs::path(root.string() + dir), error), end; !error && it != end; it.increment(error)) {
			const std::string name = it->path().filename().string();
			if (name.size() > std::string(suffix).size() && name.compare(name.size() - std::string(suffix).size(), std::string::npos, suffix) == 0) {
				by_name[name] = ReadText(it->path());
			}
		}
	}
	std::vector<std::string> texts;
	for (const auto& entry: by_name) {
		texts.push_back(entry.second);
	}
	return texts;
}

bool ProgramExists(const fs::path& root, const char* name) {
	if (root.empty()) {
		const char* path = std::getenv("PATH");
		std::string list = path != nullptr ? path : "/usr/bin:/bin";
		std::istringstream stream(list);
		std::string        dir;
		while (std::getline(stream, dir, ':')) {
			if (!dir.empty() && Exists(fs::path(dir) / name)) {
				return true;
			}
		}
		return false;
	}
	return Exists(root / "usr/bin" / name) || Exists(root / "bin" / name);
}

} // namespace

LinuxFacts ProbeLinux(const ProbeEnv& env) {
	const fs::path root(env.root);
	const auto     at = [&root](const std::string& absolute) { return fs::path(root.string() + absolute); };

	LinuxFacts facts;

	const auto mem    = ParseMeminfo(ReadText(at("/proc/meminfo")));
	facts.ram_mib           = mem.total_kib / 1024;
	facts.mem_available_mib = mem.available_kib / 1024;

	const fs::path zswap = at("/sys/module/zswap");
	facts.zswap_supported = Exists(zswap);
	if (facts.zswap_supported) {
		const auto param = [&zswap](const char* name) { return FirstLine(zswap / "parameters" / name); };
		const auto enabled = param("enabled");
		facts.zswap_enabled       = enabled == "Y" || enabled == "y" || enabled == "1";
		facts.zswap_compressor    = param("compressor");
		facts.zswap_pool_percent  = static_cast<uint32_t>(std::atoi(param("max_pool_percent").c_str()));
		const auto shrinker       = param("shrinker_enabled");
		facts.zswap_shrinker      = shrinker == "Y" || shrinker == "y" || shrinker == "1";
	}

	facts.swaps = ParseProcSwaps(ReadText(at("/proc/swaps")));
	const std::string mountinfo = ReadText(at("/proc/self/mountinfo"));
	for (auto& swap: facts.swaps) {
		if (swap.kind == SwapKind::Zram) {
			const std::string device = swap.name.substr(swap.name.rfind('/') + 1);
			const auto        stat   = ReadText(at("/sys/block/" + device + "/mm_stat"));
			std::istringstream stream(stat);
			uint64_t           orig_bytes = 0;
			stream >> orig_bytes;
			facts.zram_orig_mib += orig_bytes / (1024 * 1024);
		} else if (swap.kind == SwapKind::Partition) {
			swap.rotational = IsRotational(at("/sys/class/block/" + BaseName(at(swap.name))));
		} else if (swap.kind == SwapKind::File) {
			const auto mount = FindMount(mountinfo, swap.name);
			if (!mount.device.empty()) {
				swap.rotational = IsRotational(at("/sys/dev/block/" + mount.device));
			}
		}
	}

	facts.has_systemd    = Exists(at("/run/systemd/system"));
	facts.has_pkexec     = ProgramExists(root, "pkexec");
	facts.zram_generator = Exists(at("/usr/lib/systemd/system-generators/zram-generator")) ||
	                       Exists(at("/lib/systemd/system-generators/zram-generator"));
	facts.emulator_running = EmulatorRunning(at("/proc"));
	facts.applied_unit     = Exists(at(kUnitPath));
	facts.applied_dropin   = Exists(at(kZramDropinPath));

	// Does zswap come back after a reboot? Later setters win: Kyty's unit and tmpfiles.d run after the kernel
	// command line (the boot-time unit is ordered after systemd-tmpfiles-setup).
	{
		auto setting = ParseCmdlineZswapEnabled(ReadText(at("/proc/cmdline")));
		const auto tmpfiles = ParseTmpfilesZswapEnabled(
		    DropinTexts(root, {"/usr/lib/tmpfiles.d", "/run/tmpfiles.d", "/etc/tmpfiles.d"}, ".conf"));
		if (tmpfiles != Tristate::Unknown) {
			setting = tmpfiles;
		}
		facts.zswap_persistent = facts.applied_unit || setting == Tristate::Yes;
	}
	// zram devices zram-generator creates at the next boot (the main file has the lowest precedence).
	if (facts.zram_generator) {
		std::vector<std::string> texts;
		for (const char* main_file: {"/usr/lib/systemd/zram-generator.conf", "/etc/systemd/zram-generator.conf"}) {
			if (Exists(at(main_file))) {
				texts.push_back(ReadText(at(main_file)));  // /etc replaces /usr/lib: the later push is last
			}
		}
		const auto dropins = DropinTexts(root, {"/usr/lib/systemd/zram-generator.conf.d", "/usr/local/lib/systemd/zram-generator.conf.d",
		                                       "/etc/systemd/zram-generator.conf.d", "/run/systemd/zram-generator.conf.d"}, ".conf");
		texts.insert(texts.end(), dropins.begin(), dropins.end());
		facts.zram_at_boot = ZramDevicesAtBoot(texts);
	}

	// The swapfile goes into kSwapDir; look at the nearest directory that exists now.
	fs::path probe_path = at(kSwapDir);
	std::string logical = kSwapDir;
	while (!Exists(probe_path) && logical.size() > 1) {
		logical.resize(logical.rfind('/') == 0 ? 1 : logical.rfind('/'));
		probe_path = at(logical);
	}
	facts.swap_dir_fstype = FindMount(mountinfo, logical).fstype;
	facts.swap_dir_free_mib =
	    env.free_mib ? env.free_mib(probe_path.string()) : StatvfsFreeMiB(probe_path.string());
	return facts;
}

} // namespace HostMemory
