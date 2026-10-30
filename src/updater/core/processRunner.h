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

struct ProcessResult {
	int         wait_status = -1; // the waitpid() status; -1 (no exit, no signal) when error is set
	std::string error;            // why the process could not start; empty when it ran
	std::string standard_error;   // the first kMaxCapturedStandardError bytes of its standard error
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
	virtual kyty::core::Expected<void, std::string>
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
