#pragma once

// Running the update steps' processes (task 17.1; design.md "Updater interfaces" and "Updater
// flow"): /usr/bin/tar and kyty_updater --validate run to completion, kyty_updater --install starts
// detached. The update session uses the interface; tests use fakes or the POSIX implementation.

#include "launcher/core/expected.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kyty::updater {

// Run() keeps at most this many bytes of the process's standard error (the Qt launcher's limit).
inline constexpr size_t kMaxCapturedStandardError = 4096;

// Why a process could not be started or waited for, so that the GTK layer can say it in its own
// words (plan V1). The POSIX runner sets a kind for every error; a runner that gives only a
// message (kind None with a message) is shown as that text.
enum class ProcessErrorKind {
	None,
	NulArgument, // "Cannot start <program>: argument <argument> contains a NUL byte"
	Pipe,        // "Cannot start <program>: cannot create a pipe: <errno text>"
	Prepare,     // "Cannot start <program>: cannot prepare the spawn: <errno text>"
	Spawn,       // "Cannot start <program>: <errno text>[ (working directory <path>)]"
	OpenLog,     // "Cannot start <program>: cannot open the log file <path>: <errno text>"
	Wait,        // "Cannot wait for <program>: <errno text>"
};

struct ProcessError {
	ProcessErrorKind      kind = ProcessErrorKind::None;
	std::filesystem::path program; // made absolute
	int                   error_number = 0;
	size_t                argument     = 0; // NulArgument: the argument's number, from 1
	std::filesystem::path path;             // Spawn: the working directory (may be empty); OpenLog
	std::string           message;          // the English text

	friend bool operator==(const ProcessError&, const ProcessError&) = default;
};

struct ProcessResult {
	int         wait_status = -1; // the waitpid() status; -1 (no exit, no signal) when error is set
	std::string error;            // why the process could not start; empty when it ran
	std::string standard_error;   // the first kMaxCapturedStandardError bytes of its standard error
	ProcessError failure;          // error with its parts (failure.message is error)
};

class ProcessRunner {
public:
	virtual ~ProcessRunner() = default;

	// Runs program with argv = {program, args...} in cwd (empty: this process's directory) and
	// blocks until it exits.
	virtual ProcessResult Run(const std::filesystem::path& program, std::vector<std::string> args,
	                          const std::filesystem::path& cwd) = 0;

	// Starts program with argv = {program, args...} in cwd as the leader of a new session, with its
	// standard output and standard error written to log_file (created or truncated), and returns
	// without waiting. The design's bool, with the reason of a failure.
	virtual kyty::core::Expected<void, ProcessError>
	StartDetached(const std::filesystem::path& program, std::vector<std::string> args,
	              const std::filesystem::path& cwd, const std::filesystem::path& log_file) = 0;
};

// The POSIX implementation: posix_spawn() without a shell, a relative program made absolute first,
// posix_spawn_file_actions_addchdir_np(cwd), standard input from /dev/null, standard output to
// /dev/null and standard error to a pipe (Run) or both to log_file (StartDetached), no other
// descriptor (posix_spawn_file_actions_addclosefrom_np(3)), the default action for SIGPIPE, an
// empty signal mask, POSIX_SPAWN_SETSID for detached processes, and this process's environment
// without TAR_OPTIONS (as the Qt launcher removed it). An argument with a NUL byte is refused.
// Errors read "Cannot start <program>: <reason>". Run() keeps reading standard error after the
// first kMaxCapturedStandardError bytes, so a talkative process never blocks. A detached process
// stays a child of this process until it exits or this process ends; the launcher quits right
// after starting it.
std::unique_ptr<ProcessRunner> MakePosixProcessRunner();

} // namespace kyty::updater
