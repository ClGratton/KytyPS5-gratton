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

	QString helper_path;  // the helper script next to the launcher (empty when missing)
	bool    elevation_available = true;  // pkexec (Linux) or UAC (Windows)
};

namespace HostMemoryTool {

// The helper script that sits next to the launcher (kyty-host-memory or kyty-host-memory.ps1); empty if absent.
QString HelperPath();

HostMemoryReport MakeLinuxReport(const HostMemory::LinuxFacts& facts, const QString& helper_path);
HostMemoryReport MakeWindowsReport(const HostMemory::WindowsFacts& facts, const QString& helper_path);

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
