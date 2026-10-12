// Regression test for SysFileRead into write-protected guest memory (Linux).
//
// The GPU write tracker protects guest pages; read() into such a page fails with EFAULT after the
// bytes before it (no fault handler runs for a kernel copy), and a plain fread reported that short
// count as success. The caller (a level or model loader) then used truncated data. SysFileRead must
// deliver the whole file: the rest goes through a host buffer and user-mode stores, which the
// emulator's fault handler resolves. The test stands in for the tracker with a SIGSEGV handler that
// unprotects the faulting page.

#include "common/platform/sysFileIO.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SysFileReadTests: failed: %s\n", message);
		std::abort();
	}
}

uint8_t*    g_region      = nullptr;
size_t      g_region_size = 0;
long        g_page        = 0;
int         g_faults      = 0;

void FaultHandler(int, siginfo_t* info, void*) {
	auto* address = static_cast<uint8_t*>(info->si_addr);
	if (address < g_region || address >= g_region + g_region_size) {
		std::signal(SIGSEGV, SIG_DFL);
		return; // a real crash: re-fault with the default action
	}
	auto* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(address) &
	                                         ~static_cast<uintptr_t>(g_page - 1));
	mprotect(page, static_cast<size_t>(g_page), PROT_READ | PROT_WRITE);
	g_faults++;
}

} // namespace

int main() {
	g_page = sysconf(_SC_PAGESIZE);
	const size_t pages = 6;
	g_region_size      = pages * static_cast<size_t>(g_page);
	g_region = static_cast<uint8_t*>(mmap(nullptr, g_region_size, PROT_READ | PROT_WRITE,
	                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	Check(g_region != MAP_FAILED, "mmap");

	// A file slightly smaller than the region, with a pattern that identifies every byte.
	const size_t file_size = g_region_size - 100;
	std::vector<uint8_t> content(file_size);
	for (size_t i = 0; i < file_size; i++) {
		content[i] = static_cast<uint8_t>((i * 131u + i / 7u) & 0xffu);
	}
	const auto path = std::filesystem::temp_directory_path() / "kyty_sys_file_read_test.bin";
	{
		FILE* out = std::fopen(path.c_str(), "wb");
		Check(out != nullptr, "create test file");
		Check(std::fwrite(content.data(), 1, content.size(), out) == content.size(), "write test file");
		std::fclose(out);
	}

	// Fault injection (KYTY_SYSFILE_CHAOS_EFAULT=1000: every read of more than two pages stops at the
	// next page boundary of the destination, as a kernel copy into a protected page does). Plain
	// memory, no protection: the fallback must complete the read, and without it the read is short.
	if (const char* chaos = std::getenv("KYTY_SYSFILE_CHAOS_EFAULT"); chaos != nullptr) {
		const bool fallback_off = std::getenv("KYTY_SYSFILE_NO_READ_FALLBACK") != nullptr;
		for (size_t offset: {size_t {0}, size_t {100}, size_t {4095}}) {
			std::memset(g_region, 0, g_region_size);
			auto* f = SysFileOpenR(path);
			Check(f != nullptr, "open chaos");
			uint32_t got = 0;
			SysFileRead(g_region + offset, static_cast<uint32_t>(file_size - 4096), *f, &got);
			SysFileClose(f);
			if (fallback_off) {
				const size_t boundary = static_cast<size_t>(g_page) - offset % static_cast<size_t>(g_page);
				Check(got == boundary, "without the fallback the read stops at the page boundary");
			} else {
				Check(got == file_size - 4096, "the fallback completes an injected short read");
				Check(std::memcmp(g_region + offset, content.data(), got) == 0,
				      "the completed read delivers the right bytes");
			}
		}
		Check(SysFileReadFallbackCount() == 3, "every injected failure was counted");
		std::filesystem::remove(path);
		std::printf("SysFileReadTests: fault-injection case passed (%s)\n", chaos);
		return 0;
	}

	struct sigaction action {};
	action.sa_sigaction = FaultHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	Check(sigaction(SIGSEGV, &action, nullptr) == 0, "sigaction");

	// Control: the setup really reproduces the bug. A plain fread into protected pages is short
	// (the kernel copy stops at the first protected page; no signal is raised).
	{
		std::memset(g_region, 0, g_region_size);
		mprotect(g_region + 2 * g_page, static_cast<size_t>(g_page), PROT_READ);
		FILE* in = std::fopen(path.c_str(), "rb");
		Check(in != nullptr, "open control");
		const size_t got = std::fread(g_region, 1, file_size, in);
		std::fclose(in);
		Check(got < file_size, "control: plain fread into a write-protected page is short");
		mprotect(g_region, g_region_size, PROT_READ | PROT_WRITE);
		g_faults = 0;
	}

	// Case 1: nothing protected, the plain path.
	{
		std::memset(g_region, 0, g_region_size);
		auto* f = SysFileOpenR(path);
		Check(f != nullptr, "open");
		uint32_t got = 0;
		SysFileRead(g_region, static_cast<uint32_t>(file_size), *f, &got);
		SysFileClose(f);
		Check(got == file_size && std::memcmp(g_region, content.data(), file_size) == 0,
		      "plain read");
		Check(SysFileReadFallbackCount() == 0, "no fallback for an ordinary read");
	}

	// Case 2: pages 2 and 4 are write-protected (tracked): the read must still be complete.
	{
		std::memset(g_region, 0, g_region_size);
		mprotect(g_region + 2 * g_page, static_cast<size_t>(g_page), PROT_READ);
		mprotect(g_region + 4 * g_page, static_cast<size_t>(g_page), PROT_READ);
		auto* f = SysFileOpenR(path);
		Check(f != nullptr, "open 2");
		uint32_t got = 0;
		SysFileRead(g_region, static_cast<uint32_t>(file_size), *f, &got);
		SysFileClose(f);
		if (std::getenv("KYTY_SYSFILE_NO_READ_FALLBACK") != nullptr) {
			// Debug switch: the pre-fallback behaviour (short count) for A/B runs of a crash.
			Check(got < file_size, "with the fallback disabled the read is short");
			Check(SysFileReadFallbackCount() == 1, "the short read was counted");
			std::filesystem::remove(path);
			std::printf("SysFileReadTests: fallback-disabled case passed\n");
			return 0;
		}
		Check(got == file_size, "read into protected pages returns every byte");
		Check(std::memcmp(g_region, content.data(), file_size) == 0,
		      "read into protected pages delivers the right bytes");
		Check(g_faults >= 1, "the protected pages were written through the fault handler");
		Check(SysFileReadFallbackCount() >= 1, "the fallback was used");
	}

	std::filesystem::remove(path);
	std::printf("SysFileReadTests: all cases passed\n");
	return 0;
}
#else
int main() {
	std::printf("SysFileReadTests: Linux only, skipped\n");
	return 0;
}
#endif
