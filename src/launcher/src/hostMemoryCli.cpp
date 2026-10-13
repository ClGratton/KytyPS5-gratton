#include "hostMemoryDialog.h"
#include "hostMemoryProbe.h"
#include "launcherSelfCheck.h"

#include <QCoreApplication>
#include <QPixmap>
#include <QStringList>
#include <QTextStream>

namespace {

QString RootArgument(const QStringList& args) {
	const auto at = args.indexOf(QStringLiteral("--root"));
	return at >= 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
}

HostMemoryReport LinuxReportFor(const QStringList& args) {
	HostMemory::ProbeEnv env;
	env.root = RootArgument(args).toStdString();
	return HostMemoryTool::MakeLinuxReport(HostMemory::ProbeLinux(env), HostMemoryTool::HelperPath());
}

} // namespace

int RunHostMemory(const QStringList& args) {
	QTextStream out(stdout);
#if defined(_WIN32)
	Q_UNUSED(args);
	out << "launcher --host-memory prints the Linux report only; the Windows report is in the launcher window.\n";
	return 0;
#else
	const auto report = LinuxReportFor(args);
	out << "Verdict: " << HostMemory::VerdictName(report.verdict) << " - " << report.headline << "\n\n";
	out << report.facts << "\n\n";
	out << report.explanation << "\n";
	if (!report.drawbacks.isEmpty()) {
		out << "\n" << report.drawbacks << "\n";
	}
	if (!report.note.isEmpty()) {
		out << "\nNote: " << report.note << "\n";
	}
	if (!report.apply_args.isEmpty()) {
		out << "\nApply: " << (report.can_apply ? "available" : "not available now") << " (" << report.apply_label
		    << ")\n  pkexec " << report.helper_path << " " << report.apply_args.join(QLatin1Char(' ')) << "\n";
	}
	out << "Undo: " << (report.can_undo ? "available" : "nothing installed") << "\n";
	return 0;
#endif
}

int RunGrabHostMemory(const QStringList& args) {
	if (args.isEmpty()) {
		QTextStream(stderr) << "usage: launcher --grab-host-memory <png> [--root <dir>]\n";
		return 2;
	}
	HostMemoryDialog dialog;
#if defined(_WIN32)
	dialog.SetReport(HostMemoryTool::MakeWindowsReport(HostMemory::WindowsFacts {}, HostMemoryTool::HelperPath()));
#else
	dialog.SetReport(LinuxReportFor(args));
#endif
	dialog.show();
	QCoreApplication::processEvents();
	return dialog.grab().save(args.first()) ? 0 : 1;
}
