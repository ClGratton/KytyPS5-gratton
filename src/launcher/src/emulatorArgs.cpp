#include "emulatorArgs.h"

#include "cheatFile.h"
#include "configuration.h"
#include "emulatorOptions.h"
#include "gameContent.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

static QString BoolArg(bool value) {
	return value ? QStringLiteral("true") : QStringLiteral("false");
}

// The overlay settings file, installed next to the emulator (src/launcher/mangohud/mangohud.conf.in).
static const QString MANGOHUD_CONFIG_FILE = QStringLiteral("mangohud.conf");

QStringList CreateEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = EnumToText(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	if (!info.controller.color.isEmpty()) {
		args << "--controller-color" << info.controller.color;
	}
	args << "--controller-volume" << QString::number(info.controller.speaker_volume);
	args << "--controller-vibration" << QString::number(info.controller.vibration_intensity);
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	if (info.fullscreen_enabled) {
		args << "--fullscreen";
	}
	if (info.hide_cursor_enabled) {
		args << "--hide-cursor";
	}
	args << "--readback-linear-images" << BoolArg(info.readback_linear_images);
	args << "--sync-raw-image-buffers" << BoolArg(info.sync_raw_image_buffers);
	args << "--trophy-notifications" << BoolArg(info.trophy_enabled);
	args << "--skip-notice-screen" << BoolArg(info.skip_notice_screen);
	if (info.tessellation_enabled) {
		args << "--tessellation";
	}
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	if (info.fps_limit > 0) {
		args << "--fps-limit" << QString::number(info.fps_limit);
	}
	if (info.gpu_time_scale_percent != 100) {
		args << "--gpu-time-scale" << QString::number(info.gpu_time_scale_percent / 100.0, 'f', 2);
	}
	// Always explicit: the emulator default is gpu, and Legacy is a choice that must be passed.
	args << "--gpu-timestamps"
	     << (info.gpu_timestamps == Configuration::GpuTimestamps::Gpu ? "gpu" : "legacy");
	args << "--console-language" << QString::number(info.console_language);
	args << "--vulkan-validation" << BoolArg(info.vulkan_validation_enabled);
	args << "--shader-validation" << BoolArg(info.shader_validation_enabled);
	args << "--shader-optimization-type" << EnumToText(info.shader_optimization_type);
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	args << "--command-buffer-dump" << BoolArg(info.command_buffer_dump_enabled);
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	if (info.profiler_enabled) {
		args << "--profile";
	}
	args << "--spirv-debug-printf" << "false";
	if (info.amd_cpu_enabled) {
		args << "--amd-cpu";
	}
#if defined(_WIN32)
	if (info.red_zone_protection_enabled) {
		args << "--redzone";
	}
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	if (info.renderdoc_enabled) {
		args << "--rd";
	}

	// Pad speaker (global controller settings), then every table option of the other tabs.
	args << EmulatorOptions::Args(info.controller.options, true);
	args << EmulatorOptions::Args(info.emulator_options, false);

	QString game = info.basedir;
	if (!info.elf.isEmpty() && !GameContent::IsArchive(info.basedir)) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;

	const auto patch_plan = Cheats::PlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

QStringList CreateEmulatorEnvironment(const Configuration& info, const QString& install_dir) {
	auto env = EmulatorOptions::Environment(info.emulator_options);
	// MangoHud reads its settings from the file named by MANGOHUD_CONFIGFILE. MANGOHUD_CONFIG would
	// replace the file with a settings string, so it is not used. The file is installed next to the
	// emulator (src/launcher/mangohud/mangohud.conf.in).
	if (env.contains(QStringLiteral("MANGOHUD=1"))) {
		env << QStringLiteral("MANGOHUD_CONFIGFILE=") +
		           QDir(install_dir).filePath(MANGOHUD_CONFIG_FILE);
	}
	return env;
}
