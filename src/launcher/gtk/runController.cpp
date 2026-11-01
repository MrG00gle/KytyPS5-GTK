#include "launcher/gtk/runController.h"

#include "launcher/core/dataPaths.h"
#include "launcher/core/emulatorArguments.h"
#include "launcher/core/emulatorProcess.h"
#include "launcher/core/exitStatus.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/services/titleSoundPlayer.h"

#include <cerrno>
#include <cstdarg>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::gtk {

namespace {

namespace fs = std::filesystem;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

// SpawnEmulator's own pre-check, done before the title sound stops and the log is cleared
// (Requirement 10.9): "" for a regular file that this process may execute, else the reason.
std::string ExecutableProblem(const fs::path& executable) {
	struct stat info {};
	if (::stat(executable.c_str(), &info) != 0) {
		return g_strerror(errno);
	}
	if (!S_ISREG(info.st_mode)) {
		return _("it is not a regular file");
	}
	if (::access(executable.c_str(), X_OK) != 0) {
		return g_strerror(errno);
	}
	return "";
}

// The abnormal-exit text (Requirement 10.12), by the kind of exit; the signal name is the core's
// (sigabbrev_np, "SIGSEGV", or a number).
std::string ExitText(int wait_status, const core::ExitDescription& description) {
	if (WIFEXITED(wait_status)) {
		return Format(_("The emulator exited with exit code %d."), description.code);
	}
	if (WIFSIGNALED(wait_status)) {
		constexpr std::string_view kPrefix = "signal ";
		std::string_view           name    = description.text;
		if (name.starts_with(kPrefix)) {
			name.remove_prefix(kPrefix.size());
		}
		return Format(_("The emulator was ended by signal %s."), std::string(name).c_str());
	}
	return Format(_("The emulator ended with %s."), description.text.c_str());
}

bool HasBackslash(const fs::path& path) {
	return path.native().find('\\') != std::string::npos;
}

} // namespace

RunController::RunController(LauncherContext& context, KytyLogModel* log, Callbacks callbacks)
    : m_context(context), m_log(GObjectPtr<KytyLogModel>::Ref(log)),
      m_callbacks(std::move(callbacks)),
      m_watcher(kyty_log_model_get_collector(log),
                {.on_output = [this]() { kyty_log_model_schedule_sync(m_log.get()); },
                 .on_exit   = [this](int wait_status) { OnExit(wait_status); }}) {}

RunController::~RunController() {
	if (m_watcher.Running() && !m_watcher.Exited()) {
		// Never rely on the Emulator outliving its watcher (FEAT-003 (3)): it is asked to close;
		// the watcher's destructor reaps it in the background.
		g_message("run: the launcher closes while %s runs; sending SIGTERM to process %d",
		          m_running_path.c_str(), static_cast<int>(m_watcher.Pid()));
		core::SendTerminate(m_watcher.Pid());
	}
}

void RunController::Refuse(const std::string& body) {
	m_stats.runs_refused++;
	m_stats.last_error = body;
	g_message("run: %s", body.c_str());
	if (m_callbacks.error) {
		m_callbacks.error(_("The game cannot start"), body);
	}
}

bool RunController::Run(KytyGame* game) {
	g_return_val_if_fail(KYTY_IS_GAME(game), false);
	const core::GameEntry entry     = kyty_game_get_entry(game);
	auto&                 run_state = m_context.run_state;

	// 1. One game at a time, and none while an update installs (10.4); the update session's own
	// gate is never relied on (HC-24).
	if (!run_state.CanRun(entry.path) || m_watcher.Running()) {
		Refuse(run_state.Phase() == core::RunPhase::Installing
		           ? _("An update is being installed. Start the game after the installation has "
		               "finished.")
		           : _("Close the running game first."));
		return false;
	}
	// 2. The Data_Directory exists and is writable (12.4).
	const fs::path& data_dir = m_context.paths.data_dir;
	if (auto writable = core::EnsureWritableDirectory(data_dir); !writable) {
		Refuse(Format(_("The data directory cannot be used: %s"), writable.error().c_str()));
		return false;
	}
	// 3. The Emulator can be started (10.9).
	std::error_code ec;
	fs::path        executable = fs::absolute(m_context.emulator.executable, ec);
	if (ec) {
		executable = m_context.emulator.executable;
	}
	if (const std::string problem = ExecutableProblem(executable); !problem.empty()) {
		Refuse(Format(_("Cannot start %s: %s"), executable.c_str(), problem.c_str()));
		return false;
	}
	// 4.-6. The arguments from the configuration as it is now (10.1).
	const auto cheat_plan =
	    core::FindCheatPlan(m_context.services.environment, data_dir, entry.title_id);
	const auto effective =
	    core::ResolveEffectiveConfiguration(m_context.settings.loaded.settings, entry, cheat_plan);
	std::vector<std::string> arguments = core::BuildEmulatorArguments(entry, effective);

	// 7. No title sound while the game runs (8.10); a new log (10.1).
	m_context.services.title_sound.RunStarted();
	kyty_log_model_clear(m_log.get());
	// HC-25: BuildEmulatorArguments leaves these entries out without a word.
	if (const auto invalid =
	        core::FindInvalidMappingEntries(effective.configuration.host_input_mapping);
	    !invalid.empty()) {
		const auto  count = static_cast<unsigned long>(invalid.size());
		std::string line  = Format(
		    NGettext("The input mapping has %lu entry that is not of the form Name=Value; the "
		             "emulator does not get it:",
		             "The input mapping has %lu entries that are not of the form Name=Value; the "
		             "emulator does not get them:",
		             count),
		    count);
		for (std::size_t i = 0; i < invalid.size(); i++) {
			line += i == 0 ? " " : ", ";
			// TRANSLATORS: an invalid input mapping entry: its number (from 1) and its text.
			line += Format(_("Entry %u: “%s”"), static_cast<unsigned>(invalid[i].index + 1),
			               invalid[i].entry.c_str());
		}
		g_message("run: %s", line.c_str());
		kyty_log_model_append_line(m_log.get(), line);
	}
	// HC-20: the Emulator turns a backslash into a slash; the run goes on.
	std::vector<std::string> warnings;
	if (HasBackslash(entry.path)) {
		warnings.push_back(Format(_("The game path contains a backslash (\\). The emulator turns "
		                            "it into a slash (/), so the game may not start: %s"),
		                          entry.path.c_str()));
	}
	if (cheat_plan && HasBackslash(*cheat_plan)) {
		warnings.push_back(Format(_("The cheat file path contains a backslash (\\). The emulator "
		                            "turns it into a slash (/), so it may not find the cheats: %s"),
		                          cheat_plan->c_str()));
	}
	if (!warnings.empty()) {
		std::string details;
		for (const auto& warning: warnings) {
			g_message("run: %s", warning.c_str());
			kyty_log_model_append_line(m_log.get(), warning);
			details += (details.empty() ? "" : "\n\n") + warning;
		}
		if (m_callbacks.notify) {
			m_callbacks.notify(_("A path of this game contains a backslash"), details);
		}
	}

	// 8. The Emulator, in the Data_Directory (12.7).
	core::SpawnRequest request;
	request.executable = executable;
	request.args       = arguments;
	request.cwd        = core::SpawnWorkingDirectory(m_context.paths);
	auto spawned       = core::SpawnEmulator(request);
	if (!spawned) {
		m_context.services.title_sound.GameExited();
		const auto&       error = spawned.error();
		const std::string body =
		    Format(_("Cannot start %s: %s"), error.executable.c_str(), error.reason.c_str());
		kyty_log_model_append_line(m_log.get(), body);
		Refuse(body);
		return false;
	}

	// 9. Running (10.4, LFI-11, LFI-23).
	run_state.Started(entry.path);
	m_running_path = entry.path;
	m_stats.runs_started++;
	m_stats.last_arguments         = std::move(arguments);
	m_stats.last_working_directory = request.cwd;
	m_stats.pid                    = spawned->pid;
	m_stats.started_us             = g_get_monotonic_time();
	kyty_game_set_running(game, true);
	m_context.services.controller.SetPaused(true);
	g_debug("run: started %s (process %d, %zu argument(s), working directory %s)",
	        entry.path.c_str(), static_cast<int>(spawned->pid), m_stats.last_arguments.size(),
	        request.cwd.c_str());
	m_watcher.Watch(*spawned);
	if (m_callbacks.changed) {
		m_callbacks.changed();
	}
	return true;
}

void RunController::Stop() {
	if (!m_watcher.Running() || m_watcher.Exited() || !m_context.run_state.CanStop()) {
		return;
	}
	m_context.run_state.StopRequested();
	m_stats.stop_requested_us = g_get_monotonic_time();
	g_debug("run: stopping process %d (SIGTERM, SIGKILL in 5 s)",
	        static_cast<int>(m_watcher.Pid()));
	m_watcher.Terminate();
	if (m_callbacks.changed) {
		m_callbacks.changed();
	}
}

void RunController::OnExit(int wait_status) {
	const core::ExitReport report = m_context.run_state.Exited(wait_status);
	m_stats.exits++;
	m_stats.last_wait_status = wait_status;
	m_stats.exited_us        = g_get_monotonic_time();
	// The watcher has flushed the partial lines (10.7); the view shows them now.
	kyty_log_model_sync(m_log.get());
	// A rescan during the run made new objects; GameListModel::Replace kept the flag by path.
	if (KytyGame* game = m_context.library.Find(m_running_path); game != nullptr) {
		kyty_game_set_running(game, false);
	}
	g_debug("run: %s exited: %s%s", m_running_path.c_str(), report.description.text.c_str(),
	        report.user_stopped ? " (stopped by the user)" : "");
	m_running_path.clear();
	m_context.services.title_sound.GameExited();
	m_context.services.controller.SetPaused(false);
	if (m_callbacks.changed) {
		m_callbacks.changed();
	}
	// 10.12: only for an exit the user did not ask for, and not for exit code 0.
	if (report.abnormal_message) {
		const std::string text = ExitText(wait_status, report.description);
		m_stats.last_error     = text;
		g_message("run: %s", text.c_str());
		kyty_log_model_append_line(m_log.get(), text);
		if (m_callbacks.error) {
			m_callbacks.error(_("The game ended unexpectedly"), text);
		}
	}
}

} // namespace kyty::gtk
