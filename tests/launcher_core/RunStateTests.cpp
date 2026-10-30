// Example tests of the run state (src/launcher/core/runState.h, task 15.4): every transition, every
// ignored event, the predicates in each state, the abnormal-exit message, and the refusal text.

#include "launcher/core/runState.h"
#include "support/noDisplay.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

namespace {

using kyty::core::EntryId;
using kyty::core::RunPhase;
using kyty::core::RunState;

const EntryId kGameA = "/games/A";
const EntryId kGameB = "/games/B.zar";

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "RunStateTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string PhaseName(RunPhase phase) {
	switch (phase) {
		case RunPhase::Idle: return "Idle";
		case RunPhase::Running: return "Running";
		case RunPhase::Stopping: return "Stopping";
		case RunPhase::Installing: return "Installing";
	}
	return "?";
}

// The predicates of a state without a process (Idle or Installing).
void CheckNoProcess(const RunState& state, RunPhase phase, const std::string& context) {
	Check(state.Phase() == phase, context + ": phase " + PhaseName(state.Phase()));
	Check(state.CanRun(kGameA) == (phase == RunPhase::Idle) &&
	          state.CanRun(kGameB) == (phase == RunPhase::Idle),
	      context + ": Run is enabled only in Idle");
	Check(!state.CanStop(), context + ": Stop is disabled");
	Check(state.CanCompress(kGameA) && state.CanCompress(kGameB) &&
	          state.CanRemoveSaveData(kGameA) && state.CanRemoveSaveData(kGameB),
	      context + ": compression and save data removal are enabled");
	Check(state.CanInstallUpdate(), context + ": update installation is allowed");
	Check(!state.Running().has_value(), context + ": nothing runs");
}

// The predicates while the process for running is alive (Running or Stopping).
void CheckProcess(const RunState& state, RunPhase phase, const EntryId& running,
                  const EntryId& other, const std::string& context) {
	Check(state.Phase() == phase, context + ": phase " + PhaseName(state.Phase()));
	Check(!state.CanRun(running) && !state.CanRun(other), context + ": Run is disabled for all");
	Check(state.CanStop(), context + ": Stop is enabled");
	Check(!state.CanCompress(running) && !state.CanRemoveSaveData(running),
	      context + ": the running entry cannot be compressed or lose its save data");
	Check(state.CanCompress(other) && state.CanRemoveSaveData(other),
	      context + ": another entry can be compressed and lose its save data");
	Check(!state.CanInstallUpdate(), context + ": update installation is refused");
	Check(state.Running() == running, context + ": the started entry runs");
}

void TestInitialState() {
	const RunState state;
	CheckNoProcess(state, RunPhase::Idle, "initial");
}

void TestRunAndExit() {
	RunState state;
	state.Started(kGameA);
	CheckProcess(state, RunPhase::Running, kGameA, kGameB, "after Started");

	const auto report = state.Exited(W_EXITCODE(0, 0));
	Check(!report.user_stopped && report.description.normal &&
	          report.description.text == "exit code 0" && !report.abnormal_message,
	      "exit code 0 is not reported");
	CheckNoProcess(state, RunPhase::Idle, "after a normal exit");

	// A new run of another entry.
	state.Started(kGameB);
	CheckProcess(state, RunPhase::Running, kGameB, kGameA, "second run");
	const auto failed = state.Exited(W_EXITCODE(3, 0));
	Check(!failed.user_stopped && failed.description.code == 3 &&
	          failed.abnormal_message == std::string("exit code 3"),
	      "exit code 3 is reported");
	CheckNoProcess(state, RunPhase::Idle, "after exit code 3");

	state.Started(kGameA);
	const auto crashed = state.Exited(W_EXITCODE(0, SIGSEGV) | 0x80);
	Check(!crashed.user_stopped && crashed.description.signal == SIGSEGV &&
	          crashed.abnormal_message == std::string("signal SIGSEGV"),
	      "a crash is reported with the signal name");

	state.Started(kGameA);
	const auto max = state.Exited(W_EXITCODE(255, 0));
	Check(max.abnormal_message == std::string("exit code 255"), "exit code 255 is reported");

	state.Started(kGameA);
	const auto killed = state.Exited(W_EXITCODE(0, SIGKILL));
	Check(killed.abnormal_message == std::string("signal SIGKILL"),
	      "a SIGKILL without a user stop is reported");
}

void TestUserStop() {
	RunState state;
	state.Started(kGameA);
	state.StopRequested();
	CheckProcess(state, RunPhase::Stopping, kGameA, kGameB, "after StopRequested");

	const auto terminated = state.Exited(W_EXITCODE(0, SIGTERM));
	Check(terminated.user_stopped && terminated.description.text == "signal SIGTERM" &&
	          !terminated.abnormal_message,
	      "SIGTERM after a user stop is not reported");
	CheckNoProcess(state, RunPhase::Idle, "after a stopped run");

	// After the 5 s timer the caller sends SIGKILL; the exit is still not reported.
	state.Started(kGameA);
	state.StopRequested();
	const auto killed = state.Exited(W_EXITCODE(0, SIGKILL));
	Check(killed.user_stopped && !killed.abnormal_message, "SIGKILL after a user stop");

	// Any status after a user stop, also a failing exit code.
	state.Started(kGameB);
	state.StopRequested();
	const auto failed = state.Exited(W_EXITCODE(1, 0));
	Check(failed.user_stopped && failed.description.code == 1 && !failed.abnormal_message,
	      "exit code 1 after a user stop is not reported");
	CheckNoProcess(state, RunPhase::Idle, "after exit code 1 of a stopped run");
}

void TestIgnoredWhileRunning() {
	RunState state;
	state.Started(kGameA);
	state.Started(kGameB);
	CheckProcess(state, RunPhase::Running, kGameA, kGameB, "Started while Running is ignored");
	state.InstallStarted();
	CheckProcess(state, RunPhase::Running, kGameA, kGameB,
	             "InstallStarted while Running is ignored");
	state.InstallFinished();
	CheckProcess(state, RunPhase::Running, kGameA, kGameB,
	             "InstallFinished while Running is ignored");

	state.StopRequested();
	state.StopRequested();
	CheckProcess(state, RunPhase::Stopping, kGameA, kGameB,
	             "StopRequested while Stopping is ignored");
	state.Started(kGameB);
	state.InstallStarted();
	state.InstallFinished();
	CheckProcess(state, RunPhase::Stopping, kGameA, kGameB,
	             "Started and install events while Stopping are ignored");
}

void TestIgnoredWithoutProcess() {
	RunState state;
	state.StopRequested();
	CheckNoProcess(state, RunPhase::Idle, "StopRequested in Idle is ignored");
	const auto idle_exit = state.Exited(W_EXITCODE(2, 0));
	Check(!idle_exit.user_stopped && !idle_exit.abnormal_message &&
	          idle_exit.description.text == "exit code 2",
	      "Exited in Idle is ignored and reports nothing");
	CheckNoProcess(state, RunPhase::Idle, "after Exited in Idle");
	state.InstallFinished();
	CheckNoProcess(state, RunPhase::Idle, "InstallFinished in Idle is ignored");
}

void TestInstalling() {
	RunState state;
	state.InstallStarted();
	CheckNoProcess(state, RunPhase::Installing, "after InstallStarted");

	state.Started(kGameA);
	CheckNoProcess(state, RunPhase::Installing, "Started while Installing is ignored");
	state.StopRequested();
	CheckNoProcess(state, RunPhase::Installing, "StopRequested while Installing is ignored");
	const auto report = state.Exited(W_EXITCODE(0, SIGSEGV));
	Check(!report.abnormal_message && !report.user_stopped,
	      "Exited while Installing is ignored and reports nothing");
	CheckNoProcess(state, RunPhase::Installing, "after Exited while Installing");
	state.InstallStarted();
	CheckNoProcess(state, RunPhase::Installing, "InstallStarted while Installing is ignored");

	state.InstallFinished();
	CheckNoProcess(state, RunPhase::Idle, "after InstallFinished");
	state.Started(kGameA);
	CheckProcess(state, RunPhase::Running, kGameA, kGameB, "a run after the installation");
}

void TestRefusalMessage() {
	Check(RunState::RefusalMessage() == "Close the running game first.",
	      "the refusal message (Requirement 10.4)");
	Check(RunState::RefusalMessage() == RunState::kInstallRefusalMessage,
	      "RefusalMessage() is kInstallRefusalMessage");
}

void TestOtherStatuses() {
	RunState state;
	state.Started(kGameA);
	const auto stopped = state.Exited(W_STOPCODE(SIGSTOP));
	Check(stopped.abnormal_message == std::string("wait status 0x137f"),
	      "a status that is neither an exit nor a signal is reported as it is");
	CheckNoProcess(state, RunPhase::Idle, "after an unusual status");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("RunStateTests");
	TestInitialState();
	TestRunAndExit();
	TestUserStop();
	TestIgnoredWhileRunning();
	TestIgnoredWithoutProcess();
	TestInstalling();
	TestRefusalMessage();
	TestOtherStatuses();
	std::printf("RunStateTests: all tests passed\n");
	return 0;
}
