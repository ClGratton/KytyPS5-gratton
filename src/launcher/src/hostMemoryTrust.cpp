#include "hostMemoryTrust.h"

#include <cstdio>
#include <cstdlib>

#if !defined(_WIN32)
#include <climits>
#include <sys/stat.h>
#endif

namespace HostMemory {

const std::vector<std::string>& HelperInstallPaths() {
	static const std::vector<std::string> paths = {"/usr/local/libexec/kyty/kyty-host-memory",
	                                               "/usr/libexec/kyty/kyty-host-memory"};
	return paths;
}

namespace {

std::string Octal(uint32_t mode) {
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%o", static_cast<unsigned>(mode));
	return buffer;
}

// "/a/b/c" -> "/a/b"; "/a" -> "/"; "/" -> "".
std::string Parent(const std::string& path) {
	if (path.size() <= 1) {
		return {};
	}
	const auto slash = path.rfind('/');
	return slash == 0 ? "/" : path.substr(0, slash);
}

bool Normal(const std::string& path) {
	if (path.empty() || path[0] != '/') {
		return false;
	}
	if (path == "/") {
		return true;
	}
	if (path.back() == '/') {
		return false;
	}
	size_t start = 1;
	while (start <= path.size()) {
		const auto next      = path.find('/', start);
		const auto component = path.substr(start, next == std::string::npos ? std::string::npos : next - start);
		if (component.empty() || component == "." || component == "..") {
			return false;
		}
		if (next == std::string::npos) {
			break;
		}
		start = next + 1;
	}
	return true;
}

Trust Fail(const std::string& what, const std::string& path) {
	return {false, what + ": " + path};
}

// Every directory from `resolved` up to "/" (resolved has no symlinks).
Trust CheckChain(const std::string& resolved, const StatFn& lstat) {
	for (std::string dir = resolved; !dir.empty(); dir = Parent(dir)) {
		const FileStat st = lstat(dir);
		if (!st.exists) {
			return Fail("does not exist", dir);
		}
		if (st.is_symlink || !st.is_dir) {
			return Fail("not a plain directory", dir);
		}
		if (st.uid != 0) {
			return Fail("not owned by root", dir);
		}
		if ((st.mode & 0022) != 0) {
			return Fail("writable by group or others (mode " + Octal(st.mode) + ")", dir);
		}
	}
	return {true, {}};
}

} // namespace

Trust CheckRootOnlyDirectory(const std::string& path, const StatFn& lstat, const ResolveFn& resolve) {
	if (!Normal(path)) {
		return Fail("not a normal absolute path", path);
	}
	const std::string real = resolve(path);
	if (real.empty()) {
		return Fail("does not exist", path);
	}
	return CheckChain(real, lstat);
}

Trust CheckRootOwnedExecutable(const std::string& path, const StatFn& lstat, const ResolveFn& resolve) {
	if (!Normal(path)) {
		return Fail("not a normal absolute path", path);
	}
	const std::string real = resolve(path);
	if (real.empty()) {
		return Fail("does not exist", path);
	}
	const FileStat file = lstat(real);
	if (!file.exists || file.is_symlink || !file.is_regular) {
		return Fail("not a regular file", real);
	}
	if (file.uid != 0) {
		return Fail("not owned by root", real);
	}
	if ((file.mode & 0022) != 0) {
		return Fail("writable by group or others (mode " + Octal(file.mode) + ")", real);
	}
	if ((file.mode & 0100) == 0) {
		return Fail("not executable", real);
	}
	return CheckChain(Parent(real), lstat);
}

std::string NearestExistingDirectory(const std::string& path, const StatFn& lstat) {
	for (std::string dir = path; !dir.empty(); dir = Parent(dir)) {
		const FileStat st = lstat(dir);
		if (st.exists && st.is_dir) {
			return dir;
		}
	}
	return "/";
}

FileStat RealLstat(const std::string& path) {
	FileStat out;
#if !defined(_WIN32)
	struct stat st {};
	if (lstat(path.c_str(), &st) == 0) {
		out.exists     = true;
		out.is_symlink = S_ISLNK(st.st_mode);
		out.is_dir     = S_ISDIR(st.st_mode);
		out.is_regular = S_ISREG(st.st_mode);
		out.uid        = static_cast<uint32_t>(st.st_uid);
		out.mode       = static_cast<uint32_t>(st.st_mode & 07777);
	}
#else
	(void)path;
#endif
	return out;
}

std::string RealResolve(const std::string& path) {
#if !defined(_WIN32)
	char buffer[PATH_MAX];
	if (realpath(path.c_str(), buffer) != nullptr) {
		return buffer;
	}
#else
	(void)path;
#endif
	return {};
}

} // namespace HostMemory
