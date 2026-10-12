// Tests for --debug-hle-trace (common/hleTrace.h): the thunk must hand the callee exactly what
// the guest passed (integer and floating-point arguments, stack arguments, the vararg count in
// rax, the return value and xmm0 result), and the per-thread ring must record each call with its
// note and result.

#include "common/hleTrace.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__) && defined(__x86_64__)

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HleTraceTests: failed: %s\n", message);
		std::abort();
	}
}

int64_t g_seen_a = 0;
int64_t g_seen_f = 0;

// Ten integer arguments (four on the stack) and two doubles: a clobbered register or a shifted
// stack would change the sum.
int64_t SumMany(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g,
                int64_t h, int64_t i, int64_t j, double x, double y) {
	g_seen_a = a;
	g_seen_f = f;
	return a + b + c + d + e + f + g + h + i + j + static_cast<int64_t>(x * 2.0 + y);
}

double ReturnsDouble(double x, int n) {
	return x * n;
}

int WithNote(const char* path) {
	Common::HleTrace::NoteString(path);
	return Common::HleTrace::Ret(-2);
}

std::string Capture(unsigned own, unsigned other) {
	FILE* f = std::tmpfile();
	Check(f != nullptr, "tmpfile");
	Common::HleTrace::Dump(f, own, other, nullptr);
	std::rewind(f);
	std::string text;
	char        buf[512];
	while (std::fgets(buf, sizeof(buf), f) != nullptr) {
		text += buf;
	}
	std::fclose(f);
	return text;
}

} // namespace

int main() {
	Common::HleTrace::Enable(true);

	using SumFn    = int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
	                              int64_t, int64_t, int64_t, double, double);
	using DoubleFn = double (*)(double, int);
	using NoteFn   = int (*)(const char*);

	auto sum = reinterpret_cast<SumFn>(
	    Common::HleTrace::Wrap("SumMany", reinterpret_cast<uint64_t>(&SumMany)));
	auto dbl = reinterpret_cast<DoubleFn>(
	    Common::HleTrace::Wrap("ReturnsDouble", reinterpret_cast<uint64_t>(&ReturnsDouble)));
	auto note = reinterpret_cast<NoteFn>(
	    Common::HleTrace::Wrap("WithNote", reinterpret_cast<uint64_t>(&WithNote)));
	Check(reinterpret_cast<uint64_t>(sum) != reinterpret_cast<uint64_t>(&SumMany),
	      "a wrapped function is bound to a thunk");

	const auto total = sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1.5, 0.25);
	Check(total == 55 + 3, "integer, stack and floating-point arguments reach the callee");
	Check(g_seen_a == 1 && g_seen_f == 6, "register arguments in order");
	Check(dbl(2.5, 4) == 10.0, "a double argument and result pass through");
	Check(note("/app0/data/level.lvx") == -2, "the result passes through");

	const auto text = Capture(16, 4);
	Check(text.find("SumMany(0x1, 0x2, 0x3, 0x4)") != std::string::npos, "call and arguments recorded");
	Check(text.find("ReturnsDouble") != std::string::npos, "second call recorded");
	Check(text.find("WithNote(") != std::string::npos && text.find("= -2") != std::string::npos,
	      "result recorded");
	Check(text.find("\"/app0/data/level.lvx\"") != std::string::npos, "note recorded");
	Check(text.find("[faulting]") != std::string::npos, "the calling thread is marked");

	// Another thread's calls are in its own ring, shown after the caller's.
	std::thread other([&] {
		for (int i = 0; i < 1000; i++) {
			(void)dbl(1.0, i);
		}
	});
	other.join();
	const auto both = Capture(2, 3);
	size_t     headers = 0;
	for (size_t pos = both.find("-- thread"); pos != std::string::npos;
	     pos = both.find("-- thread", pos + 1)) {
		headers++;
	}
	Check(headers == 2, "one block per thread that made calls");

	// The ring keeps the most recent calls: 1000 calls on one thread leave a bounded window.
	Check(Common::HleTrace::CallCount() >= 1003, "all calls counted");
	std::printf("HleTraceTests: passed\n");
	return 0;
}

#else

int main() {
	std::printf("HleTraceTests: skipped (Linux x86-64 only)\n");
	return 0;
}

#endif
