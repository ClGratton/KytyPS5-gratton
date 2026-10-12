#ifndef LAUNCHER_INCLUDE_EMULATOR_ARGS_H_
#define LAUNCHER_INCLUDE_EMULATOR_ARGS_H_

#include <QStringList>

class Configuration;

// The emulator command line (without the program) for a configuration, and the environment
// assignments ("NAME=value") the emulator is started with. Empty args = invalid configuration.
// install_dir is the emulator's directory, where the MangoHud settings file (mangohud.conf) lives.
QStringList CreateEmulatorArgs(const Configuration& info);
QStringList CreateEmulatorEnvironment(const Configuration& info, const QString& install_dir);

#endif /* LAUNCHER_INCLUDE_EMULATOR_ARGS_H_ */
