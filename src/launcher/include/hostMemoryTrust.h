#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_TRUST_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_TRUST_H_

// The launcher runs its root helper through pkexec. A helper that a normal user can modify (or that sits
// in a directory a normal user can modify) would let any program running as that user run code as root as
// soon as the user types the password, so the helper is installed root-owned in a root-only place and
// checked before every pkexec. These functions are pure: the file system is passed in, so the tests run
// them on a made-up tree. Linux only; nothing here is used on Windows.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HostMemory {

// Where the helper is installed: the first of these whose parent directories are all root-only. /usr/local is
// the usual place (it also works on image-based systems); some systems make /usr/local group-writable
// (Debian: root:staff, mode 2775), and then /usr/libexec is used.
const std::vector<std::string>& HelperInstallPaths();
// The polkit action file that names the helper path; installed best effort next to the helper.
constexpr char kPolkitActionFile[] = "/usr/share/polkit-1/actions/org.kyty.host-memory.policy";

struct FileStat {
	bool     exists     = false;
	bool     is_symlink = false;
	bool     is_dir     = false;
	bool     is_regular = false;
	uint32_t uid        = 0;
	uint32_t mode       = 0;  // permission bits (st_mode & 07777)
};

using StatFn    = std::function<FileStat(const std::string& path)>;     // lstat
using ResolveFn = std::function<std::string(const std::string& path)>;  // realpath; empty when it does not exist

struct Trust {
	bool        ok = false;
	std::string reason;  // why not (empty when ok)
};

// A directory every component of whose real path is owned by root and not writable by group or others.
Trust CheckRootOnlyDirectory(const std::string& path, const StatFn& lstat, const ResolveFn& resolve);

// The helper: a regular, executable file, owned by root, not writable by group or others, whose real path
// has only such directories above it.
Trust CheckRootOwnedExecutable(const std::string& path, const StatFn& lstat, const ResolveFn& resolve);

// The nearest existing directory of `path` (it or an ancestor), through `lstat`.
std::string NearestExistingDirectory(const std::string& path, const StatFn& lstat);

// lstat and realpath on the running system (POSIX only; empty/not-existing on Windows).
FileStat    RealLstat(const std::string& path);
std::string RealResolve(const std::string& path);

// The text of the one-time install step that pkexec runs as `/bin/sh -c <text> kyty-install <target> <sha256>
// <base64 of the helper> [-- helper arguments]`.
const std::string& InstallScriptText();

} // namespace HostMemory

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_TRUST_H_ */
