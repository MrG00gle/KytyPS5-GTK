#pragma once

// Starting and signalling the Emulator process (task 15.3; design.md "Launch and monitoring flow";
// Requirements 10.3, 10.6, 10.9, 10.11). Reaping the process, reading its output, and the 5 s
// SIGTERM-to-SIGKILL escalation belong to the caller (the GTK ProcessWatcher).

#include "launcher/core/expected.h"

#include <filesystem>
#include <functional>
#include <string>
#include <sys/types.h> // pid_t (POSIX)
#include <vector>

namespace kyty::core {

struct SpawnRequest {
	std::filesystem::path    executable; // the Emulator; a relative path is made absolute first
	std::vector<std::string> args;       // argv[1...], passed as they are (no shell)
	std::filesystem::path    cwd; // the working directory (the Data_Directory); empty keeps ours
};

struct SpawnedProcess {
	pid_t pid     = -1;
	int stdout_fd = -1; // the read end of the child's standard output: non-blocking, close-on-exec
	int stderr_fd = -1; // the read end of the child's standard error: non-blocking, close-on-exec
};

enum class SpawnErrorKind {
	Missing,       // the executable does not exist
	NotExecutable, // it is not a regular file, or it cannot be executed
	SpawnFailed,   // pipes, spawn attributes, or posix_spawn itself failed
};

struct SpawnError {
	SpawnErrorKind        kind = SpawnErrorKind::SpawnFailed;
	std::filesystem::path executable;       // absolute
	int                   error_number = 0; // errno value
	std::string           reason;           // the errno text or what was wrong
	std::string           message; // "Cannot start <executable>: <reason>" (Requirement 10.9)
};

// Starts executable with argv = {absolute executable path, args...} through posix_spawn() (no
// shell), in request.cwd (posix_spawn_file_actions_addchdir_np), with the environment of this
// process. The child gets standard input from /dev/null, standard output and standard error on two
// new pipes (the child's ends block; the returned read ends are O_NONBLOCK and O_CLOEXEC), no other
// descriptor (posix_spawn_file_actions_addclosefrom_np(3)), the default action for SIGPIPE and
// SIGTERM even when this process ignores them, and an empty signal mask.
// Before spawning, the executable must be a regular file (stat) that access(X_OK) allows: a missing
// one is SpawnErrorKind::Missing, anything else NotExecutable. Every later failure is SpawnFailed
// and closes every descriptor opened here; an argument with a NUL byte is refused (EINVAL), because
// it could not be passed intact. The caller owns the returned descriptors and must reap the child.
Expected<SpawnedProcess, SpawnError> SpawnEmulator(const SpawnRequest& request);

// kill(pid, SIGTERM) and kill(pid, SIGKILL); true when the signal was sent. A pid of 0 or less (a
// process group, or every process) is refused and gives false.
bool SendTerminate(pid_t pid);
bool SendKill(pid_t pid);

// The steps of SpawnEmulator after the pre-check, for fault injection in tests. Pipe is asked once
// per pipe (standard output first).
enum class SpawnStep { Pipe, FileActions, Attributes, Spawn };

// Tests only, not thread-safe: before each step, SpawnEmulator calls fault(step); a non-zero return
// value is the errno that the step then fails with, without performing it. An empty function
// removes the hook.
void SetSpawnFaultForTesting(std::function<int(SpawnStep)> fault);

} // namespace kyty::core
