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
// Both return the process exit code.
int RunSelfCheck(const QStringList& args);
int RunPrintCommand(const QStringList& args);
// launcher --grab-settings <dir> [--global] [--preset]: saves one PNG per tab of the settings dialog.
int RunGrabSettings(const QStringList& args);

#endif /* LAUNCHER_INCLUDE_LAUNCHER_SELF_CHECK_H_ */
