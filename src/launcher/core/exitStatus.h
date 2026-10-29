#pragma once

// The text of an Emulator exit for the log view and the abnormal-exit message (task 15.3,
// Requirement 10.12).

#include <string>

namespace kyty::core {

struct ExitDescription {
	bool        normal = false; // true only for exit code 0
	int         code   = 0;     // the exit code when the process exited, else 0
	int         signal = 0;     // the terminating signal number when a signal ended it, else 0
	std::string text;           // "exit code 3", "signal SIGSEGV", or "wait status 0x<hex>"
};

// Describes a waitpid() status:
// - exited: "exit code <N>", normal only for 0;
// - ended by a signal: "signal SIG<name>" with the name from sigabbrev_np() ("signal SIGHUP"); a
//   real-time signal is "signal SIGRTMIN", "signal SIGRTMIN+<n>", "signal SIGRTMAX-<n>", or
//   "signal SIGRTMAX", the nearer bound as kill -l prints it; any other unnamed number is
//   "signal <number>";
// - anything else (stopped, continued): "wait status 0x<lower-case hex>".
ExitDescription DescribeWaitStatus(int status);

} // namespace kyty::core
