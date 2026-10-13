#ifndef LAUNCHER_INCLUDE_HOST_MEMORY_SCRIPT_H_
#define LAUNCHER_INCLUDE_HOST_MEMORY_SCRIPT_H_

// The helper script, compiled into the launcher (generated at build time from host/kyty-host-memory, or
// host/kyty-host-memory.ps1 on Windows): the text that runs as root or administrator is the text that was
// built, not a file next to the launcher that something else could have replaced.
extern const char kHostMemoryScriptText[];
// SHA-256 of that text, lower-case hex, computed by CMake from the source file.
extern const char kHostMemoryScriptSha256[];

#endif /* LAUNCHER_INCLUDE_HOST_MEMORY_SCRIPT_H_ */
