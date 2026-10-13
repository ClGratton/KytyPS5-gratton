#ifndef KYTY_COMMON_RUNTIME_TOGGLES_H_
#define KYTY_COMMON_RUNTIME_TOGGLES_H_

// Runtime toggles: flip a registered performance flag while the emulator runs (--toggle-control).
//
// Why: an A/B comparison of two flag values normally needs two boots and a walk to the scene for
// every side, and run-to-run noise on top. With a toggle the same boot measures A, B, A, B ... and
// the noise is the drift inside one run.
//
// Rules (they are what makes a flag "runtime-safe"):
//  * Only flags that are registered here can be toggled. A flag registers itself only after its
//    consumers were read: every read happens per call (no copy cached at start-up, no state that
//    depends on the value staying the same), so a change between two calls is harmless.
//  * Correctness flags are never registered: nothing a game needs to run correctly can be turned off.
//  * A flag that needs a resource set up at start-up (a second queue, a worker thread) registers
//    `requires_armed`: it can be turned off and on again, but only if it was on at start-up.
//  * The new value takes effect at a frame boundary (the presenter calls OnFrame after a present),
//    so all threads see the change between two frames, never inside the frame's bookkeeping.
//  * The flag fields are plain bools in Config::ConfigOptions. Every reader of a registered field
//    goes through Read(), the writer through Write(): relaxed atomic accesses on the same byte.
//
// Control file (--toggle-control <path>): text, one `name=value` per line (`name value` also works,
// `#` starts a comment, a leading `--` on the name is ignored, value true|false|on|off|1|0|yes|no).
// The harness writes it atomically (temp file + rename); the emulator polls it (50 ms) and applies
// a changed file at the next frame. Applied changes (and refusals) are logged loudly and appended
// to <path>.applied.csv as `t_ns,frame,flag,old,new,result` (t_ns: the steady clock of
// --bench-frames-csv, so a harness can cut frames.csv into phases exactly).
//
// This header is the pure logic (unit tested in tests/RuntimeTogglesTests.cpp); the watcher thread,
// the log and the global instance are in runtimeToggles.cpp.

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Common::RuntimeToggles {

// Relaxed atomic access to a plain bool field (C++20 atomic_ref); the field must outlive the refs.
[[nodiscard]] inline bool Read(bool& field) noexcept {
	return std::atomic_ref<bool>(field).load(std::memory_order_relaxed);
}
inline void Write(bool& field, bool value) noexcept {
	std::atomic_ref<bool>(field).store(value, std::memory_order_relaxed);
}

enum class Result : uint8_t {
	Applied,     // the value changed
	Unchanged,   // already had that value
	UnknownFlag, // not registered: not a flag, or not runtime-safe
	NotArmed,    // requires_armed and it was off at start-up
	BadValue,    // the value is not a boolean
};

[[nodiscard]] inline const char* ResultName(Result r) noexcept {
	switch (r) {
		case Result::Applied: return "applied";
		case Result::Unchanged: return "unchanged";
		case Result::UnknownFlag: return "unknown_or_not_runtime_safe";
		case Result::NotArmed: return "not_armed_at_startup";
		case Result::BadValue: return "bad_value";
	}
	return "?";
}

struct Change {
	std::string name;
	Result      result    = Result::UnknownFlag;
	bool        old_value = false;
	bool        new_value = false;
};

[[nodiscard]] inline bool ParseBool(std::string_view text, bool& out) noexcept {
	std::string lower;
	for (char c: text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (lower == "true" || lower == "on" || lower == "1" || lower == "yes") {
		out = true;
		return true;
	}
	if (lower == "false" || lower == "off" || lower == "0" || lower == "no") {
		out = false;
		return true;
	}
	return false;
}

class Registry {
public:
	// `field` must stay valid for the life of the registry. requires_armed: see the header comment.
	void Register(std::string_view name, bool* field, bool requires_armed) {
		std::lock_guard lock(m_mutex);
		m_entries.push_back({std::string(Strip(name)), field, requires_armed, false});
	}

	// Records which flags are on right now (call once, after the configuration was loaded).
	void Arm() {
		std::lock_guard lock(m_mutex);
		for (auto& e: m_entries) e.armed = Read(*e.field);
	}

	[[nodiscard]] Result Set(std::string_view name, bool value, bool* old_out = nullptr) {
		std::lock_guard lock(m_mutex);
		return SetLocked(Strip(name), value, old_out);
	}

	// Applies every line of `text`; returns one Change per line that names a flag.
	std::vector<Change> ApplyText(std::string_view text) {
		std::vector<Change> changes;
		std::lock_guard     lock(m_mutex);
		size_t              pos = 0;
		while (pos <= text.size()) {
			auto end = text.find('\n', pos);
			if (end == std::string_view::npos) end = text.size();
			auto line = Trim(text.substr(pos, end - pos));
			pos       = end + 1;
			if (line.empty() || line.front() == '#') continue;
			auto sep = line.find_first_of("= \t");
			if (sep == std::string_view::npos) continue; // a lone word is not an assignment
			const auto name = Strip(Trim(line.substr(0, sep)));
			auto       rest = Trim(line.substr(sep));
			if (!rest.empty() && rest.front() == '=') rest = Trim(rest.substr(1));
			Change c;
			c.name = std::string(name);
			bool value = false;
			if (!ParseBool(rest, value)) {
				c.result = Result::BadValue;
			} else {
				c.new_value = value;
				c.result    = SetLocked(name, value, &c.old_value);
			}
			changes.push_back(std::move(c));
		}
		return changes;
	}

	[[nodiscard]] std::vector<std::string> Names() const {
		std::lock_guard          lock(m_mutex);
		std::vector<std::string> names;
		for (const auto& e: m_entries) names.push_back(e.name);
		return names;
	}

	[[nodiscard]] bool Has(std::string_view name) const {
		std::lock_guard lock(m_mutex);
		for (const auto& e: m_entries) {
			if (e.name == Strip(name)) return true;
		}
		return false;
	}

private:
	struct Entry {
		std::string name;
		bool*       field;
		bool        requires_armed;
		bool        armed;
	};

	static std::string_view Trim(std::string_view s) {
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
		return s;
	}
	static std::string_view Strip(std::string_view name) {
		while (!name.empty() && name.front() == '-') name.remove_prefix(1);
		return name;
	}

	Result SetLocked(std::string_view name, bool value, bool* old_out) {
		for (auto& e: m_entries) {
			if (e.name != name) continue;
			const bool old = Read(*e.field);
			if (old_out != nullptr) *old_out = old;
			if (value && e.requires_armed && !e.armed) return Result::NotArmed;
			if (old == value) return Result::Unchanged;
			Write(*e.field, value);
			return Result::Applied;
		}
		return Result::UnknownFlag;
	}

	mutable std::mutex m_mutex;
	std::vector<Entry> m_entries;
};

// ---- process-wide instance (runtimeToggles.cpp) ----------------------------------------------

// Starts the watcher for `control_path` (empty = off) and registers the flags of the config. Call
// after Config::Load filled the configuration.
void Start(const std::string& control_path);
void Stop();

namespace detail {
inline std::atomic<bool> g_pending {false};
inline std::atomic<bool> g_active {false};
void                     ApplyPending(uint64_t frame_idx);
} // namespace detail

[[nodiscard]] inline bool Active() noexcept {
	return detail::g_active.load(std::memory_order_relaxed);
}

// Frame boundary hook: the presenter calls it after each present (only when Active()). One relaxed
// load when nothing is pending.
inline void OnFrame(uint64_t frame_idx) {
	if (detail::g_pending.load(std::memory_order_relaxed)) detail::ApplyPending(frame_idx);
}

} // namespace Common::RuntimeToggles

#endif /* KYTY_COMMON_RUNTIME_TOGGLES_H_ */
