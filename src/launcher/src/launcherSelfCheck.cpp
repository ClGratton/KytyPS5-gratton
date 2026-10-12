#include "launcherSelfCheck.h"

#include "common/emulatorConfig.h"
#include "configuration.h"
#include "emulatorArgs.h"
#include "configurationEditDialog.h"
#include "emulatorOptions.h"
#include "mainDialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QEventLoop>
#include <QSettings>
#include <QTextStream>

#include <cstdio>

namespace {

struct Report {
	int failures = 0;

	void Check(bool ok, const QString& what, const QString& detail = {}) {
		QTextStream out(stdout);
		out << (ok ? "PASS  " : "FAIL  ") << what;
		if (!ok && !detail.isEmpty()) {
			out << "  [" << detail << "]";
		}
		out << "\n";
		if (!ok) {
			failures++;
		}
	}
};

QString FindEmulator(const QStringList& args) {
	const auto at = args.indexOf(QStringLiteral("--emulator"));
	if (at >= 0 && at + 1 < args.size()) {
		return args.at(at + 1);
	}
	QDir dir(QCoreApplication::applicationDirPath());
	for (int i = 0; i < 2; i++) {
		const auto candidate = dir.absoluteFilePath(QStringLiteral("kyty_emulator"));
		if (QFile::exists(candidate)) {
			return candidate;
		}
		if (!dir.cdUp()) {
			break;
		}
	}
	return {};
}

// Runs the emulator with `args` plus --help (the parser validates every argument and exits 0
// without starting a game), returning its exit code and output.
int RunEmulator(const QString& emulator, const QStringList& args, QString* output) {
	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start(emulator, args + QStringList {QStringLiteral("--help")});
	if (!process.waitForFinished(30000)) {
		process.kill();
		*output = QStringLiteral("timeout");
		return -1;
	}
	*output = QString::fromUtf8(process.readAll());
	return process.exitCode();
}

// Flags and values of a bench config's "flags" list: "--flag" followed by an optional value.
QList<QPair<QString, QString>> ReadConfigFlags(const QString& path, QString* error) {
	QList<QPair<QString, QString>> flags;
	QFile                          file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		*error = QStringLiteral("cannot open ") + path;
		return flags;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	const auto list = doc.object().value(QStringLiteral("flags")).toArray();
	for (int i = 0; i < list.size(); i++) {
		const auto item = list.at(i).toString();
		if (!item.startsWith(QStringLiteral("--"))) {
			continue;
		}
		QString value;
		if (i + 1 < list.size() && !list.at(i + 1).toString().startsWith(QStringLiteral("--"))) {
			value = list.at(++i).toString();
		}
		flags.append({item, value});
	}
	return flags;
}

bool HasFlag(const QStringList& args, const QString& flag, const QString& value) {
	for (int i = 0; i < args.size(); i++) {
		if (args.at(i) != flag) {
			continue;
		}
		if (value.isEmpty()) {
			return true;
		}
		if (i + 1 < args.size() && args.at(i + 1).compare(value, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

// "on"/"off"/"true"/"false" are the same value to the emulator.
QString NormalizeBool(const QString& value) {
	const auto v = value.toLower();
	if (v == QLatin1String("on") || v == QLatin1String("1")) {
		return QStringLiteral("true");
	}
	if (v == QLatin1String("off") || v == QLatin1String("0")) {
		return QStringLiteral("false");
	}
	return v;
}

bool HasFlagNormalized(const QStringList& args, const QString& flag, const QString& value) {
	for (int i = 0; i < args.size(); i++) {
		if (args.at(i) != flag) {
			continue;
		}
		if (value.isEmpty()) {
			return true;
		}
		if (i + 1 < args.size() &&
		    NormalizeBool(args.at(i + 1)) == NormalizeBool(value)) {
			return true;
		}
	}
	return false;
}

QStringList ArgsWithoutGame(const Configuration& info) {
	auto args = CreateEmulatorArgs(info);
	const auto game = args.indexOf(QStringLiteral("--game"));
	if (game >= 0) {
		args.remove(game, 2);
	}
	const auto patch = args.indexOf(QStringLiteral("--game-patch"));
	if (patch >= 0) {
		args.remove(patch, 2);
	}
	return args;
}

void CheckTable(Report& r) {
	QStringList ids;
	bool        sane = true;
	QString     detail;
	for (const auto& option: EmulatorOptions::All()) {
		if (ids.contains(option.id)) {
			sane   = false;
			detail = QStringLiteral("duplicate id ") + option.id;
		}
		ids << option.id;
		bool       ok     = true;
		const auto parsed = EmulatorOptions::Parse(option, EmulatorOptions::ToText(option, option.def), &ok);
		if (!ok || parsed != option.def) {
			sane   = false;
			detail = QStringLiteral("default does not parse: ") + option.id;
		}
		if (option.kind == EmulatorOptions::Kind::Choice && !option.choices.contains(option.def.toString())) {
			sane   = false;
			detail = QStringLiteral("default not among the choices: ") + option.id;
		}
		if (option.kind == EmulatorOptions::Kind::Int &&
		    (option.def.toInt() < option.min || option.def.toInt() > option.max)) {
			sane   = false;
			detail = QStringLiteral("default out of range: ") + option.id;
		}
		if (option.tooltip.isEmpty() || option.label.isEmpty()) {
			sane   = false;
			detail = QStringLiteral("missing label or tooltip: ") + option.id;
		}
		if (option.flag.isEmpty() == option.env.isEmpty() && option.flag.isEmpty()) {
			sane   = false;
			detail = QStringLiteral("neither flag nor environment: ") + option.id;
		}
	}
	r.Check(sane, QStringLiteral("options table: %1 options, ids unique, defaults valid").arg(ids.size()),
	        detail);
}

// Every launcher setting that always reaches the emulator must start at the emulator's default.
void CheckDefaults(Report& r) {
	const Config::ConfigOptions emu;
	const Configuration         info;
	const auto                  resolution = EnumToText(info.screen_resolution).split('x');
	const auto check = [&r](bool ok, const char* name) {
		r.Check(ok, QStringLiteral("default: ") + QString::fromLatin1(name));
	};
	check(resolution.size() == 2 && resolution.at(0).toUInt() == emu.screen_width &&
	          resolution.at(1).toUInt() == emu.screen_height,
	      "screen resolution");
	check(info.user_name.toStdString() == emu.user_name, "user name");
	check(info.user_id == emu.user_id, "user id");
	check(static_cast<int>(info.present_mode) == static_cast<int>(emu.present_mode), "present mode");
	check(info.gpu_index == emu.gpu_index, "gpu index");
	check(info.fullscreen_enabled == emu.fullscreen_enabled, "fullscreen");
	check(info.hide_cursor_enabled == emu.hide_cursor_enabled, "hide cursor");
	check(info.readback_linear_images == emu.readback_linear_images, "readback linear images");
	check(info.sync_raw_image_buffers == emu.sync_raw_image_buffers, "sync raw image buffers");
	check(info.tessellation_enabled == emu.tessellation_enabled, "tessellation");
	check(info.trophy_enabled == emu.trophy_enabled, "trophy notifications");
	check(info.skip_notice_screen == emu.skip_notice_screen, "skip notice screen");
	check(static_cast<uint32_t>(info.vblank_frequency) == emu.vblank_frequency, "vblank frequency");
	check(static_cast<uint32_t>(info.fps_limit) == emu.fps_limit, "fps limit");
	check(info.gpu_time_scale_percent == static_cast<int>(emu.gpu_time_scale * 100.0 + 0.5),
	      "gpu time scale");
	check((info.gpu_timestamps == Configuration::GpuTimestamps::Gpu) ==
	          (emu.gpu_timestamps == Config::GpuTimestampMode::Gpu),
	      "gpu timestamps");
	check(static_cast<uint32_t>(info.console_language) == emu.console_language, "console language");
	check(info.vulkan_validation_enabled == emu.vulkan_validation_enabled, "vulkan validation");
	check(info.shader_validation_enabled == emu.shader_validation_enabled, "shader validation");
	check(static_cast<int>(info.shader_optimization_type) == static_cast<int>(emu.shader_optimization_type),
	      "shader optimization type");
	check(static_cast<int>(info.shader_log_direction) == static_cast<int>(emu.shader_log_direction),
	      "shader log direction");
	check(info.shader_log_folder.toStdString() == emu.shader_log_folder.string(), "shader log folder");
	check(info.command_buffer_dump_enabled == emu.command_buffer_dump_enabled, "command buffer dump");
	check(info.command_buffer_dump_folder.toStdString() == emu.command_buffer_dump_folder.string(),
	      "command buffer dump folder");
	check(static_cast<int>(info.printf_direction) == static_cast<int>(emu.printf_direction), "printf direction");
	check(info.printf_output_file.toStdString() == emu.printf_output_file.string(), "printf output file");
	check(info.profiler_enabled == emu.profiler_enabled, "profiler");
	check(info.renderdoc_enabled == emu.renderdoc_enabled, "renderdoc");
	check(info.amd_cpu_enabled == emu.amd_cpu_enabled, "amd cpu patch");
	check(static_cast<uint32_t>(info.controller.speaker_volume) == emu.controller_speaker_volume,
	      "controller speaker volume");
	check(static_cast<uint32_t>(info.controller.vibration_intensity) == emu.controller_vibration_intensity,
	      "controller vibration");
}

} // namespace

int RunSelfCheck(const QStringList& args) {
	Report r;
	CheckTable(r);
	CheckDefaults(r);

	Configuration defaults;
	Configuration preset;
	preset.ApplyRecommendedForPlay();
	const auto default_args = ArgsWithoutGame(defaults);
	const auto preset_args  = ArgsWithoutGame(preset);
	r.Check(default_args.contains(QStringLiteral("--async-pipelines")) &&
	            HasFlag(default_args, QStringLiteral("--async-pipelines"), QStringLiteral("off")),
	        QStringLiteral("default command line passes --async-pipelines off"));
	r.Check(HasFlag(preset_args, QStringLiteral("--async-pipelines"), QStringLiteral("stall")) &&
	            HasFlag(preset_args, QStringLiteral("--pipeline-lookahead"), QStringLiteral("true")),
	        QStringLiteral("preset command line passes stall + lookahead"));

	const auto emulator = FindEmulator(args);
	if (emulator.isEmpty()) {
		r.Check(false, QStringLiteral("emulator binary found next to the launcher or via --emulator"));
	} else {
		QTextStream(stdout) << "emulator: " << emulator << "\n";
		QString help;
		const int code = RunEmulator(emulator, {}, &help);
		r.Check(code == 0, QStringLiteral("emulator --help runs"), help.left(200));
		QStringList missing;
		for (const auto& option: EmulatorOptions::All()) {
			if (!option.flag.isEmpty() && !help.contains(option.flag + QLatin1Char(' ')) &&
			    !help.contains(option.flag + QLatin1Char('\n'))) {
				missing << option.flag;
			}
		}
		r.Check(missing.isEmpty(), QStringLiteral("every table flag is in the emulator's --help"),
		        missing.join(QLatin1Char(' ')));
		QString output;
		r.Check(RunEmulator(emulator, default_args, &output) == 0,
		        QStringLiteral("emulator accepts the default command line"), output.left(300));
		r.Check(RunEmulator(emulator, preset_args, &output) == 0,
		        QStringLiteral("emulator accepts the Recommended-for-play command line"),
		        output.left(300));
		// Each option at a non-default value must also parse.
		int rejected = 0;
		for (const auto& option: EmulatorOptions::All()) {
			if (option.flag.isEmpty()) {
				continue;
			}
			QVariantMap values;
			switch (option.kind) {
				case EmulatorOptions::Kind::Bool: values.insert(option.id, !option.def.toBool()); break;
				case EmulatorOptions::Kind::Choice:
					for (const auto& choice: option.choices) {
						if (choice != option.def.toString()) {
							values.insert(option.id, choice);
							break;
						}
					}
					break;
				case EmulatorOptions::Kind::Int: values.insert(option.id, option.max); break;
			}
			const bool controller = option.tab == EmulatorOptions::Tab::Controller;
			const auto option_args = EmulatorOptions::Args(values, controller);
			if (RunEmulator(emulator, option_args, &output) != 0) {
				rejected++;
				r.Check(false, QStringLiteral("emulator accepts %1 at a non-default value").arg(option.flag),
				        output.left(300));
			}
		}
		r.Check(rejected == 0, QStringLiteral("every flag parses at a non-default value"),
		        QStringLiteral("%1 rejected").arg(rejected));
	}

	const auto config_at = args.indexOf(QStringLiteral("--play-config"));
	if (config_at >= 0 && config_at + 1 < args.size()) {
		QString error;
		const auto flags = ReadConfigFlags(args.at(config_at + 1), &error);
		r.Check(!flags.isEmpty(), QStringLiteral("play config flags read"), error);
		QStringList missing;
		for (const auto& [flag, value]: flags) {
			if (!HasFlagNormalized(preset_args, flag, value)) {
				missing << (flag + (value.isEmpty() ? QString() : QStringLiteral(" ") + value));
			}
		}
		r.Check(missing.isEmpty(),
		        QStringLiteral("Recommended for play reproduces all %1 flags of %2")
		            .arg(flags.size())
		            .arg(QFileInfo(args.at(config_at + 1)).fileName()),
		        QStringLiteral("not reproduced: ") + missing.join(QStringLiteral(", ")));
	}

	QTextStream(stdout) << (r.failures == 0 ? "\nself-check OK\n" : "\nself-check FAILED\n");
	return r.failures == 0 ? 0 : 1;
}

int RunGrabSettings(const QStringList& args) {
	const auto dir_at = args.indexOf(QStringLiteral("--grab-settings"));
	if (dir_at < 0 || dir_at + 1 >= args.size()) {
		QTextStream(stderr) << "usage: launcher --grab-settings <dir> [--global] [--preset]\n";
		return 2;
	}
	const QDir out_dir(args.at(dir_at + 1));
	QDir().mkpath(out_dir.absolutePath());
	Configuration info;
	info.name = QStringLiteral("Grab");
	ConfigurationEditDialog dialog(info);
	if (args.contains(QStringLiteral("--global"))) {
		dialog.SetGlobalSettings({});
	}
	if (args.contains(QStringLiteral("--preset"))) {
		for (auto* button: dialog.findChildren<QPushButton*>()) {
			if (button->text() == QObject::tr("Recommended for play")) {
				button->click();
			}
		}
	}
	dialog.resize(1000, 700);
	dialog.show();
	auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settings_tabs"));
	if (tabs == nullptr) {
		return 1;
	}
	for (int i = 0; i < tabs->count(); i++) {
		tabs->setCurrentIndex(i);
		QCoreApplication::processEvents();
		const auto file = out_dir.filePath(QStringLiteral("%1-%2.png").arg(i).arg(tabs->tabText(i).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"))));
		dialog.grab().save(file);
		QTextStream(stdout) << file << "\n";
	}
	return 0;
}

int RunConfigure(const QStringList& args) {
	const auto dir_at = args.indexOf(QStringLiteral("--game-dir"));
	const bool preset = args.contains(QStringLiteral("--preset"));
	if (dir_at < 0 && !preset) {
		QTextStream(stderr) << "usage: launcher --configure [--game-dir <dir>] [--preset]\n";
		return 2;
	}
	// The same file and keys as the launcher's own settings (ConfigurationListWidget).
	QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Kyty"),
	            QStringLiteral("Kyty"));
	if (dir_at >= 0 && dir_at + 1 < args.size()) {
		const auto dir = QDir::cleanPath(QDir(args.at(dir_at + 1)).absolutePath());
		s.beginGroup(QStringLiteral("Launcher"));
		auto dirs = s.value(QStringLiteral("game_dirs")).toStringList();
		if (!dirs.contains(dir)) {
			dirs << dir;
		}
		s.setValue(QStringLiteral("game_dirs"), dirs);
		s.endGroup();
	}
	if (preset) {
		Configuration info;
		s.beginGroup(QStringLiteral("GlobalConfiguration"));
		if (!s.childKeys().isEmpty()) {
			info.ReadSettings(&s);
		}
		info.controller.ReadSettings(&s);
		s.endGroup();
		info.ApplyRecommendedForPlay();
		s.remove(QStringLiteral("GlobalConfiguration"));
		s.beginGroup(QStringLiteral("GlobalConfiguration"));
		info.WriteSettings(&s);
		info.controller.WriteSettings(&s);
		s.endGroup();
	}
	s.sync();
	QTextStream(stdout) << "settings written to " << s.fileName() << "\n";
	return s.status() == QSettings::NoError ? 0 : 1;
}

int RunGrabMain(const QStringList& args) {
	const auto png_at = args.indexOf(QStringLiteral("--grab-main"));
	if (png_at < 0 || png_at + 1 >= args.size()) {
		QTextStream(stderr) << "usage: launcher --grab-main <png>\n";
		return 2;
	}
	MainDialog window;
	window.emit Start();
	window.resize(1100, 700);
	window.show();
	// The game folders are scanned on start; give the scan (and the title lookups) time to finish.
	QEventLoop loop;
	QTimer::singleShot(4000, &loop, &QEventLoop::quit);
	loop.exec();
	QTextStream out(stdout);
	if (auto* list = window.findChild<QTreeWidget*>(QStringLiteral("cfgs_list")); list != nullptr) {
		out << "games listed: " << list->topLevelItemCount() << "\n";
		for (int i = 0; i < list->topLevelItemCount(); i++) {
			const auto* item = list->topLevelItem(i);
			out << "  " << item->text(0) << " | " << item->text(1) << " | " << item->text(5) << "\n";
		}
	}
	window.grab().save(args.at(png_at + 1));
	out << "saved " << args.at(png_at + 1) << "\n";
	out.flush();
	// The window's destructor waits for background loaders (compatibility list, trophies); a
	// diagnostic run does not need them to finish.
	std::fflush(stdout);
	std::_Exit(0);
}

int RunPrintCommand(const QStringList& args) {
	const auto game_at = args.indexOf(QStringLiteral("--game"));
	if (game_at < 0 || game_at + 1 >= args.size()) {
		QTextStream(stderr) << "usage: launcher --print-command [--shell] [--preset] --game <dir>\n";
		return 2;
	}
	Configuration info;
	if (args.contains(QStringLiteral("--preset"))) {
		info.ApplyRecommendedForPlay();
	} else {
		QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Kyty"),
		            QStringLiteral("Kyty"));
		s.beginGroup(QStringLiteral("GlobalConfiguration"));
		if (!s.childKeys().isEmpty()) {
			info.ReadSettings(&s);
		}
		info.controller.ReadSettings(&s);
		s.endGroup();
	}
	info.basedir = QDir(args.at(game_at + 1)).absolutePath();
	const auto command = CreateEmulatorArgs(info);
	QTextStream out(stdout);
	const auto quote = [](QString value) {
		value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
		return QStringLiteral("'") + value + QStringLiteral("'");
	};
	if (args.contains(QStringLiteral("--shell"))) {
		// For `eval "$(launcher --print-command --shell ...)"`: the exact command the Run button starts.
		for (const auto& assignment: CreateEmulatorEnvironment(info)) {
			const auto split = assignment.indexOf(QLatin1Char('='));
			out << "export " << assignment.left(split) << "=" << quote(assignment.mid(split + 1)) << "\n";
		}
		out << "exec " << quote(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("kyty_emulator")));
		for (const auto& arg: command) {
			out << " " << quote(arg);
		}
		out << "\n";
		return 0;
	}
	for (const auto& assignment: CreateEmulatorEnvironment(info)) {
		out << assignment << "\n";
	}
	out << "kyty_emulator";
	for (const auto& arg: command) {
		out << " " << (arg.contains(QLatin1Char(' ')) ? quote(arg) : arg);
	}
	out << "\n";
	return 0;
}
