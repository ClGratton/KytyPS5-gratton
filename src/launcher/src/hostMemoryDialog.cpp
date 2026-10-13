#include "hostMemoryDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#if defined(_WIN32)
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

QString Quote(const QString& text) {
	if (!text.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_./:=@%+,-]")))) {
		return text;
	}
	QString quoted = text;
	quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
	return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

#if defined(_WIN32)
// Runs powershell.exe with `arguments` after a UAC prompt and waits for it. Returns the exit code,
// or -1 when the prompt was declined or the process could not be started.
int RunElevatedPowerShell(const QString& arguments) {
	SHELLEXECUTEINFOW info {};
	info.cbSize       = sizeof(info);
	info.fMask        = SEE_MASK_NOCLOSEPROCESS;
	info.lpVerb       = L"runas";
	info.lpFile       = L"powershell.exe";
	const std::wstring parameters = arguments.toStdWString();
	info.lpParameters = parameters.c_str();
	info.nShow        = SW_HIDE;
	if (ShellExecuteExW(&info) == FALSE || info.hProcess == nullptr) {
		return -1;
	}
	WaitForSingleObject(info.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return static_cast<int>(code);
}
#endif

QLabel* WrappedLabel(QWidget* parent) {
	auto* label = new QLabel(parent);
	label->setWordWrap(true);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	return label;
}

} // namespace

HostMemoryDialog::HostMemoryDialog(QWidget* parent): QDialog(parent) {
	setWindowTitle(tr("Host memory"));
	setMinimumWidth(620);

	auto* layout = new QVBoxLayout(this);

	m_headline = new QLabel(this);
	auto font  = m_headline->font();
	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * 1.2);
	m_headline->setFont(font);
	layout->addWidget(m_headline);

	m_explanation = WrappedLabel(this);
	layout->addWidget(m_explanation);
	m_summary = WrappedLabel(this);
	layout->addWidget(m_summary);
	m_drawbacks = WrappedLabel(this);
	layout->addWidget(m_drawbacks);
	m_note = WrappedLabel(this);
	layout->addWidget(m_note);

	auto* facts_box    = new QGroupBox(tr("Detected on this computer"), this);
	auto* facts_layout = new QVBoxLayout(facts_box);
	m_facts            = new QPlainTextEdit(facts_box);
	m_facts->setReadOnly(true);
	m_facts->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	m_facts->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	m_facts->setMinimumHeight(130);
	facts_layout->addWidget(m_facts);
	layout->addWidget(facts_box);

	m_manual = WrappedLabel(this);
	layout->addWidget(m_manual);

	m_log = new QPlainTextEdit(this);
	m_log->setReadOnly(true);
	m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_log->setMinimumHeight(110);
	m_log->setVisible(false);
	layout->addWidget(m_log);

	m_dismiss = new QCheckBox(tr("Do not suggest this at startup"), this);
	m_dismiss->setVisible(false);
	layout->addWidget(m_dismiss);

	auto* buttons = new QHBoxLayout();
	m_apply       = new QPushButton(tr("Apply"), this);
	m_apply->setDefault(true);
	m_preview = new QPushButton(tr("Preview"), this);
	m_preview->setToolTip(tr("Shows what Apply would do. Changes nothing and needs no password."));
	m_undo = new QPushButton(tr("Undo"), this);
	m_undo->setToolTip(tr("Removes what this window installed and restores the previous settings."));
	m_refresh = new QPushButton(tr("Refresh"), this);
	auto* close = new QPushButton(tr("Close"), this);
	buttons->addWidget(m_apply);
	buttons->addWidget(m_preview);
	buttons->addWidget(m_undo);
	buttons->addStretch();
	buttons->addWidget(m_refresh);
	buttons->addWidget(close);
	layout->addLayout(buttons);

	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	connect(m_refresh, &QPushButton::clicked, this, [this] { Refresh(); });
	connect(m_apply, &QPushButton::clicked, this, [this] { RunHelper(m_report.apply_args, true, false); });
	connect(m_preview, &QPushButton::clicked, this, [this] { RunHelper(m_report.apply_args, false, true); });
	connect(m_undo, &QPushButton::clicked, this, [this] {
#if defined(_WIN32)
		RunHelper({QStringLiteral("-Action"), QStringLiteral("Undo")}, true, false);
#else
		RunHelper({QStringLiteral("undo")}, true, false);
#endif
	});
	connect(m_dismiss, &QCheckBox::toggled, this,
	        [this](bool checked) { HostMemoryTool::SetDismissed(m_report.action_key, checked); });
}

void HostMemoryDialog::SetStartupProposal(bool proposal) {
	m_startup = proposal;
	m_dismiss->setVisible(proposal);
}

QString HostMemoryDialog::ManualCommand() const {
#if defined(_WIN32)
	return {};
#else
	QStringList words;
	words << QStringLiteral("sudo") << Quote(m_report.helper_path) << m_report.apply_args;
	return words.join(QLatin1Char(' '));
#endif
}

void HostMemoryDialog::SetReport(const HostMemoryReport& report) {
	m_report = report;
	m_headline->setText(report.headline);
	m_explanation->setText(report.explanation);
	m_explanation->setVisible(!report.explanation.isEmpty());
	m_summary->setText(report.summary);
	m_summary->setVisible(!report.summary.isEmpty());
	m_drawbacks->setText(report.drawbacks);
	m_drawbacks->setVisible(!report.drawbacks.isEmpty());
	m_note->setText(report.note);
	m_note->setVisible(!report.note.isEmpty());
	m_facts->setPlainText(report.facts);

	const bool has_action = !report.apply_args.isEmpty();
	m_apply->setVisible(has_action);
	m_preview->setVisible(has_action && !report.helper_path.isEmpty());
	m_undo->setVisible(report.can_undo);
	if (has_action) {
		m_apply->setText(report.apply_label);
	}
	const bool idle = !m_busy;
	m_apply->setEnabled(idle && report.can_apply && report.elevation_available);
	m_preview->setEnabled(idle && has_action && !report.helper_path.isEmpty());
	m_undo->setEnabled(idle && report.can_undo && !report.blocked && report.elevation_available);
	m_refresh->setEnabled(idle);

	const bool manual = !report.windows && has_action && !report.elevation_available && !report.helper_path.isEmpty();
	m_manual->setVisible(manual);
	if (manual) {
		m_manual->setText(tr("Command: %1").arg(ManualCommand()));
	}
	if (m_startup) {
		m_dismiss->setChecked(HostMemoryTool::IsDismissed(report.action_key));
	}
	emit ReportChanged(report);
}

void HostMemoryDialog::Refresh() {
	HostMemoryTool::Detect(this, [this](const HostMemoryReport& report) { SetReport(report); });
}

void HostMemoryDialog::AppendLog(const QString& text) {
	m_log->setVisible(true);
	m_log->appendPlainText(text.trimmed());
}

void HostMemoryDialog::RunHelper(const QStringList& args, bool elevated, bool dry_run) {
	if (m_busy || m_report.helper_path.isEmpty()) {
		return;
	}
	m_busy = true;
	m_log->clear();
	SetReport(m_report);  // greys the buttons

#if defined(_WIN32)
	QStringList script {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
	                    QStringLiteral("-File"), m_report.helper_path};
	script << args;
	if (dry_run) {
		script << QStringLiteral("-DryRun");
	}
	if (!elevated) {
		auto* process = new QProcess(this);
		process->setProcessChannelMode(QProcess::MergedChannels);
		connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
			AppendLog(QString::fromLocal8Bit(process->readAll()));
			process->deleteLater();
			Finished(code, false);
		});
		process->start(QStringLiteral("powershell.exe"), script);
		return;
	}
	// For ShellExecute the arguments are one string: quote the script path.
	QStringList quoted = script;
	quoted[4]          = QStringLiteral("\"%1\"").arg(m_report.helper_path);
	auto* watcher = new QFutureWatcher<int>(this);
	connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
		const int code = watcher->result();
		watcher->deleteLater();
		// The elevated process cannot be read from here; its log file is.
		QFile log(qEnvironmentVariable("ProgramData") + QStringLiteral("/KytyHostMemory/last-run.log"));
		if (log.open(QIODevice::ReadOnly | QIODevice::Text)) {
			AppendLog(QString::fromLocal8Bit(log.readAll()));
		}
		Finished(code, code == -1);
	});
	watcher->setFuture(QtConcurrent::run([line = quoted.join(QLatin1Char(' '))] { return RunElevatedPowerShell(line); }));
#else
	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_process, &QProcess::readyRead, this, [this] { AppendLog(QString::fromLocal8Bit(m_process->readAll())); });
	connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		AppendLog(QString::fromLocal8Bit(m_process->readAll()));
		m_process->deleteLater();
		m_process = nullptr;
		// pkexec: 126 = the prompt was dismissed or the password was wrong, 127 = not authorised.
		Finished(code, code == 126 || code == 127);
	});
	connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			AppendLog(tr("Could not start the helper."));
			m_process->deleteLater();
			m_process = nullptr;
			Finished(-1, false);
		}
	});
	QStringList full = args;
	if (dry_run) {
		full << QStringLiteral("--dry-run");
	}
	if (elevated) {
		m_process->start(QStringLiteral("pkexec"), QStringList {m_report.helper_path} + full);
	} else {
		m_process->start(m_report.helper_path, full);
	}
#endif
}

void HostMemoryDialog::Finished(int exit_code, bool cancelled) {
	m_busy = false;
	if (cancelled) {
		AppendLog(tr("The administrator prompt was cancelled. Nothing was changed."));
	} else if (exit_code != 0) {
		AppendLog(tr("The helper stopped with an error (exit code %1).").arg(exit_code));
	}
	Refresh();
}
