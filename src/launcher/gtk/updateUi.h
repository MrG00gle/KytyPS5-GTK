#pragma once

// The update UI of the GTK launcher (task 21.6; plan D45, G2; HC-05, HC-08, HC-34, HC-35, HC-36;
// Requirements 10.4, 13.3, 13.5, 13.10, 13.12-13.14; LFI-33-LFI-35), ported from the Qt
// launcher's UpdateChecker (src/launcher/src/updateChecker.cpp). LauncherContext::update_mode,
// which the application sets once from updater::CurrentUpdateUiMode(), decides what exists:
// - Official builds: app.check-updates (Ctrl+U, the primary menu, the footer link) runs a
//   user-activated check through an updater::UpdateSession, and Start() runs a quiet one when
//   check_updates_on_startup is set. A newer release opens the offer (Cancel, Release notes, and
//   Install update, the default) and reveals the main window's banner while the offer stands, so
//   that the banner's button shows the offer again. Release notes opens the release's page and
//   keeps the offer; Cancel ends it. Install downloads the package in the progress window
//   (updateProgressWindow.h: the received bytes over the package size, Cancel; then a spinner while
//   tar and the validation run, during which the main loop may pause, G2), and the handoff to
//   kyty_updater quits the launcher at once. "No newer release is available." and check failures
//   are shown only for user-activated checks, installation failures always (with "Open release
//   page"), a failure without a message (Cancel) never; while a game runs, Install answers "Close
//   the running game first." and the offer stays (10.4). The session's install gate is
//   core::RunState (one to one), so no game starts while an update installs.
// - Other builds: only app.open-releases-page (the primary menu and the footer link), which opens
//   updater::kReleasesPage; no session and no request.
// - Flatpak builds: nothing at all (no control, no setting, no request, 13.3).
// In Official and Other builds Start() also removes stale update workspaces from the installation
// directory (updater::CleanupStaleWorkspaces in a GTask thread, HC-08) and reads the previous
// update's result (updater::ReadAndClearPreviousResult, which deletes the file), shown once as soon
// as the parent window is mapped (HC-35, 13.14).
// Every update alert has the window title "KytyPS5 Update" (the Qt launcher's message boxes). The
// updater's fixed English messages are shown as translated texts, its other messages (reasons,
// paths, a process's standard error) as they are, with invalid UTF-8 replaced (plan D46). Main
// thread only; the parent window owns the UpdateUi and outlives it.

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/updateProgressWindow.h"
#include "updater/core/updateResult.h"
#include "updater/core/updateSession.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::gtk {

// The update session's configuration for this installation (HC-34, HC-05): installed_tag
// context.release_tag, install_dir context.launcher_directory, launcher_relative "kyty_launcher"
// (the launcher of the official release archive), helper <install_dir>/kyty_updater, launcher_pid
// this process, working_dir context.working_directory. The session refuses an empty or relative
// path with its own message.
updater::UpdateSessionConfig UpdateSessionConfigFor(const LauncherContext& context);

// The page of the release tag: updater::kReleasesPage + "/tag/" + updater::PercentEncode(tag).
std::string ReleasePageUrl(std::string_view tag);

// A session message (updater::SessionOutcome::message) as shown: a fixed message of the updater as
// a translated text, anything else as it is, with invalid UTF-8 replaced (plan D46).
std::string UpdateMessageText(std::string_view message);

// The body of the previous-result message (HC-35): the message without surrounding white space,
// or the Qt launcher's default for the status when nothing is left ("The update was installed
// successfully." or "The update could not be installed.").
std::string PreviousResultText(const updater::PreviousResult& result);

enum class UpdateOfferResponse { Cancel, ReleaseNotes, Install };

// The offer (LFI-35): "Version <tag> is available" with the installed release, and the buttons
// Cancel (also Escape), Release notes, and Install update (the default). done runs once with the
// answer unless cancellable is cancelled first (alerts.h). Returns the alert's window.
GtkWindow* ShowUpdateOffer(GtkWindow* parent, std::string_view tag, std::string_view installed_tag,
                           std::function<void(UpdateOfferResponse response)> done,
                           GCancellable*                                     cancellable);

// The previous update's result (HC-35, 13.14): an alert with the window title "KytyPS5 Update",
// the heading "Update installed" or "Update failed" (the Qt launcher's information and warning
// boxes), and PreviousResultText as its body. Returns the alert's window.
GtkWindow* ShowPreviousUpdateResult(GtkWindow* parent, const updater::PreviousResult& result,
                                    GCancellable* cancellable);

class UpdateUi {
public:
	struct Callbacks {
		// The session's state or the run state's installation changed: the window updates its
		// actions (no game runs while an update installs) and the banner.
		std::function<void()> changed;
		// The helper took over (Handoff): quit now. Called from the main loop; it may destroy
		// the UpdateUi.
		std::function<void()> quit;
	};

	// What the harness and the UI tests look at.
	struct Statistics {
		uint64_t checks   = 0; // session checks started (at startup or user-activated)
		uint64_t installs = 0; // Install answers given to the session
		uint64_t offers   = 0; // offers shown
		uint64_t messages = 0; // other alerts shown (results, refusals, failures, no newer release)
		std::string                            last_heading; // of the last of those alerts
		std::string                            last_body;
		std::optional<updater::PreviousResult> previous_result; // what Start() read
		bool                                   previous_result_shown = false;
		bool                                   cleanup_finished      = false;
		std::vector<std::filesystem::path>     removed_workspaces;
		uint64_t                               cleanup_failures = 0;
		uint64_t                               quits            = 0;
	};

	// The update UI of context.update_mode for parent (the main window; its alerts and the
	// progress window are transient for it). Only Official builds create an UpdateSession, on
	// context.services (http, scheduler, process_runner) with UpdateSessionConfigFor(context).
	UpdateUi(LauncherContext& context, GtkWindow* parent, Callbacks callbacks);
	// Ends a running check or installation (the session removes its workspace) and closes the
	// alerts and the progress window; no callback runs afterwards.
	~UpdateUi();

	UpdateUi(const UpdateUi&)            = delete;
	UpdateUi& operator=(const UpdateUi&) = delete;

	// Once, at startup: the workspace cleanup and the previous result (Official and Other), and
	// the quiet check (Official, when check_updates_on_startup is set).
	void Start();

	// app.check-updates: Official builds while no check or installation runs.
	[[nodiscard]] bool CanCheck() const;
	// A user-activated check (nothing when CanCheck() is false).
	void Check();
	// app.open-releases-page: updater::kReleasesPage in the browser; an error is shown.
	void OpenReleasesPage();
	// The banner's button: the offer again while the session offers a release.
	void ShowOffer();

	// The offered release's tag while the session offers it (the banner), else nullopt.
	[[nodiscard]] std::optional<std::string> OfferedTag() const;
	// The session's state; nullopt without a session (Other and Flatpak builds).
	[[nodiscard]] std::optional<updater::SessionState> State() const;
	// The progress window while an installation runs, else nullptr.
	[[nodiscard]] KytyUpdateProgressWindow* ProgressWindow() const { return m_progress; }
	[[nodiscard]] const Statistics&         Stats() const { return m_stats; }

private:
	class Gate;

	static void     OnParentMapped(GtkWidget* parent, gpointer data);
	static gboolean OnResultIdle(gpointer data);
	static gboolean OnQuitIdle(gpointer data);
	static void     OnProgressCancel(KytyUpdateProgressWindow* window, gpointer data);
	static void     OnProgressDestroyed(GtkWidget* window, gpointer data);

	void StartCleanup();
	void StartCheck(bool user_activated);
	void OnChange(const updater::SessionOutcome& outcome);
	void OnOfferAnswer(UpdateOfferResponse response);
	void ShowMessage(const std::string& heading, const std::string& body);
	void ShowInstallFailure(const std::string& message);
	void ShowPreviousResult();
	void OpenPage(const std::string& url, const std::string& error_heading);
	void OpenProgress();
	void CloseProgress();
	void Changed();

	LauncherContext&         m_context;
	GtkWindow*               m_parent;
	Callbacks                m_callbacks;
	GObjectPtr<GCancellable> m_cancellable; // cancelled by the destructor: the alerts close
	std::shared_ptr<bool>    m_alive = std::make_shared<bool>(true);
	std::unique_ptr<Gate>    m_gate;
	// Official builds only; declared after the gate, which it calls until it is destroyed.
	std::unique_ptr<updater::UpdateSession> m_session;
	updater::SessionState                   m_state = updater::SessionState::Idle;
	// The offered release (kept after a failure, for "Open release page").
	std::optional<updater::ReleaseInfo> m_release;
	bool                                m_started    = false;
	bool                                m_installing = false; // Install was answered, not ended
	bool                                m_offer_open = false;
	bool                                m_destroying = false;
	KytyUpdateProgressWindow*           m_progress   = nullptr; // GTK owns it
	SignalScope                         m_progress_cancel;
	SignalScope                         m_progress_destroy;
	SignalScope                         m_parent_map;
	SourceId                            m_result_idle;
	SourceId                            m_quit_idle;
	Statistics                          m_stats;
};

} // namespace kyty::gtk
