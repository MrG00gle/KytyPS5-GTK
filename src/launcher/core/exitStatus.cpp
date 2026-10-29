#include "launcher/core/exitStatus.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>

namespace kyty::core {

namespace {

// The name kill -l prints for a real-time signal: the nearer of SIGRTMIN+<n> and SIGRTMAX-<n>
// (SIGRTMIN+<n> on a tie); empty outside SIGRTMIN..SIGRTMAX.
std::string RealTimeSignalName(int signal_number) {
	const int min = SIGRTMIN;
	const int max = SIGRTMAX;
	if (signal_number < min || signal_number > max) {
		return {};
	}
	const int above_min = signal_number - min;
	const int below_max = max - signal_number;
	if (above_min <= below_max) {
		return above_min == 0 ? "SIGRTMIN" : "SIGRTMIN+" + std::to_string(above_min);
	}
	return below_max == 0 ? "SIGRTMAX" : "SIGRTMAX-" + std::to_string(below_max);
}

std::string SignalName(int signal_number) {
	if (const char* abbreviation = sigabbrev_np(signal_number); abbreviation != nullptr) {
		return std::string("SIG") + abbreviation;
	}
	if (auto name = RealTimeSignalName(signal_number); !name.empty()) {
		return name;
	}
	return std::to_string(signal_number);
}

} // namespace

ExitDescription DescribeWaitStatus(int status) {
	ExitDescription description;
	if (WIFEXITED(status)) {
		description.code   = WEXITSTATUS(status);
		description.normal = description.code == 0;
		description.text   = "exit code " + std::to_string(description.code);
	} else if (WIFSIGNALED(status)) {
		description.signal = WTERMSIG(status);
		description.text   = "signal " + SignalName(description.signal);
	} else {
		char hex[16];
		std::snprintf(hex, sizeof(hex), "%x", static_cast<unsigned>(status));
		description.text = std::string("wait status 0x") + hex;
	}
	return description;
}

} // namespace kyty::core
