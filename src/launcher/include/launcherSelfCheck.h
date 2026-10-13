#ifndef LAUNCHER_INCLUDE_LAUNCHER_SELF_CHECK_H_
#define LAUNCHER_INCLUDE_LAUNCHER_SELF_CHECK_H_

#include <QStringList>

// Command-line helpers that run without a window (main() calls them before the GUI starts):
//
//   launcher --self-check [--emulator <path>] [--play-config <json>]
//       Checks the options table (defaults, choices), the launcher's defaults against the
//       emulator's, that every flag the launcher can pass is in the emulator's --help, that the
//       default and the "Recommended for play" command lines are accepted by the emulator's
//       parser, and that the preset reproduces the flags of a bench play config.
//   launcher --print-command [--shell] [--preset] --game <dir>
//       Prints the environment and command line the launcher would start the emulator with, from
//       the saved global settings (or, with --preset, the recommended set on top of the defaults).
//       --shell prints it as export/exec lines for eval (the kyty-gratton run wrapper).
//
//   launcher --configure [--game-dir <dir>] [--preset]
//       Adds a game folder to the saved settings and/or applies the "Recommended for play" set to
//       the saved global settings (used by the install script on a first install).
//
// All return the process exit code.
int RunSelfCheck(const QStringList& args);
int RunPrintCommand(const QStringList& args);
int RunConfigure(const QStringList& args);
// launcher --grab-main <png>: starts the main window, lists the games it found and saves a PNG.
int RunGrabMain(const QStringList& args);
// launcher --grab-settings <dir> [--global] [--preset]: saves one PNG per tab of the settings dialog.
int RunGrabSettings(const QStringList& args);
// launcher --host-memory [--root <dir>]: prints what the "Host memory" window would show (Linux), optionally
// for a made-up /proc and /sys tree under <dir>. Changes nothing.
int RunHostMemory(const QStringList& args);
// launcher --grab-host-memory <png> [--root <dir>]: saves the "Host memory" window as a PNG.
int RunGrabHostMemory(const QStringList& args);

#endif /* LAUNCHER_INCLUDE_LAUNCHER_SELF_CHECK_H_ */
