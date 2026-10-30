#pragma once

// Whether a game runs or an update installs (task 15.4; design.md, "Run state"; Requirements 10.4,
// 10.12, 11.18). A pure state machine: every enable and disable decision of the launcher reads its
// predicates, and the launcher reports what happened to the Emulator process and to an update
// installation as events.
//
//   Idle --Started(entry)--> Running --StopRequested--> Stopping
//   Running --Exited(status)--> Idle (an abnormal exit is reported)
//   Stopping --Exited(status)--> Idle (never reported: the user stopped the game)
//   Idle --InstallStarted--> Installing --InstallFinished--> Idle
//
// An event that the diagram does not allow in the current state is ignored and changes nothing:
// Started outside Idle, StopRequested outside Running, Exited in Idle or Installing,
// InstallStarted outside Idle, and InstallFinished outside Installing. The caller sends SIGTERM
// when it reports StopRequested and runs the SIGKILL timer itself (Requirement 10.11).

#include "launcher/core/entryId.h"
#include "launcher/core/exitStatus.h"

#include <optional>
#include <string>
#include <string_view>

namespace kyty::core {

enum class RunPhase {
	Idle,       // no Emulator process and no update installation
	Running,    // the Emulator process started for an entry has not exited
	Stopping,   // as Running, after the user asked the process to stop
	Installing, // an update installation is in progress
};

// What Exited reports about the process that ended.
struct ExitReport {
	ExitDescription description;          // DescribeWaitStatus(wait_status)
	bool            user_stopped = false; // the process exited after StopRequested
	// The message to show and to append to the log (Requirement 10.12): description.text when the
	// user did not stop the process and it did not exit with code 0; nothing otherwise, and for
	// an ignored Exited.
	std::optional<std::string> abnormal_message;
};

class RunState {
public:
	// The answer to an update installation request while CanInstallUpdate() is false
	// (Requirement 10.4).
	static constexpr std::string_view kInstallRefusalMessage = "Close the running game first.";

	// Run is enabled only in Idle, for every entry (Requirement 10.4).
	[[nodiscard]] bool CanRun(const EntryId& entry) const;
	// Stop is enabled in Running and Stopping.
	[[nodiscard]] bool CanStop() const;
	// False for the running entry while its process is alive, true otherwise (Requirement 11.18).
	[[nodiscard]] bool CanCompress(const EntryId& entry) const;
	[[nodiscard]] bool CanRemoveSaveData(const EntryId& entry) const;
	// False in Running and Stopping. True while Installing, so that an installation in progress can
	// check again before it hands over (Requirement 10.4: no installation starts while a game
	// runs; InstallStarted itself is ignored outside Idle).
	[[nodiscard]] bool CanInstallUpdate() const;
	// kInstallRefusalMessage.
	[[nodiscard]] static std::string_view RefusalMessage();

	// The entry whose process is alive (Running or Stopping), if any.
	[[nodiscard]] std::optional<EntryId> Running() const;
	[[nodiscard]] RunPhase               Phase() const { return m_phase; }

	// The Emulator process for entry started: Idle to Running.
	void Started(EntryId entry);
	// The user asked the running process to stop: Running to Stopping.
	void StopRequested();
	// The process ended with wait_status (a waitpid() status): Running or Stopping to Idle. In
	// Idle and Installing the event is ignored; the report then has the description and no
	// message.
	ExitReport Exited(int wait_status);
	// An update installation started (Idle to Installing) or ended (Installing to Idle).
	void InstallStarted();
	void InstallFinished();

private:
	RunPhase               m_phase = RunPhase::Idle;
	std::optional<EntryId> m_running;
};

} // namespace kyty::core
