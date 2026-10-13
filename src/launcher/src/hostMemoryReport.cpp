#include "hostMemoryReport.h"

#include "hostMemoryProbe.h"

#include <QCoreApplication>
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

QString HelperPath() {
#if defined(_WIN32)
	const QString name = QStringLiteral("kyty-host-memory.ps1");
#else
	const QString name = QStringLiteral("kyty-host-memory");
#endif
	QDir dir(QCoreApplication::applicationDirPath());
	for (int i = 0; i < 2; i++) {
		const auto candidate = dir.absoluteFilePath(name);
		if (QFile::exists(candidate)) {
			return candidate;
		}
		if (!dir.cdUp()) {
			break;
		}
	}
	return {};
}

HostMemoryReport MakeLinuxReport(const LinuxFacts& facts, const QString& helper_path) {
	const Advice advice = DecideLinux(facts);

	HostMemoryReport report;
	report.verdict     = advice.verdict;
	report.headline    = Headline(advice.verdict, false);
	report.explanation = Q(ExplainLinux(facts, advice));
	report.drawbacks   = Q(DrawbacksLinux(advice));
	report.facts       = Q(DescribeLinux(facts)).trimmed();
	report.helper_path = helper_path;
	report.can_undo    = advice.undo_available && !helper_path.isEmpty();
	report.blocked     = advice.blocked;
	report.elevation_available = facts.has_pkexec;
	report.action_key  = ActionKey(advice);

	if (advice.action != Action::None) {
		report.apply_label = advice.action == Action::CreateSwapfile
		                         ? QObject::tr("Add swapfile and turn on zswap")
		                         : QObject::tr("Turn on zswap");
		for (const auto& arg: ApplyArguments(advice)) {
			report.apply_args << Q(arg);
		}
		report.can_apply = !helper_path.isEmpty() && !advice.blocked;
	}
	if (advice.blocked && (advice.action != Action::None || report.can_undo)) {
		report.note = QObject::tr("An emulator is running. Close it first: the change needs free memory.");
	} else if (helper_path.isEmpty() && (advice.action != Action::None || advice.undo_available)) {
		report.note = QObject::tr("The helper script kyty-host-memory is missing next to the launcher.");
	} else if (!facts.has_pkexec && advice.action != Action::None) {
		report.note = QObject::tr("pkexec (polkit) is not installed. Run the command below as root instead.");
	}
	return report;
}

HostMemoryReport MakeWindowsReport(const WindowsFacts& facts, const QString& helper_path) {
	const WindowsAdvice advice = DecideWindows(facts);

	HostMemoryReport report;
	report.windows     = true;
	report.verdict     = advice.verdict;
	report.headline    = Headline(advice.verdict, true);
	report.explanation = Q(ExplainWindows(facts, advice));
	report.drawbacks   = Q(DrawbacksWindows(advice));
	report.facts       = Q(DescribeWindows(facts)).trimmed();
	report.helper_path = helper_path;
	report.can_undo    = advice.undo_available && !helper_path.isEmpty();
	report.blocked     = advice.blocked;

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
		report.can_apply = !helper_path.isEmpty() && !advice.blocked;
	}
	report.note = QObject::tr("Windows support of this window is untested on real Windows.");
	if (advice.blocked) {
		report.note += QStringLiteral(" ") + QObject::tr("An emulator is running; close it first.");
	}
	return report;
}

void Detect(QObject* context, const std::function<void(const HostMemoryReport&)>& done) {
#if defined(_WIN32)
	const QString helper = HelperPath();
	if (helper.isEmpty()) {
		done(MakeWindowsReport(WindowsFacts {}, helper));
		return;
	}
	auto* process = new QProcess(context);
	QObject::connect(process, &QProcess::finished, context, [process, helper, done](int, QProcess::ExitStatus) {
		const auto text = QString::fromUtf8(process->readAllStandardOutput()).toStdString();
		process->deleteLater();
		done(MakeWindowsReport(ParseWindowsFacts(text), helper));
	});
	QObject::connect(process, &QProcess::errorOccurred, context, [process, helper, done](QProcess::ProcessError) {
		if (process->state() == QProcess::NotRunning) {
			process->deleteLater();
			done(MakeWindowsReport(WindowsFacts {}, helper));
		}
	});
	process->start(QStringLiteral("powershell.exe"),
	               {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
	                QStringLiteral("-File"), helper, QStringLiteral("-Action"), QStringLiteral("Status")});
#else
	(void)context;
	done(MakeLinuxReport(ProbeLinux(ProbeEnv {}), HelperPath()));
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
