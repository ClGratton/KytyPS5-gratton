#include "emulatorOptionsPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

using EmulatorOptions::Kind;
using EmulatorOptions::Option;

EmulatorOptionsPanel::EmulatorOptionsPanel(QObject* parent): QObject(parent) {}

void EmulatorOptionsPanel::AddRow(QFormLayout* layout, const Option& option, QWidget* parent) {
	QWidget* control = nullptr;
	switch (option.kind) {
		case Kind::Bool: {
			auto* box = new QCheckBox(option.label, parent);
			box->setToolTip(option.tooltip);
			connect(box, &QCheckBox::toggled, this, [this]() { UpdateDependencies(); });
			layout->addRow(box);
			control = box;
			break;
		}
		case Kind::Choice: {
			auto* combo = new QComboBox(parent);
			combo->addItems(option.choices);
			combo->setToolTip(option.tooltip);
			connect(combo, &QComboBox::currentTextChanged, this, [this]() { UpdateDependencies(); });
			auto* label = new QLabel(option.label, parent);
			label->setToolTip(option.tooltip);
			layout->addRow(label, combo);
			control = combo;
			break;
		}
		case Kind::Int: {
			auto* spin = new QSpinBox(parent);
			spin->setRange(option.min, option.max);
			spin->setSuffix(option.suffix);
			if (!option.special.isEmpty()) {
				spin->setSpecialValueText(option.special);
			}
			spin->setToolTip(option.tooltip);
			spin->setKeyboardTracking(false);
			auto* label = new QLabel(option.label, parent);
			label->setToolTip(option.tooltip);
			layout->addRow(label, spin);
			control = spin;
			break;
		}
	}
	control->setProperty("optionId", option.id);
	m_controls.insert(option.id, control);
}

QWidget* EmulatorOptionsPanel::BuildSections(EmulatorOptions::Tab tab, QWidget* parent) {
	auto* page   = new QWidget(parent);
	auto* layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	QHash<QString, QFormLayout*> forms;
	for (const auto& option: EmulatorOptions::All()) {
		if (option.tab != tab) {
			continue;
		}
		if (!forms.contains(option.section)) {
			auto* group = new QGroupBox(option.section, page);
			auto* form  = new QFormLayout(group);
			form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			layout->addWidget(group);
			forms.insert(option.section, form);
		}
		AddRow(forms.value(option.section), option, page);
	}
	UpdateDependencies();
	return page;
}

void EmulatorOptionsPanel::AddRows(QFormLayout* layout, EmulatorOptions::Tab tab, QWidget* parent) {
	for (const auto& option: EmulatorOptions::All()) {
		if (option.tab == tab) {
			AddRow(layout, option, parent);
		}
	}
	UpdateDependencies();
}

void EmulatorOptionsPanel::Load(const QVariantMap& values) {
	for (auto it = m_controls.constBegin(); it != m_controls.constEnd(); ++it) {
		const auto* option = EmulatorOptions::Find(it.key());
		if (option == nullptr) {
			continue;
		}
		const auto value = EmulatorOptions::Value(values, *option);
		switch (option->kind) {
			case Kind::Bool: static_cast<QCheckBox*>(it.value())->setChecked(value.toBool()); break;
			case Kind::Choice: static_cast<QComboBox*>(it.value())->setCurrentText(value.toString()); break;
			case Kind::Int: static_cast<QSpinBox*>(it.value())->setValue(value.toInt()); break;
		}
	}
	UpdateDependencies();
}

QVariantMap EmulatorOptionsPanel::Save() const {
	QVariantMap values;
	for (auto it = m_controls.constBegin(); it != m_controls.constEnd(); ++it) {
		const auto* option = EmulatorOptions::Find(it.key());
		if (option == nullptr) {
			continue;
		}
		switch (option->kind) {
			case Kind::Bool: values.insert(it.key(), static_cast<QCheckBox*>(it.value())->isChecked()); break;
			case Kind::Choice:
				values.insert(it.key(), static_cast<QComboBox*>(it.value())->currentText());
				break;
			case Kind::Int: values.insert(it.key(), static_cast<QSpinBox*>(it.value())->value()); break;
		}
	}
	return values;
}

// Options that only act with async pipelines are greyed out while it is off.
void EmulatorOptionsPanel::UpdateDependencies() {
	const auto* async = qobject_cast<QComboBox*>(m_controls.value(QStringLiteral("async_pipelines")));
	if (async == nullptr) {
		return;
	}
	const bool async_on = async->currentText() != QLatin1String("off");
	for (auto it = m_controls.constBegin(); it != m_controls.constEnd(); ++it) {
		const auto* option = EmulatorOptions::Find(it.key());
		if (option != nullptr && option->needs_async) {
			it.value()->setEnabled(async_on);
		}
	}
}
