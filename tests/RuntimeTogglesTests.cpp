// Runtime toggles (common/runtimeToggles.h): the registry logic behind --toggle-control.

#include "common/runtimeToggles.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using namespace Common::RuntimeToggles;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RuntimeTogglesTests: failed: %s\n", message);
		std::abort();
	}
}

void TestParseBool() {
	bool v = false;
	Check(ParseBool("true", v) && v, "true");
	Check(ParseBool("ON", v) && v, "ON is case-insensitive");
	Check(ParseBool("1", v) && v, "1");
	Check(ParseBool("off", v) && !v, "off");
	Check(ParseBool("No", v) && !v, "No");
	Check(!ParseBool("maybe", v), "garbage is refused");
	Check(!ParseBool("", v), "empty is refused");
}

void TestSetAndUnknown() {
	bool     a = false;
	Registry r;
	r.Register("--feature-a", &a, false);
	r.Arm();
	bool old = true;
	Check(r.Set("feature-a", true, &old) == Result::Applied, "an off flag turns on");
	Check(a && !old, "the field changed, old value reported");
	Check(r.Set("--feature-a", true) == Result::Unchanged, "setting the same value changes nothing");
	Check(r.Set("feature-a", false) == Result::Applied, "and turns off again");
	Check(!a, "off");
	Check(r.Set("nope", true) == Result::UnknownFlag, "an unregistered flag is refused");
	Check(r.Has("feature-a") && r.Has("--feature-a") && !r.Has("nope"), "Has strips dashes");
}

void TestArmedRule() {
	bool     on_at_start = true, off_at_start = false;
	Registry r;
	r.Register("needs-setup-on", &on_at_start, true);
	r.Register("needs-setup-off", &off_at_start, true);
	r.Arm();
	Check(r.Set("needs-setup-on", false) == Result::Applied, "an armed flag turns off");
	Check(r.Set("needs-setup-on", true) == Result::Applied, "and on again");
	Check(on_at_start, "back on");
	Check(r.Set("needs-setup-off", true) == Result::NotArmed, "a flag off at start-up cannot be turned on");
	Check(!off_at_start, "and stays off");
	Check(r.Set("needs-setup-off", false) == Result::Unchanged, "turning it off is a no-op");
}

void TestApplyText() {
	bool     a = true, b = false, c = true;
	Registry r;
	r.Register("a", &a, false);
	r.Register("b", &b, false);
	r.Register("c", &c, true);
	r.Arm();
	const auto changes = r.ApplyText("# seq 7\n"
	                                 "a=false\n"
	                                 "--b on\n"
	                                 "  c = off  \n"
	                                 "unknown=true\n"
	                                 "a=perhaps\n"
	                                 "justaword\n"
	                                 "\n");
	Check(changes.size() == 5, "five lines name a flag");
	Check(changes[0].name == "a" && changes[0].result == Result::Applied && changes[0].old_value &&
	          !changes[0].new_value, "a=false applied, old true");
	Check(changes[1].name == "b" && changes[1].result == Result::Applied && changes[1].new_value,
	      "--b on applied");
	Check(changes[2].name == "c" && changes[2].result == Result::Applied, "spaces around = are fine");
	Check(changes[3].result == Result::UnknownFlag, "unknown flag reported");
	Check(changes[4].result == Result::BadValue, "bad value reported");
	Check(!a && b && !c, "fields");
	const auto again = r.ApplyText("a=false\n");
	Check(again.size() == 1 && again[0].result == Result::Unchanged, "idempotent");
}

// Readers on other threads see the change through Read() without a data race (run under tsan to
// prove it; here it checks that a flip is eventually seen and both values are only true/false).
void TestConcurrentRead() {
	bool     flag = false;
	Registry r;
	r.Register("f", &flag, false);
	r.Arm();
	std::atomic<bool> stop {false};
	std::atomic<int>  seen_true {0};
	std::thread       reader([&] {
        while (!stop.load()) {
            if (Read(flag)) seen_true.fetch_add(1);
        }
    });
	for (int i = 0; i < 1000; i++) {
		(void)r.Set("f", (i & 1) == 0);
		std::this_thread::yield();
	}
	(void)r.Set("f", true);
	for (int i = 0; i < 1000000 && seen_true.load() == 0; i++) std::this_thread::yield();
	stop = true;
	reader.join();
	Check(seen_true.load() > 0, "the reader saw the flag turn on");
}

} // namespace

int main() {
	TestParseBool();
	TestSetAndUnknown();
	TestArmedRule();
	TestApplyText();
	TestConcurrentRead();
	std::puts("RuntimeTogglesTests: ok");
	return 0;
}
