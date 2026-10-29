// Example tests of DescribeWaitStatus (src/launcher/core/exitStatus.h, task 15.3): exit codes,
// every standard signal by name, real-time signals, and statuses that are neither.

#include "launcher/core/exitStatus.h"
#include "support/noDisplay.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using kyty::core::DescribeWaitStatus;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ExitStatusTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckText(int status, const std::string& expected) {
	const auto text = DescribeWaitStatus(status).text;
	Check(text == expected,
	      "status " + std::to_string(status) + ": '" + text + "', expected '" + expected + "'");
}

void TestExitCodes() {
	const auto zero = DescribeWaitStatus(W_EXITCODE(0, 0));
	Check(zero.normal && zero.code == 0 && zero.signal == 0 && zero.text == "exit code 0",
	      "exit code 0 is normal");
	const auto one = DescribeWaitStatus(W_EXITCODE(1, 0));
	Check(!one.normal && one.code == 1 && one.signal == 0 && one.text == "exit code 1",
	      "exit code 1 is not normal");
	const auto max = DescribeWaitStatus(W_EXITCODE(255, 0));
	Check(!max.normal && max.code == 255 && max.text == "exit code 255", "exit code 255");
	CheckText(W_EXITCODE(3, 0), "exit code 3");
}

void TestStandardSignals() {
	// sigabbrev_np() of glibc 2.43 (u3-verification.md, U3.1): 29 is POLL.
	static const char* const kNames[] = {
	    "SIGHUP",  "SIGINT",    "SIGQUIT", "SIGILL",   "SIGTRAP", "SIGABRT", "SIGBUS",  "SIGFPE",
	    "SIGKILL", "SIGUSR1",   "SIGSEGV", "SIGUSR2",  "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT",
	    "SIGCHLD", "SIGCONT",   "SIGSTOP", "SIGTSTP",  "SIGTTIN", "SIGTTOU", "SIGURG",  "SIGXCPU",
	    "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGPOLL", "SIGPWR",  "SIGSYS"};
	static_assert(sizeof(kNames) / sizeof(kNames[0]) == 31);
	for (int signal_number = 1; signal_number <= 31; signal_number++) {
		const auto description = DescribeWaitStatus(W_EXITCODE(0, signal_number));
		const auto expected    = std::string("signal ") + kNames[signal_number - 1];
		Check(!description.normal && description.code == 0 && description.signal == signal_number &&
		          description.text == expected,
		      "signal " + std::to_string(signal_number) + ": '" + description.text +
		          "', expected '" + expected + "'");
	}
	// A core dump flag does not change the text.
	CheckText(W_EXITCODE(0, SIGSEGV) | 0x80, "signal SIGSEGV");
	CheckText(W_EXITCODE(0, SIGABRT) | 0x80, "signal SIGABRT");
}

void TestRealTimeSignals() {
	Check(SIGRTMIN == 34 && SIGRTMAX == 64, "glibc reserves 32 and 33: SIGRTMIN 34, SIGRTMAX 64");
	const auto rt1 = DescribeWaitStatus(W_EXITCODE(0, SIGRTMIN + 1));
	Check(!rt1.normal && rt1.signal == SIGRTMIN + 1 && rt1.text == "signal SIGRTMIN+1",
	      "SIGRTMIN+1: " + rt1.text);
	CheckText(W_EXITCODE(0, SIGRTMIN), "signal SIGRTMIN");
	// kill -l: SIGRTMIN+15 is 49, SIGRTMAX-14 is 50.
	CheckText(W_EXITCODE(0, SIGRTMIN + 15), "signal SIGRTMIN+15");
	CheckText(W_EXITCODE(0, SIGRTMIN + 16), "signal SIGRTMAX-14");
	CheckText(W_EXITCODE(0, SIGRTMAX - 1), "signal SIGRTMAX-1");
	CheckText(W_EXITCODE(0, SIGRTMAX), "signal SIGRTMAX");
	// The glibc-internal signals below SIGRTMIN have no name.
	CheckText(W_EXITCODE(0, 32), "signal 32");
	CheckText(W_EXITCODE(0, 33), "signal 33");
}

void TestOtherStatuses() {
	const auto stopped = DescribeWaitStatus(W_STOPCODE(SIGSTOP));
	Check(!stopped.normal && stopped.code == 0 && stopped.signal == 0 &&
	          stopped.text == "wait status 0x137f",
	      "a stopped status: " + stopped.text);
	CheckText(W_STOPCODE(SIGTSTP), "wait status 0x147f");
	CheckText(0xffff, "wait status 0xffff"); // continued (WIFCONTINUED)
}

// A real child: the status that waitpid() reports.
void TestRealChild() {
	const pid_t child = fork();
	Check(child >= 0, "fork");
	if (child == 0) {
		std::signal(SIGUSR2, SIG_DFL);
		raise(SIGUSR2);
		_exit(0);
	}
	int status = 0;
	Check(waitpid(child, &status, 0) == child, "waitpid");
	CheckText(status, "signal SIGUSR2");

	const pid_t exiting = fork();
	Check(exiting >= 0, "fork");
	if (exiting == 0) {
		_exit(42);
	}
	Check(waitpid(exiting, &status, 0) == exiting, "waitpid");
	const auto description = DescribeWaitStatus(status);
	Check(!description.normal && description.code == 42 && description.text == "exit code 42",
	      "a real exit code 42: " + description.text);
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ExitStatusTests");
	TestExitCodes();
	TestStandardSignals();
	TestRealTimeSignals();
	TestOtherStatuses();
	TestRealChild();
	std::printf("ExitStatusTests: all tests passed\n");
	return 0;
}
