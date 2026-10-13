#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_REPORT_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_REPORT_H_

// What the "Host memory" window shows: the detected setup, the verdict and the helper command to run.
// Builds on the pure policy (hostMemoryPolicy.h) and the Linux probe; on Windows the facts come from
// kyty-host-memory.ps1 -Action Status (not tested on real Windows).

#include "hostMemoryPolicy.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

// Where the root helper stands (Linux). The helper is installed root-owned in a root-only directory the first time
// Apply or Undo is used; until then, or when the installed copy is older than the launcher's, a one-time install
// step runs first (in the same password prompt).
enum class HelperState {
	Current,       // installed, trusted, and the text the launcher was built with
	Outdated,      // installed and trusted, but another version: the install step replaces it
	NotInstalled,  // not there yet: the install step puts it in `helper_path`
	Untrusted,     // something is there (or no safe place exists) that fails the ownership check: refuse
	BuiltIn,       // Windows: the script is passed to an elevated PowerShell as an encoded command
};

struct HostMemoryReport {
	HostMemory::Verdict verdict = HostMemory::Verdict::Ok;
	bool                windows = false;

	QString headline;     // one line
	QString explanation;  // why
	QString summary;      // what Apply does, in words (empty without an action)
	QString drawbacks;    // costs of the proposed action (empty without one)
	QString facts;        // what was detected, one fact per line
	QString note;         // a restart is needed, the emulator must be closed, ... (empty when none)

	QString     action_key;    // identifies the proposal for "do not suggest again" (empty without an action)
	QString     apply_label;   // text of the Apply button
	QStringList apply_args;    // arguments of the helper for Apply
	bool        can_apply = false;
	bool        can_undo  = false;
	bool        blocked   = false;  // an action exists but cannot run now (emulator running)

	HelperState helper_state = HelperState::NotInstalled;
	QString     helper_path;      // where the helper is, or will be installed (Linux)
	QString     helper_problem;   // why it is refused (Untrusted)
	bool        elevation_available = true;  // pkexec (Linux) or UAC (Windows)
};

namespace HostMemoryTool {

struct HelperInfo {
	HelperState state = HelperState::NotInstalled;
	QString     path;     // installed path, or the path an install would use
	QString     problem;  // when Untrusted
};

// Looks at the installed helper on this system (ownership of the file and every directory above it, and its
// SHA-256 against the one compiled into the launcher). Linux only.
HelperInfo InspectHelper();

HostMemoryReport MakeLinuxReport(const HostMemory::LinuxFacts& facts, const HelperInfo& helper);
// `built_in`: the PowerShell script is compiled into the launcher (false only in tests of the report).
HostMemoryReport MakeWindowsReport(const HostMemory::WindowsFacts& facts, bool built_in = true);

// The command that runs the helper with `helper_args`.
//  elevated: Linux -> pkexec on the installed helper (after re-checking it), or, when the helper is not installed or
//            outdated, pkexec on /bin/sh with the one-time install step, which then runs the helper.
//            Windows -> powershell.exe with the script as an encoded command; the caller elevates it with UAC.
//  not elevated (Preview): the compiled-in script run by the current user with --dry-run.
// `error` is set (and `program` empty) when the helper is refused.
struct HostCommand {
	QString     program;
	QStringList args;
	QString     error;
};
HostCommand BuildHelperCommand(const HostMemoryReport& report, const QStringList& helper_args, bool elevated, bool dry_run);

// Detects the host and calls `done` (right away on Linux; after PowerShell has answered on Windows).
void Detect(QObject* context, const std::function<void(const HostMemoryReport&)>& done);

// A short line for the main window.
QString StatusLine(const HostMemoryReport& report);

// "do not suggest again" is remembered per proposal, in the launcher's settings file.
bool IsDismissed(const QString& action_key);
void SetDismissed(const QString& action_key, bool dismissed);

// Whether to open the window by itself at startup: a recommended action, not blocked, not dismissed.
bool ShouldProposeAtStartup(const HostMemoryReport& report);

} // namespace HostMemoryTool

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_REPORT_H_ */
