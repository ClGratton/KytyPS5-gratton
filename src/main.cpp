#include <cstdlib>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include "common/archive.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "kytyGitVersion.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <magic_enum.hpp>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler = Debug::GetCompiler() + "-" + Debug::GetLinker();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("kyty_emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf("  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf("  --controller-volume <0-100>         DualSense speaker volume. Default: 50.\n");
	::printf("  --controller-vibration <0-100>      DualSense vibration intensity. Default: 100.\n");
	::printf("  --pad-speaker <on|off>              Send controller-speaker audio to the DualSense; off plays\n"
	         "                                      it on the normal output. Default: on.\n");
	::printf("  --pad-speaker-volume <0-100>        DualSense speaker hardware level. Default: 15.\n");
	::printf("  --pad-speaker-os-sink <on|off>      Linux: expose a Bluetooth DualSense speaker as an OS\n"
	         "                                      volume control (PipeWire/Pulse sink). Default: on.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf(
	    "  --hide-cursor                        Hide the cursor after 2 s idle. Default: off.\n");
	::printf("  --no-splash                          Disable the startup splash screen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --amd-cpu                            Apply AMD CPU instruction patches.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --gpu-timestamps <legacy|gpu>        Source of the GPU timestamps a game reads: the clock\n"
	         "                                       when a packet is parsed (legacy), or the host GPU's\n"
	         "                                       own timestamp of that point (gpu). Default: legacy.\n");
	::printf("  --gpu-time-scale <0.1-16>            Multiply the GPU durations a game observes. Games with\n"
	         "                                       dynamic resolution render lower above 1.0 and higher\n"
	         "                                       below it. Default: 1.0.\n");
	::printf("  --fps-limit <0-1000>                 Present at most this many frames per second; 0 is off.\n"
	         "                                       Capped by --vblank-frequency. Default: 0.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profile                            Enable the Tracy profiler.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf(
	    "  --readback-linear-images <true|false> Read back writable linear images on submit.\n");
	::printf(
	    "  --sync-raw-image-buffers <true|false> Synchronize raw reads of GPU images. Default: false.\n");
	::printf(
	    "  --trophy-notifications <true|false>   Show trophy unlock toasts and play their sound.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf(
	    "  --skip-notice-screen <true|false>    Skip startup logos and notices in supported games.\n"
	    "                                      Default: false.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf(
	    "  --vram-paging <on|off>               Use VK_EXT_memory_priority and\n"
	    "                                      VK_EXT_pageable_device_local_memory so the driver\n"
	    "                                      pages low-priority VRAM out instead of failing.\n"
	    "                                      Default: off.\n");
	::printf("  --vram-stats <on|off>                Log VRAM heap usage and its split every 5 s.\n"
	         "                                      Implied by --vram-paging. Default: off.\n");
	::printf("  --mem-stats <on|off>                 Log guest memory release/unmap/decommit counters and\n"
	         "                                      how much released memory is still resident, every 5 s\n"
	         "                                      and on exit. Default: off.\n");
	::printf("  --mem-release-punch <on|off>         Return guest memory the game released (and no view\n"
	         "                                      maps) to the host with fallocate(PUNCH_HOLE). Whole\n"
	         "                                      host pages only. Default: off.\n");
	::printf("  --vram-audit <seconds>               Debug: once, this many seconds after start, log the\n"
	         "                                      resident image mix and content-duplicate textures.\n");
	::printf("  --vram-ballast-mb <MiB>              Debug: hold this much device-local memory to\n"
	         "                                      simulate a smaller GPU. Default: 0.\n");
	::printf("  --gc-texture-trigger-mb <MiB>        Texture GC starts above this VRAM usage (0 = auto).\n"
	         "  --gc-texture-pressure-mb <MiB>       Texture GC evicts harder above this (0 = auto).\n"
	         "  --gc-texture-critical-mb <MiB>       Texture GC is aggressive above this (0 = auto).\n"
	         "  --gc-buffer-trigger-mb <MiB>         Buffer GC starts above this (0 = auto).\n"
	         "  --gc-buffer-critical-mb <MiB>        Buffer GC is aggressive above this (0 = auto).\n"
	         "  --gc-buffer-own-share <percent>      Buffer GC also needs the buffer cache itself to hold\n"
	         "                                       this percentage of the critical budget before it\n"
	         "                                       downloads dirty buffers (each batch is a GPU drain);\n"
	         "                                       the texture cache can keep the whole device above the\n"
	         "                                       mark on its own. 0 = stock. Default: 0.\n");
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
	::printf("  --bench-frames-csv <path>            Append \"frame_idx,t_ns\" (steady_clock) per presented\n"
	         "                                       frame; buffered, flushed at exit. Default: off.\n");
	::printf("  --bench-exit-after-frames <N>        Cleanly exit after N presented frames. Default: off.\n");
	::printf("  --pipeline-cache-key <value>         revision (default) or schema. schema keys the driver\n"
	         "                                       pipeline cache on a schema version + GPU/driver ids\n"
	         "                                       instead of the git revision, and enables it in\n"
	         "                                       non-Release/dirty builds.\n");
	::printf("  --shader-timing-log <path>           Append CSV timing rows for shader translation and\n"
	         "                                       pipeline creation misses. Default: off.\n");
	::printf("  --bench-gpu-trace <true|false>       With --shader-timing-log: also log the guest submit /\n"
	         "                                       GPU-thread execute timeline. Default: off.\n");
	::printf("  --async-pipelines <off|stall|skip>   Run shader translation and Vulkan pipeline creation\n"
	         "                                       on worker threads. off (default): synchronous, stock\n"
	         "                                       behaviour. stall: the draw/dispatch waits for the\n"
	         "                                       worker. skip: graphics draws whose program or pipeline\n"
	         "                                       is not ready are skipped (compute always waits).\n");
	::printf("  --pipeline-lookahead <true|false>    Prefetch shader programs and pipelines from submitted\n"
	         "                                       command buffers into the worker pool (needs\n"
	         "                                       --async-pipelines stall|skip). Default: false.\n");
	::printf("  --pipelines-unoptimized-first <true|false>\n"
	         "                                       A pipeline no cache holds is created unoptimized so\n"
	         "                                       its draw does not wait, then rebuilt optimized on a\n"
	         "                                       background worker and swapped in (needs\n"
	         "                                       --async-pipelines stall|skip; NVIDIA only, other GPUs\n"
	         "                                       need KYTY_PIPELINE_FAST_BUILD=1). Default: false.\n");
	::printf("  --pipeline-workers <1-64>            Worker threads for --async-pipelines.\n"
	         "                                       Default: min(8, hardware threads / 2).\n");
	::printf("  --debug-texture-gc <true|false|log>  Debug: false disables the texture garbage collector;\n"
	         "                                       log logs every free of a GPU-written image with\n"
	         "                                       the flags that decided it. Default: true.\n");
	::printf("  --gpu-phase-stats <seconds>          Every N seconds print where the GPU thread spends its\n"
	         "                                       time (phase shares, submits per frame, drains by\n"
	         "                                       cause, slowest PM4 packets) and write\n"
	         "                                       _gpu_phase.csv. For shares, never an fps baseline.\n"
	         "                                       Default: 0 (off).\n");
	::printf("  --gc-clock <runs|frames>             Age the texture and buffer caches in collector runs\n"
	         "                                       (one per guest submission, stock) or in presented\n"
	         "                                       frames. Default: runs.\n");
	::printf("  --submit-stats <seconds>             Every N seconds print frames, Vulkan submits per frame\n"
	         "                                       and the --submit-coalesce counters (no timers; safe\n"
	         "                                       next to fps measurements). Default: 0 (off).\n");
	::printf("  --bda-sync-skip <true|false>         Skip the all-buffer synchronization before draws that\n"
	         "                                       address memory through raw pointers while no page\n"
	         "                                       became CPU-modified and no buffer or mapping changed\n"
	         "                                       since the last full pass. Default: false.\n");
	::printf("  --submit-coalesce <true|false>       Hold back the Vulkan submit of RELEASE_MEM end-of-pipe\n"
	         "                                       events: submit after --submit-coalesce-max events or\n"
	         "                                       when the oldest is older than --submit-coalesce-us,\n"
	         "                                       and at every drain or end of submission. Default: false.\n");
	::printf("  --elide-barriers <true|false>        Leave out a guest cache-flush/partial-flush global memory\n"
	         "                                       barrier when nothing was recorded since the previous\n"
	         "                                       one in the same command buffer. Default: false.\n");
	::printf("  --known-fills <true|false>           Remember ranges last written by one GPU uniform fill so\n"
	         "                                       color-metadata clear discovery needs no GPU->CPU\n"
	         "                                       readback for them. Default: false.\n");
	::printf("  --clean-guest-reads <true|false>     The GPU thread reads shader metadata and vertex tables from\n"
	         "                                       the guest backing without faulting when none of the bytes\n"
	         "                                       is GPU-written (no drain). Default: false.\n");
	::printf("  --readback-queue <true|false>        Synchronous GPU->guest buffer readbacks copy on a second\n"
	         "                                       queue of the rendering family and wait only for the\n"
	         "                                       work that wrote the data. Default: false.\n");
	::printf("  --submit-coalesce-max <1-256>        Events held back at most. Default: 16.\n");
	::printf("  --submit-coalesce-us <0-20000>       Age bound in microseconds; 0 derives it from the\n"
	         "                                       vblank period (period / 8). Default: 0.\n");
	::printf("  --skip-ir-validation <true|false>    Skip the shader IR consistency checks in release\n"
	         "                                       translations (cuts translate time; output is\n"
	         "                                       identical). KYTY_IR_VALIDATE=1 forces them on.\n"
	         "                                       Default: false.\n");
	::printf("  --debug-lookahead-verify <true|false> Debug: recompile every prefetched program before\n"
	         "                                       its first use and compare the SPIR-V. Default: false.\n");
	::printf("  --pipeline-lookahead-kinds <all|graphics|compute|none>\n"
	         "                                       Debug: restrict --pipeline-lookahead to draws or\n"
	         "                                       dispatches. Default: all.\n");
	::printf("  --debug-hang-watchdog <seconds>      Debug: when no frame was presented for this long,\n"
	         "                                       write a thread / worker pool / lookahead report to\n"
	         "                                       stderr and _hang_watchdog.txt. Default: 0 (off).\n");
	::printf("  --debug-gpu-ops <off|ring|checkpoints>\n"
	         "                                       Debug: keep a host-side ring of the last draws and\n"
	         "                                       dispatches and print the ones of unfinished\n"
	         "                                       submissions when the GPU stops answering (hang\n"
	         "                                       report, a wait of 5+ s, device loss). checkpoints\n"
	         "                                       also drops VK_NV_device_diagnostic_checkpoints\n"
	         "                                       markers. Default: off.\n");
	::printf("  --bench-render-csv <path>            Append one row per guest flip with the sizes of the\n"
	         "                                       render passes since the last flip. Default: off.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

static bool ParseBool(const std::string& value, bool& out) {
	if (Common::EqualNoCase(value, "true") || value == "1" || Common::EqualNoCase(value, "yes") ||
	    Common::EqualNoCase(value, "on")) {
		out = true;
		return true;
	}

	if (Common::EqualNoCase(value, "false") || value == "0" || Common::EqualNoCase(value, "no") ||
	    Common::EqualNoCase(value, "off")) {
		out = false;
		return true;
	}

	return false;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseGpuTimestampMode(const std::string& value, Config::GpuTimestampMode& out) {
	if (Common::EqualNoCase(value, "legacy")) {
		out = Config::GpuTimestampMode::Legacy;
		return true;
	}
	if (Common::EqualNoCase(value, "gpu")) {
		out = Config::GpuTimestampMode::Gpu;
		return true;
	}
	return false;
}

static bool ParseGpuTimeScale(const std::string& value, double& out) {
	char*  end    = nullptr;
	double number = std::strtod(value.c_str(), &end);
	if (value.empty() || end != value.c_str() + value.size() || !std::isfinite(number) ||
	    number < Config::MIN_GPU_TIME_SCALE || number > Config::MAX_GPU_TIME_SCALE) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUint64(const std::string& value, uint64_t& out) {
	uint64_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

static bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

		if (arg == "--fullscreen") {
			options.config.fullscreen_enabled = true;
			continue;
		}

		if (arg == "--hide-cursor") {
			options.config.hide_cursor_enabled = true;
			continue;
		}

		if (arg == "--no-splash") {
			options.config.splash_enabled = false;
			continue;
		}

		if (arg == "--vr") {
			options.config.vr_enabled = true;
			continue;
		}

		if (arg == "--amd-cpu") {
			options.config.amd_cpu_enabled = true;
			continue;
		}

		if (arg == "--playgo-hack") {
			options.config.playgo_hack_enabled = true;
			continue;
		}

		if (arg == "--tessellation") {
			options.config.tessellation_enabled = true;
			continue;
		}

		if (arg == "--profile") {
			options.config.profiler_enabled = true;
			continue;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (arg == "--redzone") {
			options.config.red_zone_protection_enabled = true;
			continue;
		}
#endif

		if (!arg.starts_with("--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (Common::File::IsDirectoryExisting(path)) {
				options.app0_dir = path;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
				const auto root = Common::MakeArchivePath(path);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Archive does not contain eboot.bin: %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(path)) {
				options.app0_dir = path.parent_path();

				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}

				options.elf = std::filesystem::path("/app0") / path.filename();
			} else {
				::printf("--game must point to an existing directory, ELF, or archive: %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.assign(argv + i + 1, argv + argc);
			break;
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (!Common::File::IsFileExisting(path)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = path;
		} else if (arg == "--screen-width") {
			if (!ParseUint32(value, options.config.screen_width) ||
			    options.config.screen_width == 0) {
				::printf("invalid screen width: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--screen-height") {
			if (!ParseUint32(value, options.config.screen_height) ||
			    options.config.screen_height == 0) {
				::printf("invalid screen height: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			Config::ControllerColor color {};
			if (!ParseControllerColor(value, color)) {
				::printf("invalid controller color (expected #RRGGBB): %s\n", value.c_str());
				return false;
			}
			options.config.controller_color = color;
		} else if (arg == "--controller-volume") {
			if (!ParseUint32(value, options.config.controller_speaker_volume) ||
			    options.config.controller_speaker_volume > 100) {
				::printf("invalid controller volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--controller-vibration") {
			if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
			    options.config.controller_vibration_intensity > 100) {
				::printf("invalid controller vibration intensity: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pad-speaker") {
			if (!ParseBool(value, options.config.pad_speaker_enabled)) {
				::printf("invalid pad speaker value (expected on|off): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pad-speaker-volume") {
			if (!ParseUint32(value, options.config.pad_speaker_volume) ||
			    options.config.pad_speaker_volume > 100) {
				::printf("invalid pad speaker volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pad-speaker-os-sink") {
			if (!ParseBool(value, options.config.pad_speaker_os_sink)) {
				::printf("invalid pad speaker OS sink value (expected on|off): %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			if (!ParseInt32(value, options.config.gpu_index)) {
				::printf("invalid gpu index: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vblank-frequency") {
			if (!ParseUint32(value, options.config.vblank_frequency)) {
				::printf("invalid vblank frequency: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu-timestamps") {
			if (!ParseGpuTimestampMode(value, options.config.gpu_timestamps)) {
				::printf("invalid --gpu-timestamps (legacy or gpu): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu-time-scale") {
			if (!ParseGpuTimeScale(value, options.config.gpu_time_scale)) {
				::printf("invalid --gpu-time-scale (%.1f to %.0f): %s\n",
				         Config::MIN_GPU_TIME_SCALE, Config::MAX_GPU_TIME_SCALE, value.c_str());
				return false;
			}
		} else if (arg == "--fps-limit") {
			if (!ParseUint32(value, options.config.fps_limit) ||
			    options.config.fps_limit > Config::MAX_FPS_LIMIT) {
				::printf("invalid --fps-limit (0 to %u): %s\n", Config::MAX_FPS_LIMIT,
				         value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-assisted-validation") {
			if (!ParseBool(value, options.config.gpu_assisted_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-validation") {
			if (!ParseBool(value, options.config.shader_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = Common::PathFromUtf8(value);
		} else if (arg == "--command-buffer-dump") {
			if (!ParseBool(value, options.config.command_buffer_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
		} else if (arg == "--graphics-debug-dump") {
			if (!ParseBool(value, options.config.graphics_debug_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--spirv-debug-printf") {
			if (!ParseBool(value, options.config.spirv_debug_printf_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-linear-images") {
			if (!ParseBool(value, options.config.readback_linear_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--sync-raw-image-buffers") {
			if (!ParseBool(value, options.config.sync_raw_image_buffers)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--trophy-notifications") {
			if (!ParseBool(value, options.config.trophy_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--vram-stats") {
			if (!ParseBool(value, options.config.vram_stats)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--mem-stats") {
			if (!ParseBool(value, options.config.mem_stats)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--mem-release-punch") {
			if (!ParseBool(value, options.config.mem_release_punch)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--vram-audit") {
			if (!ParseUint32(value, options.config.vram_audit_s)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--vram-ballast-mb") {
			if (!ParseUint32(value, options.config.vram_ballast_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-texture-trigger-mb") {
			if (!ParseUint32(value, options.config.gc_texture_trigger_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-texture-pressure-mb") {
			if (!ParseUint32(value, options.config.gc_texture_pressure_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-texture-critical-mb") {
			if (!ParseUint32(value, options.config.gc_texture_critical_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-buffer-trigger-mb") {
			if (!ParseUint32(value, options.config.gc_buffer_trigger_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-buffer-critical-mb") {
			if (!ParseUint32(value, options.config.gc_buffer_critical_mb)) {
				::printf("invalid value for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gc-buffer-own-share") {
			if (!ParseUint32(value, options.config.gc_buffer_own_share_pct) ||
			    options.config.gc_buffer_own_share_pct > 100) {
				::printf("invalid value for %s (0..100): %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--vram-paging") {
			if (!ParseBool(value, options.config.vram_paging)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--skip-notice-screen") {
			if (!ParseBool(value, options.config.skip_notice_screen)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--bench-frames-csv") {
			if (value.empty()) {
				::printf("invalid --bench-frames-csv path\n");
				return false;
			}
			options.config.bench_frames_csv = Common::PathFromUtf8(value);
		} else if (arg == "--pipeline-cache-key") {
			if (Common::EqualNoCase(value, "revision")) {
				options.config.pipeline_cache_key = Config::PipelineCacheKey::Revision;
			} else if (Common::EqualNoCase(value, "schema")) {
				options.config.pipeline_cache_key = Config::PipelineCacheKey::Schema;
			} else {
				::printf("invalid pipeline cache key (expected revision or schema): %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--async-pipelines") {
			if (Common::EqualNoCase(value, "off")) {
				options.config.async_pipelines = Config::AsyncPipelineMode::Off;
			} else if (Common::EqualNoCase(value, "stall")) {
				options.config.async_pipelines = Config::AsyncPipelineMode::Stall;
			} else if (Common::EqualNoCase(value, "skip")) {
				options.config.async_pipelines = Config::AsyncPipelineMode::Skip;
			} else {
				::printf("invalid async pipelines mode (expected off, stall or skip): %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--pipeline-lookahead") {
			if (!ParseBool(value, options.config.pipeline_lookahead)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--debug-texture-gc") {
			if (Common::EqualNoCase(value, "log")) {
				options.config.texture_gc_enabled = true;
				options.config.texture_gc_log     = true;
			} else if (!ParseBool(value, options.config.texture_gc_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-phase-stats") {
			uint64_t seconds = 0;
			if (!ParseUint64(value, seconds) || seconds > 3600) {
				::printf("invalid gpu phase stats interval (expected 0-3600 seconds): %s\n",
				         value.c_str());
				return false;
			}
			options.config.gpu_phase_stats_s = static_cast<uint32_t>(seconds);
		} else if (arg == "--gc-clock") {
			if (Common::EqualNoCase(value, "runs")) {
				options.config.gc_clock_frames = false;
			} else if (Common::EqualNoCase(value, "frames")) {
				options.config.gc_clock_frames = true;
			} else {
				::printf("invalid gc clock (expected runs or frames): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--bda-sync-skip") {
			if (!ParseBool(value, options.config.bda_sync_skip)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--submit-coalesce") {
			if (!ParseBool(value, options.config.submit_coalesce)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--elide-barriers") {
			if (!ParseBool(value, options.config.elide_barriers)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--known-fills") {
			if (!ParseBool(value, options.config.known_fills)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--clean-guest-reads") {
			if (!ParseBool(value, options.config.clean_guest_reads)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-queue") {
			if (!ParseBool(value, options.config.readback_queue)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--submit-coalesce-max") {
			uint64_t events = 0;
			if (!ParseUint64(value, events) || events == 0 || events > 256) {
				::printf("invalid submit coalesce max (expected 1-256): %s\n", value.c_str());
				return false;
			}
			options.config.submit_coalesce_max = static_cast<uint32_t>(events);
		} else if (arg == "--submit-coalesce-us") {
			uint64_t us = 0;
			if (!ParseUint64(value, us) || us > 20000) {
				::printf("invalid submit coalesce age (expected 0-20000 us): %s\n", value.c_str());
				return false;
			}
			options.config.submit_coalesce_us = static_cast<uint32_t>(us);
		} else if (arg == "--submit-stats") {
			uint64_t seconds = 0;
			if (!ParseUint64(value, seconds) || seconds > 3600) {
				::printf("invalid submit stats interval (expected 0-3600 seconds): %s\n",
				         value.c_str());
				return false;
			}
			options.config.submit_stats_s = static_cast<uint32_t>(seconds);
		} else if (arg == "--pipelines-unoptimized-first") {
			if (!ParseBool(value, options.config.pipelines_unoptimized_first)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--skip-ir-validation") {
			if (!ParseBool(value, options.config.skip_ir_validation)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--debug-lookahead-verify") {
			if (!ParseBool(value, options.config.lookahead_verify)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--pipeline-lookahead-kinds") {
			if (Common::EqualNoCase(value, "all")) {
				options.config.pipeline_lookahead_kinds = 3;
			} else if (Common::EqualNoCase(value, "graphics")) {
				options.config.pipeline_lookahead_kinds = 1;
			} else if (Common::EqualNoCase(value, "compute")) {
				options.config.pipeline_lookahead_kinds = 2;
			} else if (Common::EqualNoCase(value, "programs")) {
				options.config.pipeline_lookahead_kinds = 3 | 4; // programs only, no pipelines
			} else if (Common::EqualNoCase(value, "compute-programs")) {
				options.config.pipeline_lookahead_kinds = 2 | 4;
			} else if (Common::EqualNoCase(value, "graphics-programs")) {
				options.config.pipeline_lookahead_kinds = 1 | 4;
			} else if (Common::EqualNoCase(value, "none")) {
				options.config.pipeline_lookahead_kinds = 0; // shadow decode only
			} else {
				::printf("invalid lookahead kinds (expected all, graphics, compute, programs, compute-programs, graphics-programs or none): %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--debug-hang-watchdog") {
			uint64_t seconds = 0;
			if (!ParseUint64(value, seconds) || seconds > 3600) {
				::printf("invalid watchdog seconds (expected 0-3600): %s\n", value.c_str());
				return false;
			}
			options.config.hang_watchdog_seconds = static_cast<uint32_t>(seconds);
		} else if (arg == "--debug-gpu-ops") {
			if (Common::EqualNoCase(value, "off")) {
				options.config.gpu_ops_mode = 0;
			} else if (Common::EqualNoCase(value, "ring")) {
				options.config.gpu_ops_mode = 1;
			} else if (Common::EqualNoCase(value, "checkpoints")) {
				options.config.gpu_ops_mode = 2;
			} else {
				::printf("invalid --debug-gpu-ops (expected off, ring or checkpoints): %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pipeline-workers") {
			uint64_t workers = 0;
			if (!ParseUint64(value, workers) || workers == 0 || workers > 64) {
				::printf("invalid pipeline worker count (expected 1-64): %s\n", value.c_str());
				return false;
			}
			options.config.pipeline_workers = static_cast<uint32_t>(workers);
		} else if (arg == "--bench-gpu-trace") {
			if (!ParseBool(value, options.config.bench_gpu_trace)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-timing-log") {
			if (value.empty()) {
				::printf("invalid --shader-timing-log path\n");
				return false;
			}
			options.config.shader_timing_log = Common::PathFromUtf8(value);
		} else if (arg == "--bench-render-csv") {
			if (value.empty()) {
				::printf("invalid --bench-render-csv path\n");
				return false;
			}
			options.config.bench_render_csv = Common::PathFromUtf8(value);
		} else if (arg == "--bench-exit-after-frames") {
			if (!ParseUint64(value, options.config.bench_exit_after_frames)) {
				::printf("invalid frame count: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

static int Main(int argc, char* argv[]) {
	VirtualMemory::Init();
	InitializeThreads();

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc));

    for (int index = 0; index < argc; index++) {
        const std::wstring_view wide(argv[index]);
        const std::u16string utf16(wide.begin(), wide.end());

        utf8_args.push_back(Common::Utf16ToUtf8(utf16));
    }

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());

    for (auto& argument: utf8_args) {
        utf8_argv.push_back(argument.data());
    }

    return Main(argc, utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
#if defined(__linux__)
    // Debug aid: with KYTY_ALLOW_PTRACE=1 any process of this user may attach (gdb, eu-stack) to
    // get stacks of a hung emulator even where kernel.yama.ptrace_scope restricts attaching.
    if (const char* allow = ::getenv("KYTY_ALLOW_PTRACE"); allow != nullptr && allow[0] == '1') {
        ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    }
#endif
    return Main(argc, argv);
}

#endif
