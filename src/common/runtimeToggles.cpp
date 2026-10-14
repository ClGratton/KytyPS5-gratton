#include "common/runtimeToggles.h"

#include "common/benchInstrument.h"
#include "common/emulatorConfig.h"
#include "common/gpuPhaseStats.h"
#include "common/logging/log.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace Common::RuntimeToggles {

namespace {

struct State {
	Registry                registry;
	std::filesystem::path   control;
	std::filesystem::path   applied_csv;
	std::mutex              mutex; // guards `text` and the watcher bookkeeping
	std::string             text;  // the content the watcher saw last and the presenter has not applied
	std::atomic<bool>       stop {false};
	std::thread             watcher;
};

std::unique_ptr<State> g_state;

bool ReadFile(const std::filesystem::path& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

void WatchLoop(State* s, std::string last_seen, bool have_seen) {
	while (!s->stop.load(std::memory_order_relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		std::string now;
		if (!ReadFile(s->control, now)) continue;
		if (have_seen && now == last_seen) continue;
		have_seen = true;
		last_seen = now;
		{
			std::lock_guard lock(s->mutex);
			s->text = std::move(now);
		}
		detail::g_pending.store(true, std::memory_order_release);
	}
}

} // namespace

void detail::ApplyPending(uint64_t frame_idx) {
	if (!g_pending.exchange(false, std::memory_order_acquire) || !g_state) return;
	std::string text;
	{
		std::lock_guard lock(g_state->mutex);
		text.swap(g_state->text);
	}
	const auto changes = g_state->registry.ApplyText(text);
	Common::GpuPhase::RequestCut();
	const auto t_ns    = Bench::NowNs();
	std::ofstream csv(g_state->applied_csv, std::ios::app);
	for (const auto& c: changes) {
		char line[256];
		std::snprintf(line, sizeof(line), "%lld,%llu,%s,%d,%d,%s\n", static_cast<long long>(t_ns),
		              static_cast<unsigned long long>(frame_idx), c.name.c_str(), c.old_value ? 1 : 0,
		              c.new_value ? 1 : 0, ResultName(c.result));
		if (csv) csv << line;
		char msg[256];
		std::snprintf(msg, sizeof(msg), "[toggle] frame %llu: --%s %s -> %s (%s)\n",
		              static_cast<unsigned long long>(frame_idx), c.name.c_str(),
		              c.old_value ? "on" : "off", c.new_value ? "on" : "off", ResultName(c.result));
		Log::WriteToConsoleAndLog(msg);
	}
}

void Start(const std::string& control_path) {
	Stop();
	if (control_path.empty()) return;
	g_state              = std::make_unique<State>();
	g_state->control     = control_path;
	g_state->applied_csv = control_path + ".applied.csv";
	Config::RegisterRuntimeToggles(g_state->registry);
	g_state->registry.Arm();
	// Whatever file was there before the emulator started is not a request.
	std::string before;
	const bool  existed = ReadFile(g_state->control, before);
	g_state->watcher    = std::thread(WatchLoop, g_state.get(), std::move(before), existed);
	detail::g_active.store(true, std::memory_order_relaxed);
	std::string names;
	for (const auto& n: g_state->registry.Names()) names += (names.empty() ? "" : ", ") + n;
	Log::WriteToConsoleAndLog("[toggle] runtime toggles on, control file " + control_path +
	                          "; flags: " + names + "\n");
}

void Stop() {
	detail::g_active.store(false, std::memory_order_relaxed);
	detail::g_pending.store(false, std::memory_order_relaxed);
	if (!g_state) return;
	g_state->stop.store(true, std::memory_order_relaxed);
	if (g_state->watcher.joinable()) g_state->watcher.join();
	g_state.reset();
}

} // namespace Common::RuntimeToggles
