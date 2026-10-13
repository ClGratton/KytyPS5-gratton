#include "hostMemoryReport.h"

#include "hostMemoryProbe.h"
#include "hostMemoryScript.h"
#include "hostMemoryTrust.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>

#include <string>

namespace {

using namespace HostMemory;

QString Q(const std::string& text) {
	return QString::fromStdString(text);
}

QString Headline(Verdict verdict, bool windows) {
	switch (verdict) {
		case Verdict::Ok: return QObject::tr("OK: nothing to do");
		case Verdict::Recommended:
			return windows ? QObject::tr("Recommended: fix the Windows memory settings")
			               : QObject::tr("Recommended: use compressed swap with your disk");
		case Verdict::Optional: return QObject::tr("Optional: add a swapfile");
		case Verdict::Tuned: return QObject::tr("OK: already set up");
		case Verdict::NotRecommended: return QObject::tr("Not changed: the usual fix does not suit this computer");
		case Verdict::Unsupported: return QObject::tr("Not available on this computer");
	}
	return {};
}

QString ActionKey(const Advice& advice) {
	switch (advice.action) {
		case Action::EnableZswap: return QStringLiteral("linux:zswap");
		case Action::CreateSwapfile: return QStringLiteral("linux:swapfile");
		case Action::None: break;
	}
	return {};
}

} // namespace

namespace HostMemoryTool {

HelperInfo InspectHelper() {
	HelperInfo info;
#if defined(_WIN32)
	info.state = HelperState::BuiltIn;
	return info;
#else
	const HostMemory::StatFn    lstat   = HostMemory::RealLstat;
	const HostMemory::ResolveFn resolve = HostMemory::RealResolve;

	// An installed helper: the first candidate path that exists must pass the ownership check.
	for (const auto& candidate: HostMemory::HelperInstallPaths()) {
		if (!lstat(candidate).exists) {
			continue;
		}
		info.path = QString::fromStdString(candidate);
		const auto trust = HostMemory::CheckRootOwnedExecutable(candidate, lstat, resolve);
		if (!trust.ok) {
			info.state   = HelperState::Untrusted;
			info.problem = QObject::tr("The helper at %1 is refused (%2).").arg(info.path, QString::fromStdString(trust.reason));
			return info;
		}
		QFile file(info.path);
		const bool readable = file.open(QIODevice::ReadOnly);
		const auto hash     = readable ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex() : QByteArray();
		info.state          = hash == QByteArray(kHostMemoryScriptSha256) ? HelperState::Current : HelperState::Outdated;
		return info;
	}

	// Not installed: the first location whose existing parent directories are all root-only.
	std::string reasons;
	for (const auto& candidate: HostMemory::HelperInstallPaths()) {
		const std::string dir      = candidate.substr(0, candidate.rfind('/'));
		const std::string existing = HostMemory::NearestExistingDirectory(dir, lstat);
		const auto        trust    = HostMemory::CheckRootOnlyDirectory(existing, lstat, resolve);
		if (trust.ok) {
			info.state = HelperState::NotInstalled;
			info.path  = QString::fromStdString(candidate);
			return info;
		}
		reasons += (reasons.empty() ? "" : "; ") + trust.reason;
	}
	info.state   = HelperState::Untrusted;
	info.problem = QObject::tr("No root-only place for the helper (%1).").arg(QString::fromStdString(reasons));
	return info;
#endif
}

HostMemoryReport MakeLinuxReport(const LinuxFacts& facts, const HelperInfo& helper) {
	const Advice advice = DecideLinux(facts);

	HostMemoryReport report;
	report.verdict     = advice.verdict;
	report.headline    = Headline(advice.verdict, false);
	report.explanation = Q(ExplainLinux(facts, advice));
	report.summary     = Q(ActionSummaryLinux(advice));
	report.drawbacks   = Q(DrawbacksLinux(advice));
	report.facts       = Q(DescribeLinux(facts)).trimmed();
	report.helper_state   = helper.state;
	report.helper_path    = helper.path;
	report.helper_problem = helper.problem;
	const bool helper_ok  = helper.state != HelperState::Untrusted;
	report.can_undo       = advice.undo_available && helper_ok;
	report.blocked        = advice.blocked;
	report.elevation_available = facts.has_pkexec;
	report.action_key  = ActionKey(advice);

	if (advice.action != Action::None) {
		report.apply_label = advice.action == Action::CreateSwapfile
		                         ? QObject::tr("Add swapfile and turn on zswap")
		                         : QObject::tr("Turn on zswap");
		for (const auto& arg: ApplyArguments(advice)) {
			report.apply_args << Q(arg);
		}
		report.can_apply = helper_ok && !advice.blocked;
	}
	if (!helper_ok && (advice.action != Action::None || advice.undo_available)) {
		report.note = helper.problem + QStringLiteral(" ") +
		              QObject::tr("It runs with administrator rights, so it must be owned by root and not writable by others.");
	} else if (advice.blocked && (advice.action != Action::None || report.can_undo)) {
		report.note = QObject::tr("An emulator is running. Close it first: the change needs free memory.");
	} else if (!facts.has_pkexec && advice.action != Action::None) {
		report.note = QObject::tr("pkexec (polkit) is not installed, so Apply is not available. To do it by hand, save the helper "
		                          "with: launcher --print-host-helper > kyty-host-memory, then run the command below as root.");
	} else if (helper.state == HelperState::NotInstalled && advice.action != Action::None) {
		report.note = QObject::tr("The first Apply also installs the helper in %1, owned by root (one password prompt).").arg(helper.path);
	} else if (helper.state == HelperState::Outdated && advice.action != Action::None) {
		report.note = QObject::tr("The installed helper in %1 is another version; Apply replaces it (one password prompt).").arg(helper.path);
	}
	return report;
}

HostMemoryReport MakeWindowsReport(const WindowsFacts& facts, bool built_in) {
	const WindowsAdvice advice = DecideWindows(facts);

	HostMemoryReport report;
	report.windows     = true;
	report.verdict     = advice.verdict;
	report.headline    = Headline(advice.verdict, true);
	report.explanation = Q(ExplainWindows(facts, advice));
	report.summary     = Q(ActionSummaryWindows(advice));
	report.drawbacks   = Q(DrawbacksWindows(advice));
	report.facts       = Q(DescribeWindows(facts)).trimmed();
	report.helper_state = HelperState::BuiltIn;
	report.can_undo     = advice.undo_available && built_in;
	report.blocked      = advice.blocked;

	if (advice.system_managed_pagefile || advice.enable_memory_compression) {
		report.action_key  = QStringLiteral("windows:%1%2")
		                         .arg(advice.system_managed_pagefile ? QStringLiteral("pagefile") : QString(),
		                              advice.enable_memory_compression ? QStringLiteral("compression") : QString());
		report.apply_label = QObject::tr("Fix (administrator prompt)");
		report.apply_args << QStringLiteral("-Action") << QStringLiteral("Apply");
		if (advice.system_managed_pagefile) {
			report.apply_args << QStringLiteral("-SystemManagedPagefile");
		}
		if (advice.enable_memory_compression) {
			report.apply_args << QStringLiteral("-EnableMemoryCompression");
		}
		report.can_apply = built_in && !advice.blocked;
	}
	report.note = QObject::tr("Windows support of this window is untested on real Windows.");
	if (advice.blocked) {
		report.note += QStringLiteral(" ") + QObject::tr("An emulator is running; close it first.");
	}
	return report;
}

#if defined(_WIN32)
namespace {

// powershell.exe arguments that run `script_text` with `script_args`, as one encoded command (UTF-16LE, base64): no file
// that something else could replace is involved. Empty list when it would be too long for a command line.
QStringList EncodedPowerShellArguments(const QString& script_text, const QStringList& script_args) {
	const QString command = QStringLiteral("& {\n") + script_text + QStringLiteral("\n} ") + script_args.join(QLatin1Char(' '));
	const QByteArray utf16(reinterpret_cast<const char*>(command.utf16()), static_cast<qsizetype>(command.size()) * 2);
	const QString    encoded = QString::fromLatin1(utf16.toBase64());
	if (encoded.size() > 30000) {  // CreateProcess and ShellExecute allow about 32767 characters
		return {};
	}
	return {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
	        QStringLiteral("-EncodedCommand"), encoded};
}

} // namespace
#endif

HostCommand BuildHelperCommand(const HostMemoryReport& report, const QStringList& helper_args, bool elevated, bool dry_run) {
	HostCommand command;
	const QString script = QString::fromUtf8(kHostMemoryScriptText);
#if defined(_WIN32)
	Q_UNUSED(report);
	Q_UNUSED(elevated);
	QStringList script_args = helper_args;
	if (dry_run) {
		script_args << QStringLiteral("-DryRun");
	}
	command.args = EncodedPowerShellArguments(script, script_args);
	if (command.args.isEmpty()) {
		command.error = QObject::tr("The script is too long for a PowerShell command line.");
		return command;
	}
	command.program = QStringLiteral("powershell.exe");
	return command;
#else
	Q_UNUSED(report);
	QStringList args = helper_args;
	if (dry_run) {
		args << QStringLiteral("--dry-run");
	}
	if (!elevated) {
		// Preview: the compiled-in script, run by the current user; with --dry-run it changes nothing.
		command.program = QStringLiteral("bash");
		command.args    = QStringList {QStringLiteral("-c"), script, QStringLiteral("kyty-host-memory")} + args;
		return command;
	}
	// Re-check right before running: the installed helper and every directory above it.
	const HelperInfo info = InspectHelper();
	if (info.state == HelperState::Untrusted) {
		command.error = info.problem;
		return command;
	}
	command.program = QStringLiteral("pkexec");
	if (info.state == HelperState::Current) {
		command.args = QStringList {info.path} + args;
	} else {
		// Not installed, or another version: one pkexec runs the install step (hash-checked against the build) and then the helper.
		const QByteArray text = script.toUtf8();
		command.args = QStringList {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QString::fromStdString(HostMemory::InstallScriptText()),
		                            QStringLiteral("kyty-install"), info.path, QString::fromLatin1(kHostMemoryScriptSha256),
		                            QString::fromLatin1(text.toBase64()), QStringLiteral("--")} +
		               args;
	}
	return command;
#endif
}

void Detect(QObject* context, const std::function<void(const HostMemoryReport&)>& done) {
#if defined(_WIN32)
	const auto command = BuildHelperCommand(HostMemoryReport {}, {QStringLiteral("-Action"), QStringLiteral("Status")}, false, false);
	if (command.program.isEmpty()) {
		done(MakeWindowsReport(WindowsFacts {}, false));
		return;
	}
	auto* process = new QProcess(context);
	QObject::connect(process, &QProcess::finished, context, [process, done](int, QProcess::ExitStatus) {
		const auto text = QString::fromUtf8(process->readAllStandardOutput()).toStdString();
		process->deleteLater();
		done(MakeWindowsReport(ParseWindowsFacts(text), true));
	});
	QObject::connect(process, &QProcess::errorOccurred, context, [process, done](QProcess::ProcessError) {
		if (process->state() == QProcess::NotRunning) {
			process->deleteLater();
			done(MakeWindowsReport(WindowsFacts {}, true));
		}
	});
	process->start(command.program, command.args);
#else
	(void)context;
	done(MakeLinuxReport(ProbeLinux(ProbeEnv {}), InspectHelper()));
#endif
}

QString StatusLine(const HostMemoryReport& report) {
	switch (report.verdict) {
		case Verdict::Ok: return QObject::tr("Host memory: OK");
		case Verdict::Tuned: return QObject::tr("Host memory: tuned");
		case Verdict::Recommended: return QObject::tr("Host memory: a change is recommended");
		case Verdict::Optional: return QObject::tr("Host memory: a swapfile is optional");
		case Verdict::NotRecommended: return QObject::tr("Host memory: low RAM, no safe fix");
		case Verdict::Unsupported: return QObject::tr("Host memory: not available here");
	}
	return {};
}

namespace {
QSettings OpenSettings() {
	return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Kyty"), QStringLiteral("Kyty"));
}
} // namespace

bool IsDismissed(const QString& action_key) {
	auto settings = OpenSettings();
	return settings.value(QStringLiteral("HostMemory/dismissed")).toStringList().contains(action_key);
}

void SetDismissed(const QString& action_key, bool dismissed) {
	if (action_key.isEmpty()) {
		return;
	}
	auto settings = OpenSettings();
	auto keys     = settings.value(QStringLiteral("HostMemory/dismissed")).toStringList();
	keys.removeAll(action_key);
	if (dismissed) {
		keys << action_key;
	}
	settings.setValue(QStringLiteral("HostMemory/dismissed"), keys);
}

bool ShouldProposeAtStartup(const HostMemoryReport& report) {
	return report.verdict == Verdict::Recommended && report.can_apply && !report.action_key.isEmpty() &&
	       !IsDismissed(report.action_key);
}

} // namespace HostMemoryTool
