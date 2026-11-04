#include "launcher/gtk/updateUi.h"

#include "launcher/core/text.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/backgroundTask.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/desktopLauncher.h"
#include "updater/core/downloadVerifier.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/workspaceCleanup.h"

#include <charconv>
#include <cstdarg>
#include <initializer_list>
#include <optional>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace kyty::gtk {
namespace {

namespace fs = std::filesystem;
using updater::SessionState;
using updater::UpdateUiMode;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

const char* ModeName(UpdateUiMode mode) {
	switch (mode) {
		case UpdateUiMode::Official: return "Official";
		case UpdateUiMode::Flatpak: return "Flatpak";
		case UpdateUiMode::Other: return "Other";
	}
	return "?";
}

const char* StateName(SessionState state) {
	switch (state) {
		case SessionState::Idle: return "Idle";
		case SessionState::FetchingLatest: return "FetchingLatest";
		case SessionState::FetchingInstalled: return "FetchingInstalled";
		case SessionState::Offering: return "Offering";
		case SessionState::NoNewer: return "NoNewer";
		case SessionState::Downloading: return "Downloading";
		case SessionState::Extracting: return "Extracting";
		case SessionState::Validating: return "Validating";
		case SessionState::Handoff: return "Handoff";
		case SessionState::Failed: return "Failed";
	}
	return "?";
}

// A check or an installation step runs, or the helper took over.
bool Busy(SessionState state) {
	switch (state) {
		case SessionState::FetchingLatest:
		case SessionState::FetchingInstalled:
		case SessionState::Downloading:
		case SessionState::Extracting:
		case SessionState::Validating:
		case SessionState::Handoff: return true;
		case SessionState::Idle:
		case SessionState::Offering:
		case SessionState::NoNewer:
		case SessionState::Failed: break;
	}
	return false;
}

// The updater's fixed messages and their translatable texts (plan D46; phase 7 plan D28: the
// launcher_gtk_update_message_text test drives a real updater::UpdateSession into each of them).
struct FixedMessage {
	std::string_view core;
	const char*      text;
};

const FixedMessage kFixedMessages[] = {
    {updater::kNoNewerReleaseMessage, N_("No newer release is available.")},
    {updater::kUnknownReleaseOrderMessage,
     N_("Could not determine whether the available release is newer.")},
    {updater::kCloseGameMessage, N_("Close the running game first.")},
    {updater::kGameRunningCancelledMessage,
     N_("The update was cancelled. Close the running game first.")},
    {updater::kInvalidReleaseMessage, N_("Invalid GitHub release response.")},
    {updater::kUnstableReleaseMessage, N_("The release is not a published stable release.")},
    {updater::kInvalidAssetMessage, N_("The update package size or SHA-256 checksum is invalid.")},
    {updater::kMissingAssetMessage, N_("This release has no update package for your platform.")},
    {updater::kDownloadTooLargeMessage,
     N_("The downloaded file is larger than the release asset.")},
    {updater::kDownloadDigestMessage,
     N_("The update failed its SHA-256 verification. Please try again.")},
    {updater::kDownloadAbortedMessage, N_("The download was cancelled.")},
    // updater/core/updateSession.cpp writes these sentences without exporting them
    // (kMetadataTooLargeMessage, DescribeMetadataFailure, DescribeDownloadFailure).
    {"The update response is larger than 2 MiB.", N_("The update response is larger than 2 MiB.")},
    {"The update check timed out.", N_("The update check timed out.")},
    {"The update check was cancelled.", N_("The update check was cancelled.")},
    {"The download timed out.", N_("The download timed out.")},
};

// The updater's sentences with numbers (updater/core/updateSession.cpp DescribeMetadataFailure and
// DescribeDownloadFailure, updater/core/downloadVerifier.cpp Finish): the words around the numbers
// (words[0], a number, words[1], ..., a number, the last word) and the translatable text, whose
// last number picks the plural form.
struct NumberedMessage {
	std::vector<std::string_view> words;
	PluralText                    text;
};

const NumberedMessage kNumberedMessages[] = {
    {{"The update check failed with HTTP status ", "."},
     // TRANSLATORS: %llu is an HTTP status code, for example 404.
     {N_("The update check failed with HTTP status %llu."), nullptr}},
    {{"The download failed with HTTP status ", "."},
     // TRANSLATORS: %llu is an HTTP status code, for example 404.
     {N_("The download failed with HTTP status %llu."), nullptr}},
    {{"The update check received no data for ", " seconds."},
     NGettextNoop("The update check received no data for %llu second.",
                  "The update check received no data for %llu seconds.")},
    {{"The download received no data for ", " seconds."},
     NGettextNoop("The download received no data for %llu second.",
                  "The download received no data for %llu seconds.")},
    {{"The downloaded file is smaller than the release asset (", " of ", " bytes)."},
     // TRANSLATORS: the bytes received so far and the size of the update package.
     NGettextNoop("The downloaded file is smaller than the release asset (%llu of %llu byte).",
                  "The downloaded file is smaller than the release asset (%llu of %llu bytes).")},
};

// The numbers of message when it is words[0], a number, words[1], ..., a number, words.back()
// (each number in decimal digits); otherwise nothing.
std::vector<uint64_t> NumbersIn(std::string_view                     message,
                                const std::vector<std::string_view>& words) {
	std::vector<uint64_t> numbers;
	bool                  first = true;
	for (const auto word: words) {
		if (!std::exchange(first, false)) {
			uint64_t   value     = 0;
			const auto end       = message.data() + message.size();
			const auto [next, e] = std::from_chars(message.data(), end, value);
			if (e != std::errc {}) {
				return {};
			}
			numbers.push_back(value);
			message.remove_prefix(static_cast<size_t>(next - message.data()));
		}
		if (!message.starts_with(word)) {
			return {};
		}
		message.remove_prefix(word.size());
	}
	return message.empty() ? numbers : std::vector<uint64_t> {};
}

// The translated text of key with its numbers.
std::string KeyText(const UpdateMessageKey& key) {
	using Count               = unsigned long long;
	const auto&       numbers = key.numbers;
	const char* const text =
	    key.plural_msgid != nullptr && !numbers.empty()
	        ? NGettext(key.msgid, key.plural_msgid, static_cast<unsigned long>(numbers.back()))
	        : _(key.msgid);
	switch (numbers.size()) {
		case 0: return text;
		case 1: return Format(text, static_cast<Count>(numbers[0]));
		default:
			return Format(text, static_cast<Count>(numbers[0]), static_cast<Count>(numbers[1]));
	}
}

// Every update alert has the window title of the Qt launcher's update message boxes.
GtkWindow* Titled(GtkWindow* window) {
	if (window != nullptr) {
		gtk_window_set_title(window, _("KytyPS5 Update"));
	}
	return window;
}

} // namespace

// ---- The RunState adapter (HC-34: one to one; the window hears about every change)

class UpdateUi::Gate final: public updater::InstallGate {
public:
	Gate(core::RunState& run_state, std::function<void()> changed)
	    : m_run_state(run_state), m_changed(std::move(changed)) {}

	bool CanInstallUpdate() const override { return m_run_state.CanInstallUpdate(); }
	void InstallStarted() override {
		m_run_state.InstallStarted();
		m_changed();
	}
	void InstallFinished() override {
		m_run_state.InstallFinished();
		m_changed();
	}

private:
	core::RunState&       m_run_state;
	std::function<void()> m_changed;
};

// ---- Free functions

updater::UpdateSessionConfig UpdateSessionConfigFor(const LauncherContext& context) {
	updater::UpdateSessionConfig config;
	config.installed_tag     = context.release_tag;
	config.install_dir       = context.launcher_directory;
	config.launcher_relative = "kyty_launcher";
	config.helper            = context.launcher_directory / updater::kHelperFileName;
	config.launcher_pid      = static_cast<int64_t>(::getpid());
	config.working_dir       = context.working_directory;
	return config;
}

std::string ReleasePageUrl(std::string_view tag) {
	return std::string(updater::kReleasesPage) + "/tag/" + updater::PercentEncode(tag);
}

std::optional<UpdateMessageKey> UpdateMessageKeyFor(std::string_view message) {
	for (const auto& fixed: kFixedMessages) {
		if (message == fixed.core) {
			return UpdateMessageKey {fixed.text, nullptr, {}};
		}
	}
	for (const auto& numbered: kNumberedMessages) {
		auto numbers = NumbersIn(message, numbered.words);
		if (!numbers.empty() && numbers.size() + 1 == numbered.words.size()) {
			return UpdateMessageKey {numbered.text.singular, numbered.text.plural,
			                         std::move(numbers)};
		}
	}
	return std::nullopt;
}

std::string UpdateMessageText(std::string_view message) {
	if (const auto key = UpdateMessageKeyFor(message)) {
		return KeyText(*key);
	}
	// "Could not verify the currently installed release. <reason>"
	const std::string unverified = std::string(updater::kInstalledReleaseUnverifiedMessage) + " ";
	if (message.starts_with(unverified)) {
		const std::string reason = UpdateMessageText(message.substr(unverified.size()));
		// TRANSLATORS: %s is the reason (an English message of the updater or of the network).
		return Format(_("Could not verify the currently installed release. %s"), reason.c_str());
	}
	return core::SanitizeUtf8(message);
}

std::string PreviousResultText(const updater::PreviousResult& result) {
	const std::string sanitized = core::SanitizeUtf8(result.message);
	const auto        trimmed   = core::TrimWhitespace(sanitized);
	if (!trimmed.empty()) {
		return std::string(trimmed);
	}
	return result.success ? _("The update was installed successfully.")
	                      : _("The update could not be installed.");
}

GtkWindow* ShowUpdateOffer(GtkWindow* parent, std::string_view tag, std::string_view installed_tag,
                           std::function<void(UpdateOfferResponse response)> done,
                           GCancellable*                                     cancellable) {
	const std::string latest(tag);
	const std::string installed(installed_tag);
	// TRANSLATORS: %s is the tag of the newer release.
	const std::string heading = Format(_("Version %s is available"), latest.c_str());
	const std::string body =
	    // TRANSLATORS: %s is the tag of the installed release.
	    Format(_("Current: %s\n\nDownload and install it now? The launcher will restart. Your "
	             "settings and games are kept."),
	           installed.c_str());
	const std::vector<std::string> labels = {_("Cancel"), _("Release notes"), _("Install update")};
	return Titled(Choose(
	    parent, heading, body, labels, 0, 2,
	    [done = std::move(done)](int index) {
		    if (done) {
			    done(index == 2   ? UpdateOfferResponse::Install
			         : index == 1 ? UpdateOfferResponse::ReleaseNotes
			                      : UpdateOfferResponse::Cancel);
		    }
	    },
	    cancellable));
}

GtkWindow* ShowPreviousUpdateResult(GtkWindow* parent, const updater::PreviousResult& result,
                                    GCancellable* cancellable) {
	const char* heading = result.success ? _("Update installed") : _("Update failed");
	return Titled(ShowError(parent, heading, PreviousResultText(result), cancellable));
}

// ---- UpdateUi

UpdateUi::UpdateUi(LauncherContext& context, GtkWindow* parent, Callbacks callbacks)
    : m_context(context), m_parent(parent), m_callbacks(std::move(callbacks)),
      m_cancellable(GObjectPtr<GCancellable>::Adopt(g_cancellable_new())),
      m_gate(std::make_unique<Gate>(context.run_state, [this]() { Changed(); })) {
	if (context.update_mode == UpdateUiMode::Official) {
		m_session = std::make_unique<updater::UpdateSession>(
		    UpdateSessionConfigFor(context), context.services.http, context.services.scheduler,
		    context.services.process_runner, *m_gate,
		    [this](const updater::SessionOutcome& outcome) { OnChange(outcome); });
	}
}

UpdateUi::~UpdateUi() {
	m_destroying = true;
	*m_alive     = false;
	g_cancellable_cancel(m_cancellable.get());
	m_result_idle.Remove();
	m_quit_idle.Remove();
	m_parent_map.Disconnect();
	// A running installation ends at the gate (InstallFinished) and its workspace is removed.
	m_session.reset();
	CloseProgress();
}

void UpdateUi::Start() {
	if (m_started) {
		return;
	}
	m_started             = true;
	const auto&     mode  = m_context.update_mode;
	const fs::path& where = m_context.launcher_directory;
	g_debug("update: %s mode, installation %s, release %s", ModeName(mode), where.c_str(),
	        m_context.release_tag.empty() ? "(none)" : m_context.release_tag.c_str());
	if (mode == UpdateUiMode::Flatpak) {
		return; // 13.3: nothing at all
	}
	if (where.empty() || !where.is_absolute()) {
		g_message("update: the installation directory is unknown, so stale update workspaces are "
		          "not removed and no previous update result is read");
	} else {
		StartCleanup();
		m_stats.previous_result = updater::ReadAndClearPreviousResult(where);
		if (const auto& result = m_stats.previous_result) {
			g_debug("update: previous result (%s): %s", result->success ? "success" : "failure",
			        PreviousResultText(*result).c_str());
			if (m_parent == nullptr || gtk_widget_get_mapped(GTK_WIDGET(m_parent)) != FALSE) {
				m_result_idle = SourceId(g_idle_add(&UpdateUi::OnResultIdle, this));
			} else {
				m_parent_map = SignalScope::Connect(m_parent, "map",
				                                    G_CALLBACK(&UpdateUi::OnParentMapped), this);
			}
		} else {
			g_debug("update: no previous update result in %s", where.c_str());
		}
	}
	if (mode == UpdateUiMode::Official &&
	    m_context.settings.loaded.settings.check_updates_on_startup) {
		StartCheck(false);
	}
}

bool UpdateUi::CanCheck() const {
	return m_session && !Busy(m_state);
}

void UpdateUi::Check() {
	if (CanCheck()) {
		StartCheck(true);
	}
}

void UpdateUi::OpenReleasesPage() {
	OpenPage(std::string(updater::kReleasesPage), _("The releases page could not be opened"));
}

void UpdateUi::ShowOffer() {
	if (m_offer_open || !m_session || m_state != SessionState::Offering || !m_release) {
		return;
	}
	m_offer_open = true;
	m_stats.offers++;
	g_debug("update: offering %s", m_release->tag.c_str());
	ShowUpdateOffer(
	    m_parent, m_release->tag, m_context.release_tag,
	    [this](UpdateOfferResponse response) {
		    m_offer_open = false;
		    OnOfferAnswer(response);
	    },
	    m_cancellable.get());
}

std::optional<std::string> UpdateUi::OfferedTag() const {
	if (m_state == SessionState::Offering && m_release) {
		return m_release->tag;
	}
	return std::nullopt;
}

std::optional<updater::SessionState> UpdateUi::State() const {
	if (!m_session) {
		return std::nullopt;
	}
	return m_session->State();
}

void UpdateUi::OnParentMapped(GtkWidget* /*parent*/, gpointer data) {
	auto* self = static_cast<UpdateUi*>(data);
	self->m_parent_map.Disconnect();
	// From the main loop, after the window is on the screen.
	self->m_result_idle = SourceId(g_idle_add(&UpdateUi::OnResultIdle, self));
}

gboolean UpdateUi::OnResultIdle(gpointer data) {
	static_cast<UpdateUi*>(data)->ShowPreviousResult();
	return G_SOURCE_REMOVE;
}

gboolean UpdateUi::OnQuitIdle(gpointer data) {
	// The callback may destroy this object.
	const auto quit = static_cast<UpdateUi*>(data)->m_callbacks.quit;
	if (quit) {
		quit();
	}
	return G_SOURCE_REMOVE;
}

void UpdateUi::OnProgressCancel(KytyUpdateProgressWindow* /*window*/, gpointer data) {
	auto* self = static_cast<UpdateUi*>(data);
	if (self->m_session) {
		g_debug("update: cancel requested in %s", StateName(self->m_state));
		self->m_session->Cancel(); // Failed without a message: the window closes, nothing shown
	}
}

void UpdateUi::OnProgressDestroyed(GtkWidget* /*window*/, gpointer data) {
	static_cast<UpdateUi*>(data)->m_progress = nullptr;
}

void UpdateUi::StartCleanup() {
	const fs::path where  = m_context.launcher_directory;
	auto           result = std::make_shared<updater::WorkspaceCleanup>();
	// An unowned GTask, a recorded exception (p6-u9-6): the alive flag drops a late result.
	RunInBackground(
	    [where, result]() {
		    *result = updater::CleanupStaleWorkspaces(where, fs::file_time_type::clock::now());
	    },
	    [this, alive = m_alive, result]() {
		    if (!*alive) {
			    return;
		    }
		    for (const auto& path: result->removed) {
			    g_debug("update: removed the stale workspace %s", path.c_str());
		    }
		    for (const auto& [path, error]: result->failed) {
			    g_message("update: cannot remove the stale workspace %s: %s", path.c_str(),
			              error.message().c_str());
		    }
		    m_stats.removed_workspaces = result->removed;
		    m_stats.cleanup_failures   = result->failed.size();
		    m_stats.cleanup_finished   = true;
		    g_debug("update: workspace cleanup finished, %zu removed, %zu failed",
		            result->removed.size(), result->failed.size());
	    });
}

void UpdateUi::StartCheck(bool user_activated) {
	if (!m_session) {
		return;
	}
	m_installing = false;
	m_stats.checks++;
	g_debug("update: check started (%s)", user_activated ? "user-activated" : "at startup");
	m_session->Check(user_activated);
}

void UpdateUi::OnChange(const updater::SessionOutcome& outcome) {
	const auto previous = std::exchange(m_state, outcome.state);
	if (outcome.release) {
		m_release = outcome.release;
	}
	if (outcome.state != previous) {
		g_debug("update: %s -> %s%s%s", StateName(previous), StateName(outcome.state),
		        outcome.message.empty() ? "" : ": ", outcome.message.c_str());
	}
	switch (outcome.state) {
		case SessionState::Idle:
		case SessionState::FetchingLatest:
		case SessionState::FetchingInstalled: break;
		case SessionState::Offering:
			if (!outcome.message.empty()) {
				// 10.4: Install was refused while a game runs; the offer stays (the banner).
				m_installing = false;
				ShowMessage(_("The update cannot be installed now"),
				            UpdateMessageText(outcome.message));
			} else if (previous != SessionState::Offering) {
				ShowOffer();
			}
			break;
		case SessionState::NoNewer:
			// 13.13: only for a user-activated check.
			if (outcome.user_activated) {
				ShowMessage(_("No update available"), UpdateMessageText(outcome.message));
			}
			break;
		case SessionState::Downloading:
			OpenProgress();
			kyty_update_progress_window_set_downloaded(
			    m_progress, m_session->DownloadedBytes(),
			    m_release && m_release->asset ? m_release->asset->size : 0);
			break;
		case SessionState::Extracting:
		case SessionState::Validating:
			OpenProgress();
			kyty_update_progress_window_set_preparing(m_progress);
			break;
		case SessionState::Handoff:
			// The helper waits for this process to exit: quit at once (from the main loop, never
			// inside the session's callback).
			m_installing = false;
			OpenProgress();
			kyty_update_progress_window_set_restarting(m_progress);
			m_stats.quits++;
			m_quit_idle = SourceId(g_idle_add(&UpdateUi::OnQuitIdle, this));
			break;
		case SessionState::Failed: {
			CloseProgress();
			const bool installing = std::exchange(m_installing, false);
			if (outcome.message.empty()) {
				break; // Cancel: nothing to say
			}
			if (installing) {
				ShowInstallFailure(outcome.message); // always (13.8)
			} else if (outcome.user_activated) {
				// 13.12: only for a user-activated check.
				ShowMessage(_("Could not check for updates"), UpdateMessageText(outcome.message));
			} else {
				g_message("update: the check at startup failed: %s",
				          core::SanitizeUtf8(outcome.message).c_str());
			}
			break;
		}
	}
	if (outcome.state != previous) {
		Changed();
	}
}

void UpdateUi::OnOfferAnswer(UpdateOfferResponse response) {
	// The session may have moved on meanwhile (a new check).
	if (!m_session || m_state != SessionState::Offering || !m_release) {
		return;
	}
	switch (response) {
		case UpdateOfferResponse::Install:
			g_debug("update: install %s", m_release->tag.c_str());
			m_installing = true;
			m_stats.installs++;
			m_session->Install();
			break;
		case UpdateOfferResponse::ReleaseNotes:
			// The offer stays: the banner shows it again.
			OpenPage(ReleasePageUrl(m_release->tag), _("The release page could not be opened"));
			break;
		case UpdateOfferResponse::Cancel:
			g_debug("update: offer of %s declined", m_release->tag.c_str());
			m_session->Cancel();
			break;
	}
}

void UpdateUi::ShowMessage(const std::string& heading, const std::string& body) {
	m_stats.messages++;
	m_stats.last_heading = heading;
	m_stats.last_body    = body;
	Titled(ShowError(m_parent, heading, body, m_cancellable.get()));
}

void UpdateUi::ShowInstallFailure(const std::string& message) {
	const std::string heading = _("The update could not be installed");
	const std::string body    = UpdateMessageText(message);
	g_message("update: installation failed: %s", body.c_str());
	if (!m_release || m_release->tag.empty()) {
		ShowMessage(heading, body);
		return;
	}
	m_stats.messages++;
	m_stats.last_heading   = heading;
	m_stats.last_body      = body;
	const std::string page = ReleasePageUrl(m_release->tag);
	Titled(Confirm(
	    m_parent, heading, body, _("Open release page"), _("Close"), false,
	    [this, page](bool open) {
		    if (open) {
			    OpenPage(page, _("The release page could not be opened"));
		    }
	    },
	    m_cancellable.get()));
}

void UpdateUi::ShowPreviousResult() {
	if (!m_stats.previous_result || m_stats.previous_result_shown) {
		return;
	}
	m_stats.previous_result_shown = true;
	const auto& result            = *m_stats.previous_result;
	m_stats.messages++;
	m_stats.last_heading = result.success ? _("Update installed") : _("Update failed");
	m_stats.last_body    = PreviousResultText(result);
	ShowPreviousUpdateResult(m_parent, result, m_cancellable.get());
	g_debug("update: previous result shown");
}

void UpdateUi::OpenPage(const std::string& url, const std::string& error_heading) {
	g_debug("update: opening %s", url.c_str());
	m_context.services.desktop_launcher.OpenUri(
	    m_parent, url,
	    [this, error_heading](const std::string& error) {
		    if (!error.empty()) {
			    ShowMessage(error_heading, error);
		    }
	    },
	    m_cancellable.get());
}

void UpdateUi::OpenProgress() {
	if (m_progress != nullptr) {
		return;
	}
	m_progress         = kyty_update_progress_window_new(m_parent, m_release ? m_release->tag : "");
	m_progress_cancel  = SignalScope::Connect(m_progress, "cancel-requested",
	                                          G_CALLBACK(&UpdateUi::OnProgressCancel), this);
	m_progress_destroy = SignalScope::Connect(m_progress, "destroy",
	                                          G_CALLBACK(&UpdateUi::OnProgressDestroyed), this);
	gtk_window_present(GTK_WINDOW(m_progress));
}

void UpdateUi::CloseProgress() {
	KytyUpdateProgressWindow* window = std::exchange(m_progress, nullptr);
	m_progress_cancel.Disconnect();
	m_progress_destroy.Disconnect();
	if (window != nullptr) {
		gtk_window_destroy(GTK_WINDOW(window));
	}
}

void UpdateUi::Changed() {
	if (!m_destroying && m_callbacks.changed) {
		m_callbacks.changed();
	}
}

} // namespace kyty::gtk
