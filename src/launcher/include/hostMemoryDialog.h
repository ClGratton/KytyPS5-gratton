#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_DIALOG_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_DIALOG_H_

#include "hostMemoryReport.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

// The "Host memory" window: what was detected, what is proposed, and the buttons that run the root
// helper (pkexec on Linux, a UAC prompt on Windows). Nothing runs without a click.
class HostMemoryDialog: public QDialog {
	Q_OBJECT

public:
	explicit HostMemoryDialog(QWidget* parent = nullptr);

	// Shows `report` (the window refreshes itself after every action).
	void SetReport(const HostMemoryReport& report);
	// Offers the "do not suggest this at startup" check box.
	void SetStartupProposal(bool proposal);

signals:
	void ReportChanged(const HostMemoryReport& report);

private:
	void Refresh();
	void RunHelper(const QStringList& args, bool elevated, bool dry_run);
	void Finished(int exit_code, bool cancelled);
	void AppendLog(const QString& text);
	[[nodiscard]] QString ManualCommand() const;

	HostMemoryReport m_report;
	QLabel*          m_headline    = nullptr;
	QLabel*          m_explanation = nullptr;
	QLabel*          m_summary     = nullptr;
	QLabel*          m_drawbacks   = nullptr;
	QLabel*          m_note        = nullptr;
	QPlainTextEdit*  m_facts       = nullptr;
	QPlainTextEdit*  m_log         = nullptr;
	QLabel*          m_manual      = nullptr;
	QPushButton*     m_apply       = nullptr;
	QPushButton*     m_preview     = nullptr;
	QPushButton*     m_undo        = nullptr;
	QPushButton*     m_refresh     = nullptr;
	QCheckBox*       m_dismiss     = nullptr;
	QProcess*        m_process     = nullptr;
	bool             m_busy        = false;
	bool             m_startup     = false;
	QString          m_running_action;
};

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_DIALOG_H_ */
