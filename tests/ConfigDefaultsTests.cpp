// Defaults of the emulator options (docs/defaults.md). A default is part of the emulator's
// behaviour: these tests pin the values the emulator starts with when no flag is given, and that
// the opt-out flags still reach the config.

#include "common/emulatorConfig.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ConfigDefaultsTests: failed: %s\n", message);
		std::abort();
	}
}

// Loads `options` as main() does after parsing the command line.
void Apply(const Config::ConfigOptions& options) {
	Config::Load(options);
}

// Correctness: the guest reads GPU timestamps from the host GPU's execution, not from the time the
// command processor parsed the packet (an accurate emulation; Legacy is the opt-out).
void TestGpuTimestampsDefault() {
	const Config::ConfigOptions options;
	Check(options.gpu_timestamps == Config::GpuTimestampMode::Gpu, "gpu_timestamps defaults to gpu");
	Apply(options);
	Check(Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Gpu,
	      "an unconfigured emulator reads real GPU timestamps");

	Config::ConfigOptions legacy;
	legacy.gpu_timestamps = Config::GpuTimestampMode::Legacy;
	Apply(legacy);
	Check(Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Legacy,
	      "--gpu-timestamps legacy still selects the parse-time clock");
	Check(Config::GetGpuTimeScale() == 1.0, "the time scale stays off by default");
}

} // namespace

int main() {
	Config::Initialize();

	TestGpuTimestampsDefault();

	Config::Shutdown();
	std::printf("ConfigDefaultsTests: ok\n");
	return 0;
}
