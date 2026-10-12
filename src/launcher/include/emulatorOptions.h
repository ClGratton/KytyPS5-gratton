#ifndef LAUNCHER_INCLUDE_EMULATOR_OPTIONS_H_
#define LAUNCHER_INCLUDE_EMULATOR_OPTIONS_H_

#include "common/emulatorConfig.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <functional>

// The emulator options the launcher exposes beyond the classic settings, as one table. Each entry
// names its emulator flag, its tab and section, its control and its tooltip, and takes its default
// from a default-constructed Config::ConfigOptions, so the launcher's default is the emulator's
// default by construction (launcher --self-check also verifies the flags against the binary's
// --help). Values live in a QVariantMap keyed by id; a missing key means the default.
namespace EmulatorOptions {

enum class Tab { Display, Shaders, Gpu, Memory, Controller, Debug };

enum class Kind { Bool, Choice, Int };

struct Option {
	QString     id;      // settings key suffix and map key
	QString     flag;    // emulator flag; empty = launcher only
	Tab         tab;
	QString     section; // group box title inside the tab
	Kind        kind;
	QString     label;
	QString     tooltip;
	QVariant    def;     // bool / QString / int; the emulator's own default
	QStringList choices; // Kind::Choice: the values the emulator takes
	int         min          = 0;
	int         max          = 0;
	QString     special;           // Kind::Int: text for the minimum ("Auto", "Off")
	QString     suffix;            // Kind::Int: unit
	bool        needs_async  = false; // only has an effect with --async-pipelines stall|skip
	bool        flag_if_off  = false; // bare flag when off (--no-splash), instead of "--flag true|false"
	QString     env;               // launcher-only: environment variable set to 1 when on
};

[[nodiscard]] const QList<Option>& All();
[[nodiscard]] const Option*        Find(const QString& id);

// Stored value or the default, coerced to the option's type.
[[nodiscard]] QVariant Value(const QVariantMap& values, const Option& option);
// Text of a value as stored in settings and game-config files, and the way back. Parse returns the
// default and sets ok = false when the text is not a valid value.
[[nodiscard]] QString  ToText(const Option& option, const QVariant& value);
[[nodiscard]] QVariant Parse(const Option& option, const QVariant& raw, bool* ok = nullptr);

// Flags for the emulator command line. Booleans are always explicit, so a launcher setting wins
// over a future change of the emulator default; integers are passed when they differ from the
// default (0 usually means "automatic").
// The Controller tab's options are stored with the global controller settings, so they are
// passed separately (controller = true selects them, false selects all the others).
[[nodiscard]] QStringList Args(const QVariantMap& values, bool controller = false);
// Environment assignments ("NAME=value") for the launched emulator (the MangoHud overlay).
[[nodiscard]] QStringList Environment(const QVariantMap& values);

// "Recommended for play": the options of bench/config-play-next.json (the measured play set).
[[nodiscard]] QVariantMap Recommended();

} // namespace EmulatorOptions

#endif /* LAUNCHER_INCLUDE_EMULATOR_OPTIONS_H_ */
