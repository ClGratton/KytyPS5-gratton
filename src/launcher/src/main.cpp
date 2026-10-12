#include "launcherSelfCheck.h"
#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QStringList>

int main(int argc, char* argv[]) {
	// Window-less helpers (see launcherSelfCheck.h).
	for (int i = 1; i < argc; i++) {
		const QString arg = QString::fromLocal8Bit(argv[i]);
		if (arg == QLatin1String("--self-check") || arg == QLatin1String("--print-command") ||
		    arg == QLatin1String("--grab-settings") || arg == QLatin1String("--configure")) {
			qputenv("QT_QPA_PLATFORM", "offscreen");
			QApplication app(argc, argv);
			QStringList  rest = QCoreApplication::arguments();
			rest.removeFirst();
			if (arg == QLatin1String("--self-check")) {
				return RunSelfCheck(rest);
			}
			if (arg == QLatin1String("--print-command")) {
				return RunPrintCommand(rest);
			}
			return arg == QLatin1String("--configure") ? RunConfigure(rest) : RunGrabSettings(rest);
		}
	}

	QApplication a(argc, argv);
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
