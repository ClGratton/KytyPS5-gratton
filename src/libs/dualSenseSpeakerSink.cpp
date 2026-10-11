#include "libs/dualSenseSpeakerSink.h"

#include "common/common.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Libs::Controller::DualSenseSpeakerSink {

namespace {

constexpr const char* SINK_NAME = "kyty_dualsense_speaker";
constexpr const char* SINK_ARGS = "sink_properties=\"device.description='DualSense Speaker (Kyty)' "
								  "device.icon_name=input-gaming\"";
constexpr float MAX_GAIN		= 4.0f;

std::mutex g_mutex;
std::thread g_thread;
bool g_started = false;
int g_wake[2]  = {-1, -1};
std::atomic<float> g_gain{1.0f};
std::atomic<bool> g_active{false};

struct Child {
	pid_t pid = -1;
	int out	  = -1; // Read end of stdout.
};

// Runs a command without a shell (no fork of this large process: posix_spawn) with the C locale
// so that pactl output is parseable. Closes every other descriptor in the child.
bool Spawn(const std::vector<std::string>& args, Child& child) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return false;
	}
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
	posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t none, all;
	sigemptyset(&none);
	sigemptyset(&all);
	for (int sig = 1; sig < NSIG; sig++) {
		if (sig != SIGKILL && sig != SIGSTOP) {
			sigaddset(&all, sig);
		}
	}
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &all);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	std::vector<std::string> env_storage;
	for (char** e = environ; e != nullptr && *e != nullptr; e++) {
		if (std::strncmp(*e, "LC_", 3) != 0 && std::strncmp(*e, "LANG", 4) != 0) {
			env_storage.emplace_back(*e);
		}
	}
	env_storage.emplace_back("LC_ALL=C");
	std::vector<char*> envp;
	for (auto& e : env_storage) {
		envp.push_back(e.data());
	}
	envp.push_back(nullptr);
	std::vector<char*> argv;
	for (const auto& a : args) {
		argv.push_back(const_cast<char*>(a.c_str()));
	}
	argv.push_back(nullptr);

	pid_t pid	 = -1;
	const int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close(fds[1]);
	if (rc != 0) {
		close(fds[0]);
		return false;
	}
	child.pid = pid;
	child.out = fds[0];
	return true;
}

void Reap(Child& child, bool kill_it) {
	if (child.out >= 0) {
		close(child.out);
		child.out = -1;
	}
	if (child.pid > 0) {
		if (kill_it) {
			kill(child.pid, SIGTERM);
		}
		int status = 0;
		waitpid(child.pid, &status, 0);
		child.pid = -1;
	}
}

// Runs to completion; false when pactl is missing, times out or fails.
bool Run(const std::vector<std::string>& args, std::string& out, int timeout_ms = 3000) {
	out.clear();
	Child child;
	if (!Spawn(args, child)) {
		return false;
	}
	bool ok = true;
	for (;;) {
		pollfd p{child.out, POLLIN, 0};
		const int r = poll(&p, 1, timeout_ms);
		if (r <= 0) {
			ok = false;
			break;
		}
		char buf[4096];
		const auto n = read(child.out, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<size_t>(n));
	}
	int fd	  = child.out;
	child.out = -1;
	close(fd);
	int status = 0;
	if (!ok) {
		kill(child.pid, SIGKILL);
	}
	if (waitpid(child.pid, &status, 0) < 0) {
		return ok; // SIGCHLD ignored by the host: output was complete.
	}
	return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct SinkInfo {
	bool found = false;
	long index = -1;
	float gain = 1.0f;
};

// Mean channel volume of a "front-left: 32768 /  50% / -18.06 dB,   front-right: ..." line as an
// amplitude gain. PulseAudio volumes are cubic in amplitude (pa_sw_volume_to_linear).
float VolumeLineGain(const std::string& line, bool muted) {
	if (muted) {
		return 0.0f;
	}
	double sum = 0;
	int count  = 0;
	for (size_t p = 0; (p = line.find(':', p)) != std::string::npos; p++) {
		char* end		 = nullptr;
		const double raw = std::strtod(line.c_str() + p + 1, &end);
		if (end != line.c_str() + p + 1) {
			sum += raw;
			count++;
		}
	}
	const double v = count > 0 ? sum / count / 65536.0 : 1.0;
	return std::clamp(static_cast<float>(v * v * v), 0.0f, MAX_GAIN);
}

// Parses `pactl list sinks` for our sink: "Sink #N" ... "Name: ..." / "Mute: ..." / "Volume: ...".
SinkInfo ParseSink(const std::string& text) {
	SinkInfo info;
	size_t pos		 = 0;
	bool in_block	 = false;
	long block_index = -1;
	bool muted		 = false;
	std::string volume_line;
	auto finish = [&] {
		if (in_block && info.found) {
			info.gain = VolumeLineGain(volume_line, muted);
		}
	};
	while (pos < text.size()) {
		auto eol = text.find('\n', pos);
		if (eol == std::string::npos) {
			eol = text.size();
		}
		std::string line = text.substr(pos, eol - pos);
		pos				 = eol + 1;
		if (line.rfind("Sink #", 0) == 0) {
			finish();
			in_block	= !info.found;
			block_index = std::strtol(line.c_str() + 6, nullptr, 10);
			muted		= false;
			volume_line.clear();
			continue;
		}
		if (!in_block) {
			continue;
		}
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos) {
			continue;
		}
		line.erase(0, first);
		if (line.rfind("Name: ", 0) == 0) {
			if (line.substr(6) == SINK_NAME) {
				info.found = true;
				info.index = block_index;
			} else {
				in_block = false;
			}
		} else if (line.rfind("Mute: ", 0) == 0) {
			muted = line.substr(6) == "yes";
		} else if (line.rfind("Volume: ", 0) == 0) {
			volume_line = line.substr(8);
		}
	}
	finish();
	return info;
}

void Publish(const SinkInfo& info) {
	const float gain = info.found ? info.gain : 1.0f;
	if (std::fabs(gain - g_gain.load(std::memory_order_relaxed)) > 1e-4f) {
		LOGF("DualSenseSpeakerSink: speaker gain %.3f\n", static_cast<double>(gain));
	}
	g_gain.store(info.found ? info.gain : 1.0f, std::memory_order_relaxed);
	g_active.store(info.found, std::memory_order_relaxed);
}

std::string FindModuleId() {
	std::string out;
	if (!Run({"pactl", "list", "short", "modules"}, out)) {
		return {};
	}
	size_t pos = 0;
	while (pos < out.size()) {
		auto eol = out.find('\n', pos);
		if (eol == std::string::npos) {
			eol = out.size();
		}
		const std::string line = out.substr(pos, eol - pos);
		pos					   = eol + 1;
		if (line.find("module-null-sink") != std::string::npos &&
			line.find(std::string("sink_name=") + SINK_NAME) != std::string::npos) {
			return line.substr(0, line.find('\t'));
		}
	}
	return {};
}

void Worker() {
	std::string out;
	if (!Run({"pactl", "list", "sinks"}, out)) {
		LOGF("DualSenseSpeakerSink: pactl or the sound server is unavailable; the controller "
			 "speaker has no OS volume control\n");
		return;
	}
	auto info = ParseSink(out);
	// A sink left behind by a crashed run is reused (it keeps the user's volume).
	std::string module_id = info.found ? FindModuleId() : std::string{};
	if (!info.found) {
		if (!Run({"pactl", "load-module", "module-null-sink", std::string("sink_name=") + SINK_NAME,
				  SINK_ARGS},
				 out)) {
			LOGF("DualSenseSpeakerSink: cannot create the sink (module-null-sink)\n");
			return;
		}
		module_id = out.substr(0, out.find_first_of("\r\n"));
		// The sound server restores the volume a previous sink of this name had; start at unity,
		// which is the hardware level chosen with --pad-speaker-volume, and unmuted.
		std::string ignored;
		Run({"pactl", "set-sink-volume", SINK_NAME, "100%"}, ignored);
		Run({"pactl", "set-sink-mute", SINK_NAME, "0"}, ignored);
		if (!Run({"pactl", "list", "sinks"}, out) || !(info = ParseSink(out)).found) {
			LOGF("DualSenseSpeakerSink: the new sink did not appear\n");
			if (!module_id.empty()) {
				Run({"pactl", "unload-module", module_id}, out);
			}
			return;
		}
	}
	Publish(info);
	LOGF("DualSenseSpeakerSink: sink '%s' ready (volume gain %.3f); OS volume and mute control "
		 "the controller speaker\n",
		 SINK_NAME, static_cast<double>(info.gain));

	Child sub;
	bool sub_alive = Spawn({"pactl", "subscribe"}, sub);
	std::string pending;
	for (;;) {
		pollfd fds[2] = {{g_wake[0], POLLIN, 0}, {sub_alive ? sub.out : -1, POLLIN, 0}};
		// Without the event stream (pactl subscribe died) fall back to polling twice a second.
		const int r = poll(fds, 2, sub_alive ? -1 : 500);
		if (fds[0].revents != 0) {
			break;
		}
		bool dirty = !sub_alive && r == 0;
		if (sub_alive && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
			char buf[4096];
			const auto n = read(sub.out, buf, sizeof(buf));
			if (n <= 0) {
				Reap(sub, false);
				sub_alive = false;
			} else {
				pending.append(buf, static_cast<size_t>(n));
				size_t nl;
				while ((nl = pending.find('\n')) != std::string::npos) {
					const std::string line = pending.substr(0, nl);
					pending.erase(0, nl + 1);
					// "Event 'change' on sink #N": ignore client/sink-input traffic.
					const auto at = line.find(" on sink #");
					if (at != std::string::npos &&
						(line.find("'new'") != std::string::npos ||
						 std::strtol(line.c_str() + at + 10, nullptr, 10) == info.index)) {
						dirty = true;
					}
				}
			}
		}
		if (dirty) {
			// Coalesce a slider drag into one query per ~20 ms.
			pollfd wake{g_wake[0], POLLIN, 0};
			if (poll(&wake, 1, 20) > 0) {
				break;
			}
			if (Run({"pactl", "list", "sinks"}, out)) {
				const auto latest = ParseSink(out);
				if (latest.found) {
					info = latest;
				} else {
					info.found = false;
				}
				Publish(info);
			}
		}
	}
	Reap(sub, true);
	Publish(SinkInfo{});
	if (!module_id.empty()) {
		Run({"pactl", "unload-module", module_id}, out);
	}
}

} // namespace

void Ensure() {
	std::lock_guard lock(g_mutex);
	if (g_started) {
		return;
	}
	g_started = true;
	if (pipe2(g_wake, O_CLOEXEC) != 0) {
		return;
	}
	g_thread = std::thread(Worker);
}

float Gain() { return g_gain.load(std::memory_order_relaxed); }

bool Active() { return g_active.load(std::memory_order_relaxed); }

void Shutdown() {
	std::lock_guard lock(g_mutex);
	if (!g_started) {
		return;
	}
	if (g_wake[1] >= 0) {
		const char byte = 1;
		(void)!write(g_wake[1], &byte, 1);
	}
	if (g_thread.joinable()) {
		g_thread.join();
	}
	for (auto& fd : g_wake) {
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
	g_started = false;
}

} // namespace Libs::Controller::DualSenseSpeakerSink

#else

namespace Libs::Controller::DualSenseSpeakerSink {
void Ensure() {}
float Gain() { return 1.0f; }
bool Active() { return false; }
void Shutdown() {}
} // namespace Libs::Controller::DualSenseSpeakerSink

#endif
