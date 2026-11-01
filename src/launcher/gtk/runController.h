#pragma once

// Running, stopping, and watching the Emulator (task 20.5; plan D34, D35; design.md "Launch and
// monitoring flow"; Requirements 8.10, 10.1, 10.4-10.7, 10.9, 10.11, 10.12, 12.4, 12.7; LFI-11,
// LFI-23; HC-20, HC-24, HC-25, HC-27).
// - Run(game), in the order of HC-24: core::RunState::CanRun (false while a game runs and while an
//   update installs, 10.4; the refusal names the reason), core::EnsureWritableDirectory of the
//   Data_Directory (12.4: the error names the directory, the step, and the reason), the Emulator
//   executable (10.9: a regular file this process may execute; the error names its absolute path
//   and the reason), core::FindCheatPlan, core::ResolveEffectiveConfiguration with the in-memory
//   settings, core::BuildEmulatorArguments; then the title sound stops and pending loads are
//   discarded (TitleSoundPlayer::RunStarted, 8.10), the log is cleared (10.1), the invalid entries
//   of the effective input mapping are listed in a log line (HC-25: BuildEmulatorArguments drops
//   them), a game path or cheat-plan path with a backslash gets a toast and a log line (HC-20; the
//   run goes on), and core::SpawnEmulator starts the Emulator in core::SpawnWorkingDirectory
//   (12.7). On success: RunState::Started, the running flag on that game (LFI-11), the controller
//   service paused (LFI-23), and the ProcessWatcher watching the process. Every refusal and every
//   failure shows an error and starts nothing, so Run stays enabled (10.9, 12.4).
// - Stop(): RunState::StopRequested and ProcessWatcher::Terminate (SIGTERM now, SIGKILL 5 s later
//   if the process is still alive; 10.6, 10.11).
// - The exit (HC-27): the watcher has flushed the partial lines (10.7); RunState::Exited; the log
//   shows the last lines at once; the running flag goes off, the controller service resumes, the
//   title sound may play again; an exit with a non-zero code or by a signal that the user did not
//   ask for gets a log line and an error with the exit code or the signal name (10.12, D34), a
//   user stop nothing.
// Errors go to Callbacks::error (the window shows an alert), warnings to Callbacks::notify (a
// toast). Destroying the controller while the Emulator runs sends it SIGTERM (no SIGKILL follows)
// and reaps it in the background; the quit flow (mainWindow.h) stops it properly first. Main
// thread only.

#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/logModel.h"
#include "launcher/gtk/services/processWatcher.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace kyty::gtk {

class RunController {
public:
	struct Callbacks {
		// A game started, a stop was requested, or the game exited (the window updates its
		// actions).
		std::function<void()> changed;
		// A warning that does not stop the run (HC-20): a short heading and the full text.
		std::function<void(const std::string& heading, const std::string& details)> notify;
		// Run refused or failed, or the game ended unexpectedly: a heading and the message.
		std::function<void(const std::string& heading, const std::string& body)> error;
	};

	// What the harness and the UI tests look at.
	struct Statistics {
		uint64_t                 runs_started = 0;
		uint64_t                 runs_refused = 0; // Run() calls that started nothing
		uint64_t                 exits        = 0;
		std::vector<std::string> last_arguments; // of the last spawn
		std::filesystem::path    last_working_directory;
		pid_t                    pid              = -1; // of the last spawn
		int                      last_wait_status = 0;
		std::string              last_error; // the body of the last error
		// g_get_monotonic_time() in microseconds.
		int64_t started_us        = 0;
		int64_t stop_requested_us = 0;
		int64_t exited_us         = 0;
	};

	// log is the log view's model; its collector receives the Emulator's output.
	RunController(LauncherContext& context, KytyLogModel* log, Callbacks callbacks);
	~RunController();

	RunController(const RunController&)            = delete;
	RunController& operator=(const RunController&) = delete;

	// Runs game (the flow above); true when the Emulator started.
	bool Run(KytyGame* game);
	// Stops the running game; nothing while none runs.
	void Stop();

	// From a successful Run() until the exit flow has finished.
	[[nodiscard]] bool Running() const { return m_watcher.Running(); }
	// The path of the running game ("" while none runs).
	[[nodiscard]] const std::filesystem::path& RunningPath() const { return m_running_path; }
	[[nodiscard]] const Statistics&            Stats() const { return m_stats; }

private:
	void Refuse(const std::string& body);
	void OnExit(int wait_status);

	LauncherContext&         m_context;
	GObjectPtr<KytyLogModel> m_log;
	Callbacks                m_callbacks;
	ProcessWatcher           m_watcher;
	std::filesystem::path    m_running_path;
	Statistics               m_stats;
};

} // namespace kyty::gtk
