#ifndef LAUNCHER_INCLUDE_EMULATOR_OPTIONS_PANEL_H_
#define LAUNCHER_INCLUDE_EMULATOR_OPTIONS_PANEL_H_

#include "emulatorOptions.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QFormLayout;
class QWidget;

// The controls of the EmulatorOptions table: one control per option, built from the table, and the
// conversion between the controls and a QVariantMap of values by option id.
class EmulatorOptionsPanel: public QObject {
	Q_OBJECT

public:
	explicit EmulatorOptionsPanel(QObject* parent = nullptr);

	// A widget with one group box per section of the tab (or, for a tab without section titles,
	// plain rows). The controls belong to this panel's lifetime through `parent`.
	[[nodiscard]] QWidget* BuildSections(EmulatorOptions::Tab tab, QWidget* parent);
	// Appends the tab's controls as rows of an existing form layout.
	void AddRows(QFormLayout* layout, EmulatorOptions::Tab tab, QWidget* parent);

	// Shows `values` in the controls (a missing id shows the default).
	void Load(const QVariantMap& values);
	// The value of every control, by option id.
	[[nodiscard]] QVariantMap Save() const;

private:
	void AddRow(QFormLayout* layout, const EmulatorOptions::Option& option, QWidget* parent);
	void UpdateDependencies();

	QHash<QString, QWidget*> m_controls;
};

#endif /* LAUNCHER_INCLUDE_EMULATOR_OPTIONS_PANEL_H_ */
