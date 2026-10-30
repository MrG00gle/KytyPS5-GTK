// Feature: linux-gtk-modernization, Property 24: Run-state invariants
//
// Validates Requirements 10.4, 10.12, and 11.18. Sequences of 0-80 events (start one of four
// entries, stop requested, exit with an arbitrary wait status, rescan, install started, install
// finished) drive a RunState and a reference model of the design's state diagram. After every
// event: the phase matches the model; at most one entry runs and it is the started one; Run is
// enabled for no entry while a process is alive; Stop is enabled only while a process is alive;
// compression and save data removal are disabled for the running entry only; update installation
// is refused while a process is alive. An exit without a user stop and with a non-zero code or a
// signal reports DescribeWaitStatus(status), which contains the decimal exit code or the signal
// (its SIG name, or its number for the unnamed signals 32 and 33); after a user stop, and for an
// exit while no process is alive, nothing is reported.

#include "launcher/core/exitStatus.h"
#include "launcher/core/runState.h"
#include "support/property.h"

#include <cstddef>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {

using kyty::core::EntryId;
using kyty::core::RunPhase;

int Choose(int min, int max) {
	return *rc::gen::resize(100, rc::gen::inRange(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

const std::vector<EntryId>& Pool() {
	static const std::vector<EntryId> pool = {"/games/A", "/games/B.zar", "/games/C", "/other/D"};
	return pool;
}

// Exit codes, signal deaths (with or without a core dump), stopped and continued statuses, and
// arbitrary integers.
int WaitStatus() {
	switch (Choose(0, 6)) {
		case 0: return W_EXITCODE(0, 0);
		case 1: return W_EXITCODE(Choose(1, 256), 0);
		case 2: return W_EXITCODE(0, Choose(1, 65));
		case 3: return W_EXITCODE(0, Choose(1, 65)) | 0x80;
		case 4: return OneIn(2) ? W_STOPCODE(Choose(1, 65)) : 0xffff;
		default: return *rc::gen::arbitrary<int>();
	}
}

const char* PhaseName(RunPhase phase) {
	switch (phase) {
		case RunPhase::Idle: return "Idle";
		case RunPhase::Running: return "Running";
		case RunPhase::Stopping: return "Stopping";
		case RunPhase::Installing: return "Installing";
	}
	return "?";
}

} // namespace

KYTY_PROPERTY(24, "Run-state invariants") {
	return rc::check(name, []() {
		kyty::core::RunState state;
		// The reference model.
		RunPhase               phase = RunPhase::Idle;
		std::optional<EntryId> running;
		// The entries the last rescan found; the run state does not depend on them.
		std::vector<EntryId> listed   = Pool();
		bool                 reported = false, stopped = false, ignored_exit = false;

		const int count = Choose(0, 81);
		for (int index = 0; index < count; index++) {
			const bool alive = phase == RunPhase::Running || phase == RunPhase::Stopping;
			switch (Choose(0, 6)) {
				case 0: {
					const auto& entry = Pool()[static_cast<size_t>(Choose(0, 4))];
					RC_LOG() << index << ": Started " << entry.string() << "\n";
					state.Started(entry);
					if (phase == RunPhase::Idle) {
						phase   = RunPhase::Running;
						running = entry;
					}
					break;
				}
				case 1:
					RC_LOG() << index << ": StopRequested\n";
					state.StopRequested();
					if (phase == RunPhase::Running) {
						phase = RunPhase::Stopping;
					}
					break;
				case 2: {
					const int status = WaitStatus();
					RC_LOG() << index << ": Exited " << status << "\n";
					const auto report      = state.Exited(status);
					const auto description = kyty::core::DescribeWaitStatus(status);
					RC_ASSERT(report.description.text == description.text);
					if (!alive) {
						RC_ASSERT(!report.abnormal_message.has_value());
						RC_ASSERT(!report.user_stopped);
						ignored_exit = true;
						break;
					}
					const bool user_stop = phase == RunPhase::Stopping;
					RC_ASSERT(report.user_stopped == user_stop);
					const bool exit_zero = WIFEXITED(status) && WEXITSTATUS(status) == 0;
					if (user_stop || exit_zero) {
						RC_ASSERT(!report.abnormal_message.has_value());
						stopped = stopped || user_stop;
					} else {
						RC_ASSERT(report.abnormal_message == description.text);
						const auto& message = *report.abnormal_message;
						if (WIFEXITED(status)) {
							RC_ASSERT(message.find(std::to_string(WEXITSTATUS(status))) !=
							          std::string::npos);
						} else if (WIFSIGNALED(status)) {
							RC_ASSERT(message.find("SIG") != std::string::npos ||
							          message.find(std::to_string(WTERMSIG(status))) !=
							              std::string::npos);
						}
						reported = true;
					}
					phase = RunPhase::Idle;
					running.reset();
					break;
				}
				case 3: {
					RC_LOG() << index << ": rescan\n";
					listed.clear();
					for (const auto& entry: Pool()) {
						if (!OneIn(3)) {
							listed.push_back(entry);
						}
					}
					break;
				}
				case 4:
					RC_LOG() << index << ": InstallStarted\n";
					state.InstallStarted();
					if (phase == RunPhase::Idle) {
						phase = RunPhase::Installing;
					}
					break;
				default:
					RC_LOG() << index << ": InstallFinished\n";
					state.InstallFinished();
					if (phase == RunPhase::Installing) {
						phase = RunPhase::Idle;
					}
					break;
			}

			// Invariants after every event.
			RC_LOG() << "  phase " << PhaseName(state.Phase()) << "\n";
			RC_ASSERT(state.Phase() == phase);
			const bool now_alive = phase == RunPhase::Running || phase == RunPhase::Stopping;
			RC_ASSERT(state.Running() == (now_alive ? running : std::nullopt));
			RC_ASSERT(state.CanStop() == now_alive);
			RC_ASSERT(state.CanInstallUpdate() == !now_alive);
			std::vector<EntryId> entries = Pool();
			entries.insert(entries.end(), listed.begin(), listed.end());
			for (const auto& entry: entries) {
				RC_ASSERT(state.CanRun(entry) == (phase == RunPhase::Idle));
				if (now_alive) {
					RC_ASSERT(!state.CanRun(entry));
				}
				const bool is_running = now_alive && entry == *running;
				RC_ASSERT(state.CanCompress(entry) == !is_running);
				RC_ASSERT(state.CanRemoveSaveData(entry) == !is_running);
			}
		}

		RC_TAG(reported ? "abnormal exit reported" : "no abnormal exit");
		RC_CLASSIFY(stopped, "user stop");
		RC_CLASSIFY(ignored_exit, "exit while no process is alive");
	});
}
