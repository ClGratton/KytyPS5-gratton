#include "launcherSelfCheck.h"
#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QStringList>

int main(int argc, char* argv[]) {
	// Window-less helpers (see launcherSelfCheck.h).
	for (int i = 1; i < argc; i++) {
		const QString arg = QString::fromLocal8Bit(argv[i]);
		if (arg == QLatin1String("--self-check") || arg == QLatin1String("--print-command")) {
			qputenv("QT_QPA_PLATFORM", "offscreen");
			QApplication app(argc, argv);
			QStringList  rest = QCoreApplication::arguments();
			rest.removeFirst();
			return arg == QLatin1String("--self-check") ? RunSelfCheck(rest)
			                                            : RunPrintCommand(rest);
		}
	}

	QApplication a(argc, argv);
	LauncherTheme::Initialize(a);

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
