#include "graphics/presentation/framePacer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using Libs::Graphics::FramePacer;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FramePacerTests: failed: %s\n", message);
		std::abort();
	}
}

// Presented flips over `vblanks` vblanks for a game that always has a flip ready; `pattern`
// receives one character per vblank ('x' presented, '.' skipped).
uint32_t Run(FramePacer& pacer, uint32_t vblanks, std::string* pattern = nullptr) {
	uint32_t presented = 0;
	for (uint32_t i = 0; i < vblanks; i++) {
		const bool allow = pacer.OnVblank();
		if (allow) {
			pacer.OnPresented();
			presented++;
		}
		if (pattern != nullptr) {
			pattern->push_back(allow ? 'x' : '.');
		}
	}
	return presented;
}

void TestDisabled() {
	FramePacer off;
	off.Configure(0, 60);
	Check(!off.Active(), "limit 0 is off");
	Check(Run(off, 600) == 600, "off presents every vblank");
	FramePacer at_rate;
	at_rate.Configure(60, 60);
	Check(!at_rate.Active(), "a limit equal to the vblank rate is off");
	FramePacer above;
	above.Configure(144, 60);
	Check(!above.Active() && Run(above, 60) == 60, "a limit above the vblank rate cannot raise it");
}

void TestHalfRate() {
	FramePacer p;
	p.Configure(30, 60);
	std::string pattern;
	Check(Run(p, 600, &pattern) == 300, "30 on 60 presents every other vblank");
	Check(pattern.substr(0, 6) == ".x.x.x", "half rate alternates");
}

void TestEvenSpread() {
	FramePacer p;
	p.Configure(45, 60);
	std::string pattern;
	Check(Run(p, 600, &pattern) == 450, "45 on 60 presents 3 of 4");
	// No two skipped vblanks in a row, and never more than three presented in a row.
	Check(pattern.find("..") == std::string::npos, "skips are never adjacent");
	Check(pattern.find("xxxx") == std::string::npos, "presented runs are at most 3");
}

void TestOtherRates() {
	for (const uint32_t limit: {20u, 24u, 25u, 30u, 40u, 48u, 50u, 59u}) {
		FramePacer p;
		p.Configure(limit, 60);
		const auto presented = Run(p, 6000);
		const auto expected  = limit * 100u;
		Check(presented + 1 >= expected && presented <= expected + 1, "long-run rate matches the limit");
	}
	FramePacer p;
	p.Configure(60, 120);
	Check(Run(p, 1200) == 600, "60 on a 120 Hz vblank presents every other vblank");
	FramePacer q;
	q.Configure(90, 360);
	Check(Run(q, 3600) == 900, "90 on 360 Hz");
}

void TestIdleDoesNotBurst() {
	FramePacer p;
	p.Configure(30, 60);
	Check(Run(p, 10) == 5, "warm-up");
	// The game has nothing to present for a long time: vblanks pass without OnPresented.
	for (int i = 0; i < 1000; i++) {
		(void)p.OnVblank();
	}
	// Presenting resumes at the limit; the stored credit allows at most two flips in a row at 30 on 60.
	std::string pattern;
	Run(p, 8, &pattern);
	Check(pattern.find("xxx") == std::string::npos, "no burst after an idle stretch");
	Check(Run(p, 600) == 300, "rate recovers");
}

void TestReconfigure() {
	FramePacer p;
	p.Configure(30, 60);
	Check(Run(p, 60) == 30, "limited");
	p.Configure(0, 60);
	Check(Run(p, 60) == 60, "limit removed");
	p.Configure(15, 60);
	Check(Run(p, 60) == 15, "limit applied again");
}

} // namespace

int main() {
	TestDisabled();
	TestHalfRate();
	TestEvenSpread();
	TestOtherRates();
	TestIdleDoesNotBurst();
	TestReconfigure();
	std::puts("FramePacerTests: all passed");
	return 0;
}
