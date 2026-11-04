// UI tests of the update UI in each of the three modes (task 22.5; Requirements 13.3, 13.5, 13.8,
// 13.10, 13.12-13.14; design.md LFI-33-LFI-35; phase 7 plan P7-HC-16, P7-HC-19, W4-26), each on
// Wayland and on X11. Every case runs the real KytyApplication (UiContext::RunApplication) with
// the mode set through the application's test hook (phase 7 plan D13): no build of these tests is
// an official build.
//
// Official cases (update_official_*, update_progress_*, update_previous_result) use
// KytyTests::FakeHttpClient on a KytyTests::FakeScheduler (a virtual clock: the 15 s and 30 s
// stalls take no time) and FakeProcessRunner, whose on_run hook shows the progress window while
// tar and the validation run. Every request goes to the fork's release URLs (or is the
// compatibility database); the loopback server gets none, so nothing reaches the network.
// - update_official_startup_check (13.5): the quiet check at startup offers a newer release; a
//   startup check without a newer release shows nothing.
// - update_official_offer_install (13.5, 13.7, LFI-35): the offer, the download progress in IEC
//   units, Preparing while tar and the validation run, the handoff to kyty_updater, and the quit.
// - update_official_no_newer (13.13), update_official_errors (13.8, 13.12: each failure's text,
//   once through a test catalog to show the translation), update_official_game_running (10.4).
// - update_progress_escape_downloading, update_progress_escape_restarting (W4-26, p6-u11-2):
//   Escape and the close button of the progress window.
// - update_previous_result (13.14): the previous installation's result, once.
// update_flatpak_nothing (13.3): no control, no setting, no request. update_other_releases_link
// (13.10): only the releases page link, the previous result, and the stale workspace cleanup.

#include "launcher/core/compatibility.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/updateProgressWindow.h"
#include "launcher/gtk/updateUi.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/testServices.h"
#include "support/updaterGenerators.h"
#include "ui/support/uiCatalog.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"
#include "ui/support/uiWindows.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateResult.h"
#include "updater/core/updateSession.h"

#include <adwaita.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
namespace gen  = KytyTests::UpdaterGen;
using kyty::updater::SessionState;
using kyty::updater::UpdateUiMode;
using KytyTests::FakeResponse;
using KytyUi::Check;
using std::chrono::milliseconds;

constexpr const char* kInstalled = "2026.03.01-abc1234";
constexpr const char* kLatest    = "2026.04.01-def5678";

const std::string& Package() {
	static const std::string package = gen::PatternBytes(262144, 7);
	return package;
}

gen::ReleaseSpec Release(std::string_view tag, std::string_view published_at) {
	return gen::ValidRelease(tag, published_at, gen::DigestOf(Package()), Package().size());
}

FakeResponse Ok(std::string body) {
	FakeResponse response;
	response.body = std::move(body);
	return response;
}

FakeResponse Status(int status) {
	FakeResponse response;
	response.status = status;
	return response;
}

std::string AssetUrl() {
	return kyty::updater::AssetDownloadUrl(kLatest, kyty::updater::AssetNameForTag(kLatest));
}

// An Official installation on a virtual clock: the case's TestServices over a FakeHttpClient and
// its FakeScheduler, the installed helper, and the compatibility database.
class OfficialWorld {
public:
	explicit OfficialWorld(KytyUi::UiContext& ui): m_ui(ui) {
		KytyUi::UseCaseEnvironment(ui, services);
		m_services.emplace(services.Get());
		Check(gen::WriteExecutable(ui.AppDirectory() / "kyty_updater", "#!/bin/sh\nexit 0\n"),
		      "the installed helper");
	}

	// A newer latest release (published after the installed one), the installed release, and the
	// package.
	void ScriptNewer(FakeResponse asset = Ok(Package())) {
		Latest(Ok(gen::ReleaseDocument(Release(kLatest, "2026-04-01T12:00:00Z"))));
		Installed();
		http.AddResponse(AssetUrl(), std::move(asset));
	}
	void Latest(FakeResponse response) {
		http.AddResponse(kyty::updater::LatestReleaseUrl(), std::move(response));
	}
	void Installed() {
		http.AddResponse(kyty::updater::ReleaseByTagUrl(kInstalled),
		                 Ok(gen::ReleaseDocument(Release(kInstalled, "2026-03-01T12:00:00Z"))));
	}

	// RunApplication in mode with these services and the installed release kInstalled; the
	// compatibility database answers once per run.
	int Run(const KytyUi::Drive& drive, UpdateUiMode mode = UpdateUiMode::Official) {
		http.AddResponse(core::kCompatibilityUrl, Ok(KytyUi::kFixtureCompatibility));
		return m_ui.RunApplication(
		    drive, {.update_mode = mode, .services = &*m_services, .release_tag = kInstalled});
	}

	// Lets the virtual clock run every timer that is due within ms, then the main loop.
	void Advance(int ms) {
		scheduler.AdvanceBy(milliseconds {ms});
		KytyUi::Pump(30);
	}
	void RunUntilIdle() {
		scheduler.RunUntilIdle();
		KytyUi::Pump(30);
	}

	// The library is there (the compatibility request needs the virtual clock).
	bool WaitForLibrary(KytyMainWindow* window) {
		auto& stats = kyty::gtk::MainWindowLibrary(window).Stats();
		return KytyUi::WaitFor(
		           [&]() {
			           scheduler.RunUntilIdle();
			           return stats.scans_applied > 0 && stats.compatibility_finished;
		           },
		           8000, "the library and the compatibility database") &&
		       KytyUi::WaitForLibrary(window);
	}

	// Every request was one of the fork's release URLs or the compatibility database, and none
	// reached the loopback server (the network).
	void CheckRequests() const {
		for (const auto& request: http.Requests()) {
			std::printf("%s: request %s\n", KytyUi::CaseName().c_str(), request.url.c_str());
			Check(request.url == core::kCompatibilityUrl ||
			          kyty::updater::IsAllowedUpdateUrl(request.url),
			      "a request to the fork's releases: " + request.url);
		}
		Check(m_ui.Server().Requests().empty(), "nothing reached the loopback server");
	}

	[[nodiscard]] std::size_t RequestsTo(std::string_view url) const {
		std::size_t count = 0;
		for (const auto& request: http.Requests()) {
			count += request.url == url ? 1 : 0;
		}
		return count;
	}

	KytyTests::FakeScheduler  scheduler;
	KytyTests::FakeHttpClient http {scheduler};
	KytyTests::TestServices   services {http, &scheduler};

private:
	KytyUi::UiContext&                 m_ui;
	std::optional<kyty::gtk::Services> m_services;
};

// The alert (not the main window, whose banner repeats the offer's heading) with text.
GtkWindow* Dialog(std::string_view text, KytyMainWindow* window, int timeout_ms = 3000) {
	GtkWindow* found = nullptr;
	KytyUi::WaitFor(
	    [&]() {
		    GListModel* toplevels = gtk_window_get_toplevels();
		    for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
			    auto candidate = kyty::gtk::GObjectPtr<GtkWindow>::Adopt(
			        static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
			    if (candidate.get() != GTK_WINDOW(window) &&
			        gtk_widget_get_mapped(GTK_WIDGET(candidate.get())) != FALSE &&
			        KytyUi::AllText(GTK_WIDGET(candidate.get())).find(text) != std::string::npos) {
				    found = candidate.get();
				    return true;
			    }
		    }
		    return false;
	    },
	    timeout_ms, "a dialog with \"" + std::string(text) + "\"");
	return found;
}

// The offer, after a user-activated check that the virtual clock answers.
GtkWindow* CheckAndOffer(OfficialWorld& world, KytyMainWindow* window) {
	gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
	world.RunUntilIdle();
	return Dialog(std::string("Current: ") + kInstalled, window);
}

bool BannerShown(KytyMainWindow* window) {
	GtkWidget* banner = KytyUi::FindByBuildableId(GTK_WIDGET(window), "update_banner");
	return banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE;
}

std::vector<fs::path> Workspaces(const fs::path& install) {
	std::vector<fs::path> found;
	std::error_code       ec;
	for (auto it = fs::directory_iterator(install, ec); !ec && it != fs::directory_iterator();
	     it.increment(ec)) {
		if (it->path().filename().string().starts_with(".kyty-update-") &&
		    it->path().filename() != ".kyty-update-result") {
			found.push_back(it->path());
		}
	}
	return found;
}

std::string ProgressText(KytyUpdateProgressWindow* progress) {
	auto* bar = GTK_PROGRESS_BAR(KytyUi::FindByType(GTK_WIDGET(progress), GTK_TYPE_PROGRESS_BAR));
	const char* text = bar != nullptr ? gtk_progress_bar_get_text(bar) : nullptr;
	return text != nullptr ? text : "";
}

// The user-activated check's failure alert of a session that the virtual clock ends.
void ExpectCheckFailure(OfficialWorld& world, KytyMainWindow* window, const std::string& body,
                        const std::string& name) {
	auto& update = kyty::gtk::MainWindowUpdate(window);
	KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "the session to be idle");
	gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
	world.RunUntilIdle();
	if (GtkWindow* alert = Dialog("Could not check for updates", window); alert != nullptr) {
		Check(KytyUi::AllText(GTK_WIDGET(alert)).find(body) != std::string::npos &&
		          update.Stats().last_body == body,
		      name + ": the alert says \"" + body + "\" (" + update.Stats().last_body + ")");
		Check(std::string_view(gtk_window_get_title(alert)) == "KytyPS5 Update",
		      name + ": the alert's window title");
		KytyUi::CloseError(alert);
	}
}

} // namespace

// The quiet check at startup (13.5): a newer release is offered with the banner; without a newer
// release nothing is shown.
KYTY_UI_CASE(update_official_startup_check) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	{
		OfficialWorld world(ui);
		world.ScriptNewer();
		const int status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
			auto& update = kyty::gtk::MainWindowUpdate(window);
			world.WaitForLibrary(window);
			Check(update.Stats().checks == 1, "one check at startup (the default setting)");
			world.RunUntilIdle();
			GtkWindow* offer = Dialog(std::string("Current: ") + kInstalled, window);
			if (offer != nullptr) {
				Check(KytyUi::AllText(GTK_WIDGET(offer))
				              .find(std::string("Version ") + kLatest + " is available") !=
				          std::string::npos,
				      "the offer names the newer release");
				Check(BannerShown(window), "the banner shows the offer");
				Check(KytyUi::Capture(offer, "offer"), "the offer renders");
				KytyUi::CloseAlert(offer, "Cancel");
			}
			KytyUi::Pump(100);
			Check(!BannerShown(window) && update.State() == SessionState::Failed,
			      "Cancel ends the offer");
			Check(world.RequestsTo(AssetUrl()) == 0, "nothing was downloaded");
			KytyUi::QuitApplication(application);
		});
		Check(status == 0, "the application ends with status 0");
		world.CheckRequests();
	}
	{
		OfficialWorld world(ui);
		world.Latest(Ok(gen::ReleaseDocument(Release(kInstalled, "2026-03-01T12:00:00Z"))));
		const int status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
			auto& update = kyty::gtk::MainWindowUpdate(window);
			world.WaitForLibrary(window);
			world.RunUntilIdle();
			KytyUi::Pump(300);
			Check(update.Stats().checks == 1 && update.State() == SessionState::NoNewer,
			      "the startup check finds no newer release");
			Check(update.Stats().messages == 0 && update.Stats().offers == 0 &&
			          KytyUi::ToplevelCount() == 1,
			      "a startup check without a newer release shows nothing");
			KytyUi::QuitApplication(application);
		});
		Check(status == 0, "the application ends with status 0");
		world.CheckRequests();
	}
}

// The offer, the download progress in IEC units, Preparing while tar and kyty_updater --validate
// run (FakeProcessRunner's on_run hook), the handoff (kyty_updater --install started detached),
// and the quit (13.5, 13.7, LFI-35).
KYTY_UI_CASE(update_official_offer_install) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	FakeResponse  asset  = Ok(Package());
	asset.chunk_sizes    = {131072};
	asset.chunk_interval = milliseconds {1000};
	world.ScriptNewer(asset);
	auto&                    runner = world.services.process_runner;
	std::vector<std::string> preparing;
	const int status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		auto& update = kyty::gtk::MainWindowUpdate(window);
		world.WaitForLibrary(window);
		Check(update.Stats().checks == 0, "no check at startup (switched off)");
		runner.on_run = [&](const fs::path& program) {
			KytyUpdateProgressWindow* progress = update.ProgressWindow();
			KytyUi::Pump(50);
			preparing.push_back(program.filename().string() + ": " +
			                    (progress != nullptr ? KytyUi::AllText(GTK_WIDGET(progress)) : ""));
			if (progress != nullptr && program.filename() == "tar") {
				KytyUi::Capture(GTK_WINDOW(progress), "preparing");
			}
		};
		GtkWindow* offer = CheckAndOffer(world, window);
		if (offer == nullptr) {
			return;
		}
		KytyUi::CloseAlert(offer, "Install update");
		world.Advance(0); // the first 128 KiB
		KytyUpdateProgressWindow* progress = update.ProgressWindow();
		if (!Check(progress != nullptr, "the progress window is open")) {
			return;
		}
		const std::string half = "128.0\xc2\xa0KiB of 256.0\xc2\xa0KiB";
		KytyUi::WaitFor([&]() { return ProgressText(progress) == half; }, 3000,
		                "the progress to read \"128.0 KiB of 256.0 KiB\"");
		Check(KytyUi::AllText(GTK_WIDGET(progress)).find(std::string("Downloading ") + kLatest) !=
		          std::string::npos,
		      "the window names the release");
		Check(KytyUi::Capture(GTK_WINDOW(progress), "download"), "the progress renders");
		Check(!KytyUi::ContextOf(application).run_state.CanRun(fs::path("/any")),
		      "no game can start while the update installs");
		world.RunUntilIdle(); // the rest, tar, the validation, and the handoff
		Check(update.State() == SessionState::Handoff, "the helper took over");
		KytyUi::WaitFor([&update]() { return update.Stats().quits == 1; }, 3000,
		                "the handoff to quit the launcher");
	});
	Check(status == 0, "the application quit after the handoff with status 0");
	Check(preparing.size() == 2 &&
	          preparing[0].find("Preparing the update…") != std::string::npos &&
	          preparing[1].find("Preparing the update…") != std::string::npos,
	      "Preparing is shown while tar and the validation run (" +
	          std::to_string(preparing.size()) + " runs)");
	const auto& calls = runner.calls;
	for (const auto& call: calls) {
		std::string line = call.program.string();
		for (const auto& argument: call.args) {
			line += " " + argument;
		}
		std::printf("%s: %s %s\n", KytyUi::CaseName().c_str(), call.detached ? "started" : "ran",
		            line.c_str());
	}
	Check(calls.size() == 3 && calls[0].program == "/usr/bin/tar" && !calls[0].detached &&
	          calls[1].args.size() > 0 && calls[1].args[0] == "--validate" && calls[2].detached &&
	          calls[2].args.size() == 6 && calls[2].args[0] == "--install" &&
	          calls[2].args[2] == ui.AppDirectory().string() && calls[2].args[3] == "kyty_launcher",
	      "tar, kyty_updater --validate, then kyty_updater --install started detached");
	world.CheckRequests();
}

// No newer release (13.13): the user-activated check says so and downloads nothing.
KYTY_UI_CASE(update_official_no_newer) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	world.Latest(Ok(gen::ReleaseDocument(Release(kLatest, "2026-02-01T12:00:00Z"))));
	world.Installed();
	const int status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		world.WaitForLibrary(window);
		gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
		world.RunUntilIdle();
		if (GtkWindow* alert = Dialog("No update available", window); alert != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(alert)).find("No newer release is available.") !=
			          std::string::npos,
			      "the alert says that no newer release is available");
			Check(KytyUi::Capture(alert, "no-newer"), "the alert renders");
			KytyUi::CloseError(alert);
		}
		Check(!BannerShown(window), "no offer");
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	Check(world.RequestsTo(AssetUrl()) == 0, "no download");
	world.CheckRequests();
}

// Each failure's text (13.8, 13.12): the check's HTTP status, a stall of 15 s, more than 2 MiB, an
// invalid release; the installation's package with a wrong digest (with Open release page), an
// HTTP status, a stall of 30 s; once through a test catalog, which the alert then shows.
KYTY_UI_CASE(update_official_errors) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	world.Latest(Status(500));
	FakeResponse stalled      = Ok(gen::ReleaseDocument(Release(kLatest, "2026-04-01T12:00:00Z")));
	stalled.stall_after_bytes = 10;
	world.Latest(stalled);
	world.Latest(Ok(std::string(2 * 1024 * 1024 + 1, ' ')));
	world.Latest(Ok("{\"tag_name\": "));
	world.Latest(Status(404)); // the run with the test catalog
	const auto& calls  = world.services.desktop_launcher.calls;
	const int   status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		auto& update = kyty::gtk::MainWindowUpdate(window);
		world.WaitForLibrary(window);
		ExpectCheckFailure(world, window, "The update check failed with HTTP status 500.",
		                   "HTTP 500");
		ExpectCheckFailure(world, window, "The update check received no data for 15 seconds.",
		                   "a stall");
		ExpectCheckFailure(world, window, "The update response is larger than 2 MiB.",
		                   "more than 2 MiB");
		ExpectCheckFailure(world, window, "Invalid GitHub release response.", "an invalid release");
		{
			const KytyUi::TestCatalog catalog(
			    ui.Root() / "locale",
			    {{"The update check failed with HTTP status %llu.",
			      "Die Update-Prüfung schlug mit dem HTTP-Status %llu fehl."},
			     {"Could not check for updates", "Keine Update-Prüfung möglich"}});
			KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "an idle session");
			gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
			world.RunUntilIdle();
			if (GtkWindow* alert = Dialog("Keine Update-Prüfung möglich", window);
			    alert != nullptr) {
				Check(KytyUi::AllText(GTK_WIDGET(alert))
				              .find("Die Update-Prüfung schlug mit dem HTTP-Status 404 fehl.") !=
				          std::string::npos,
				      "the alert shows the translated text: " + KytyUi::AllText(GTK_WIDGET(alert)));
				Check(KytyUi::Capture(alert, "translated"), "the translated alert renders");
				KytyUi::CloseError(alert);
			}
		}
		// The installation's failures, each after an offer.
		struct Failure {
			FakeResponse asset;
			std::string  body;
		};
		std::string corrupt     = Package();
		corrupt[1000]           = static_cast<char>(corrupt[1000] ^ 0x55);
		FakeResponse stall      = Ok(Package());
		stall.stall_after_bytes = 1000;
		for (auto& [asset, body]: std::vector<Failure> {
		         {Ok(corrupt), "The update failed its SHA-256 verification. Please try again."},
		         {Status(404), "The download failed with HTTP status 404."},
		         {stall, "The download received no data for 30 seconds."}}) {
			world.ScriptNewer(asset);
			KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "an idle session");
			if (GtkWindow* offer = CheckAndOffer(world, window); offer != nullptr) {
				KytyUi::CloseAlert(offer, "Install update");
			}
			world.RunUntilIdle();
			GtkWindow* failure = Dialog("The update could not be installed", window);
			if (failure == nullptr) {
				continue;
			}
			Check(KytyUi::AllText(GTK_WIDGET(failure)).find(body) != std::string::npos,
			      "the installation failure says \"" + body + "\"");
			Check(Workspaces(ui.AppDirectory()).empty(), "no workspace is left: " + body);
			if (body.starts_with("The update failed its SHA-256")) {
				Check(KytyUi::Capture(failure, "install-failed"), "the failure renders");
				const auto before = calls.size();
				KytyUi::CloseAlert(failure, "Open release page");
				KytyUi::WaitFor([&]() { return calls.size() > before; }, 3000,
				                "Open release page to open the release's page");
				Check(!calls.empty() &&
				          calls.back() == "OpenUri " + kyty::gtk::ReleasePageUrl(kLatest),
				      "Open release page opens the release's page");
			} else {
				KytyUi::CloseAlert(failure, "Close");
			}
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	world.CheckRequests();
}

// While a game runs (10.4): Install answers "Close the running game first." and the offer stays.
KYTY_UI_CASE(update_official_game_running) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	world.ScriptNewer();
	const KytyUi::FakeEmulatorBehavior fake("--stdout ready --sleep-ms 60000");
	const int status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		auto& context = KytyUi::ContextOf(application);
		world.WaitForLibrary(window);
		KytyUi::SelectTitle(context.library, KytyUi::kAlphaTitle);
		KytyUi::ActivateGameAction(window, "run");
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "the game to run");
		// The refusal opens while the offer closes, and may get the offer's address: the button
		// is clicked without waiting for the offer to be gone (uiDriver.h CloseAlert would).
		if (GtkWindow* offer = CheckAndOffer(world, window); offer != nullptr) {
			GtkWidget* install = KytyUi::ButtonLabelled(GTK_WIDGET(offer), {"Install update"});
			Check(install != nullptr, "the offer has Install update");
			if (install != nullptr) {
				g_signal_emit_by_name(install, "clicked");
			}
		}
		world.RunUntilIdle();
		if (GtkWindow* refusal = Dialog("The update cannot be installed now", window);
		    refusal != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(refusal)).find("Close the running game first.") !=
			          std::string::npos,
			      "the refusal asks to close the game first");
			Check(KytyUi::Capture(refusal, "refused"), "the refusal renders");
			KytyUi::CloseError(refusal);
		}
		auto& update = kyty::gtk::MainWindowUpdate(window);
		Check(update.State() == SessionState::Offering && BannerShown(window),
		      "the offer stays (the banner)");
		Check(world.RequestsTo(AssetUrl()) == 0 && update.ProgressWindow() == nullptr,
		      "nothing is downloaded");
		KytyUi::ActivateGameAction(window, "stop");
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	world.CheckRequests();
}

// W4-26 (p6-u11-2): Escape and the close button while the package downloads cancel the
// installation: the window closes, nothing is shown, no workspace is left, games can run again.
KYTY_UI_CASE(update_progress_escape_downloading) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	const int     status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		auto& update  = kyty::gtk::MainWindowUpdate(window);
		auto& context = KytyUi::ContextOf(application);
		world.WaitForLibrary(window);
		for (const bool escape: {true, false}) {
			const std::string how   = escape ? "Escape" : "the close button";
			FakeResponse      asset = Ok(Package());
			asset.stall_after_bytes = 131072; // the download stays in progress
			world.ScriptNewer(asset);
			KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "an idle session");
			GtkWindow* offer = CheckAndOffer(world, window);
			if (offer == nullptr) {
				return;
			}
			KytyUi::CloseAlert(offer, "Install update");
			world.Advance(0);
			KytyUpdateProgressWindow* progress = update.ProgressWindow();
			if (!Check(progress != nullptr && update.State() == SessionState::Downloading,
			           "downloading (" + how + ")")) {
				return;
			}
			GtkWindow* window_of_progress = GTK_WINDOW(progress);
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(progress));
			Check(!Workspaces(ui.AppDirectory()).empty(), "the workspace exists while it runs");
			const auto messages = update.Stats().messages;
			if (escape) {
				Check(KytyUi::KeyPressOn(GTK_WIDGET(progress), GDK_KEY_Escape),
				      "Escape is handled");
			} else {
				gtk_window_close(window_of_progress);
			}
			KytyUi::WaitClosed(window_of_progress, how + " to close the progress window");
			world.Advance(0);
			KytyUi::Pump(200);
			Check(update.ProgressWindow() == nullptr && update.State() == SessionState::Failed,
			      how + " cancels the installation");
			Check(update.Stats().messages == messages && KytyUi::ToplevelCount() == 1,
			      how + " shows nothing");
			Check(Workspaces(ui.AppDirectory()).empty(), how + " removes the workspace");
			Check(context.run_state.CanRun(fs::path("/any")), "games can run again (" + how + ")");
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	world.CheckRequests();
}

// W4-26 (p6-u11-2): once the helper took over, the progress window shows Restarting without
// Cancel; Escape and the close button change nothing, and the launcher quits.
KYTY_UI_CASE(update_progress_escape_restarting) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	OfficialWorld world(ui);
	world.ScriptNewer();
	const int status = world.Run([&](KytyApplication* /*application*/, KytyMainWindow* window) {
		auto& update = kyty::gtk::MainWindowUpdate(window);
		world.WaitForLibrary(window);
		GtkWindow* offer = CheckAndOffer(world, window);
		if (offer == nullptr) {
			return;
		}
		KytyUi::CloseAlert(offer, "Install update");
		// Everything up to the handoff, without the main loop: the quit has not run yet.
		world.scheduler.RunUntilIdle();
		KytyUpdateProgressWindow* progress = update.ProgressWindow();
		if (!Check(progress != nullptr && update.State() == SessionState::Handoff,
		           "the helper took over")) {
			return;
		}
		Check(KytyUi::AllText(GTK_WIDGET(progress)).find("Restarting to install the update…") !=
		          std::string::npos,
		      "the window says the launcher restarts");
		GtkWidget* cancel = KytyUi::FindByBuildableId(GTK_WIDGET(progress), "cancel_button");
		Check(cancel != nullptr && gtk_widget_get_sensitive(cancel) == FALSE, "no Cancel any more");
		KytyUi::KeyPressOn(GTK_WIDGET(progress), GDK_KEY_Escape);
		gtk_window_close(GTK_WINDOW(progress));
		Check(update.ProgressWindow() == progress && update.State() == SessionState::Handoff,
		      "Escape and the close button change nothing");
		Check(KytyUi::Capture(GTK_WINDOW(progress), "restarting"), "the window renders");
		KytyUi::WaitFor([&update]() { return update.Stats().quits == 1; }, 3000,
		                "the handoff to quit the launcher");
	});
	Check(status == 0, "the launcher quits after the handoff with status 0");
	world.CheckRequests();
}

// The previous installation's result (13.14): shown once at the next start, success and failure.
KYTY_UI_CASE(update_previous_result) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	const fs::path result = ui.AppDirectory() / ".kyty-update-result";
	struct Run {
		kyty::updater::PreviousResult previous;
		std::string                   heading;
		std::string                   body;
		const char*                   capture;
	};
	for (const auto& [previous, heading, body, capture]:
	     std::vector<Run> {{{true, "Updated to 2026.03.01."},
	                        "Update installed",
	                        "Updated to 2026.03.01.",
	                        "installed"},
	                       {{false, "Cannot replace kyty_launcher: Permission denied"},
	                        "Update failed",
	                        "Cannot replace kyty_launcher: Permission denied",
	                        "failed"},
	                       {{false, ""},
	                        "Update failed",
	                        "The update could not be installed.",
	                        "failed-default"}}) {
		Check(static_cast<bool>(kyty::updater::WriteUpdateResult(ui.AppDirectory(), previous)),
		      "write the previous result");
		OfficialWorld world(ui);
		const int     status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
			if (GtkWindow* alert = Dialog(heading, window); alert != nullptr) {
				Check(KytyUi::AllText(GTK_WIDGET(alert)).find(body) != std::string::npos,
				      heading + ": the body says \"" + body + "\"");
				Check(KytyUi::Capture(alert, capture), "the result renders");
				KytyUi::CloseError(alert);
			}
			Check(!KytyUi::Exists(result), "the result is read once (the file is gone)");
			world.WaitForLibrary(window);
			KytyUi::QuitApplication(application);
		});
		Check(status == 0, "the application ends with status 0");
	}
	// The next start shows nothing.
	OfficialWorld world(ui);
	const int     status = world.Run([&](KytyApplication* application, KytyMainWindow* window) {
		world.WaitForLibrary(window);
		KytyUi::Pump(300);
		Check(KytyUi::ToplevelCount() == 1 &&
		          !kyty::gtk::MainWindowUpdate(window).Stats().previous_result,
		      "without a result file nothing is shown");
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
}

// Flatpak builds (13.3): no update control, no update setting, no release request, and neither
// the previous result nor the workspaces are touched.
KYTY_UI_CASE(update_flatpak_nothing) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path result = ui.AppDirectory() / ".kyty-update-result";
	const fs::path stale  = ui.AppDirectory() / ".kyty-update-abc123";
	Check(static_cast<bool>(kyty::updater::WriteUpdateResult(ui.AppDirectory(), {true, "old"})),
	      "write a previous result");
	KytyUi::WriteText(stale / "package-Linux-x86_64.tar.gz", "old");
	const int status = ui.RunApplication(
	    [&](KytyApplication* application, KytyMainWindow* window) {
		    KytyUi::WaitForLibrary(window);
		    GtkWidget* root = GTK_WIDGET(window);
		    Check(
		        g_action_group_has_action(G_ACTION_GROUP(application), "check-updates") == FALSE &&
		            g_action_group_has_action(G_ACTION_GROUP(application), "open-releases-page") ==
		                FALSE,
		        "no update action");
		    for (const char* id: {"check_updates_link", "releases_link"}) {
			    GtkWidget* link = KytyUi::FindByBuildableId(root, id);
			    Check(link == nullptr || gtk_widget_get_visible(link) == FALSE,
			          std::string("no ") + id);
		    }
		    Check(!kyty::gtk::MainWindowUpdate(window).State(), "no update session");
		    g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		    auto* settings = KYTY_SETTINGS_WINDOW(
		        KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings"));
		    if (settings != nullptr) {
			    Check(KytyUi::RowTitled(GTK_WIDGET(settings), "Check for updates on startup") ==
			              nullptr,
			          "no update setting");
			    Check(!KytyUi::PageShown(GTK_WIDGET(settings), "updates"), "no Updates page");
			    KytyUi::CloseWithEscape(GTK_WINDOW(settings), "the global settings");
		    }
		    Check(KytyUi::Capture(GTK_WINDOW(window), "main-window"), "the main window renders");
		    KytyUi::QuitApplication(application);
	    },
	    UpdateUiMode::Flatpak);
	Check(status == 0, "the application ends with status 0");
	for (const auto& request: ui.Http().Requests()) {
		Check(!request.url.starts_with(kyty::updater::kReleaseApiPrefix) &&
		          !request.url.starts_with(kyty::updater::kReleaseDownloadPrefix),
		      "no release request: " + request.url);
	}
	Check(KytyUi::Exists(result) && KytyUi::Exists(stale),
	      "the previous result and the workspace stay as they are");
}

// Other builds (13.10): only the releases page link (footer and primary menu), no check and no
// request; the previous result once; stale update workspaces are removed, recent ones kept.
KYTY_UI_CASE(update_other_releases_link) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path stale  = ui.AppDirectory() / ".kyty-update-old123";
	const fs::path recent = ui.AppDirectory() / ".kyty-update-new456";
	KytyUi::WriteText(stale / "package-Linux-x86_64.tar.gz", "old");
	KytyUi::WriteText(recent / "package-Linux-x86_64.tar.gz", "new");
	std::error_code ec;
	const auto      old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
	fs::last_write_time(stale / "package-Linux-x86_64.tar.gz", old_time, ec);
	fs::last_write_time(stale, old_time, ec);
	Check(static_cast<bool>(kyty::updater::WriteUpdateResult(
	          ui.AppDirectory(), {false, "Cannot replace kyty_launcher: Permission denied"})),
	      "write a previous result");
	const auto& calls  = ui.Services().desktop_launcher.calls;
	const int   status = ui.RunApplication(
	    [&](KytyApplication* application, KytyMainWindow* window) {
		    auto& update = kyty::gtk::MainWindowUpdate(window);
		    if (GtkWindow* alert = Dialog("Update failed", window); alert != nullptr) {
			    Check(KytyUi::AllText(GTK_WIDGET(alert))
			                  .find("Cannot replace kyty_launcher: Permission denied") !=
			              std::string::npos,
			          "the previous result's reason");
			    KytyUi::CloseError(alert);
		    }
		    KytyUi::WaitForLibrary(window);
		    KytyUi::WaitFor([&update]() { return update.Stats().cleanup_finished; }, 5000,
		                    "the workspace cleanup");
		    Check(update.Stats().removed_workspaces == std::vector<fs::path> {stale},
		          "the stale workspace is removed");
		    GtkWidget* root = GTK_WIDGET(window);
		    Check(
		        g_action_group_has_action(G_ACTION_GROUP(application), "check-updates") == FALSE &&
		            g_action_group_has_action(G_ACTION_GROUP(application), "open-releases-page") !=
		                FALSE,
		        "only the releases page action");
		    GtkWidget* link = KytyUi::FindByBuildableId(root, "releases_link");
		    Check(link != nullptr && gtk_widget_get_visible(link) != FALSE &&
		              gtk_widget_get_visible(
		                  KytyUi::FindByBuildableId(root, "check_updates_link")) == FALSE,
		          "the footer has the releases page link and no update check");
		    Check(!update.State() && !update.CanCheck(), "no update session");
		    Check(KytyUi::Activate(link), "a click on Releases page");
		    KytyUi::WaitFor([&calls]() { return !calls.empty(); }, 3000, "the releases page");
		    Check(!calls.empty() &&
		              calls.back() == "OpenUri " + std::string(kyty::updater::kReleasesPage),
		          "the link opens the releases page");
		    gtk_widget_activate_action(root, "app.open-releases-page", nullptr);
		    KytyUi::WaitFor([&calls]() { return calls.size() == 2; }, 3000,
		                    "the primary menu's item to open it again");
		    Check(KytyUi::Capture(GTK_WINDOW(window), "main-window"), "the main window renders");
		    KytyUi::QuitApplication(application);
	    },
	    UpdateUiMode::Other);
	Check(status == 0, "the application ends with status 0");
	Check(!KytyUi::Exists(stale) && KytyUi::Exists(recent),
	      "the stale workspace is gone, the recent one stays");
	Check(!KytyUi::Exists(ui.AppDirectory() / ".kyty-update-result"), "the result is read once");
	for (const auto& request: ui.Http().Requests()) {
		Check(!request.url.starts_with(kyty::updater::kReleaseApiPrefix) &&
		          !request.url.starts_with(kyty::updater::kReleaseDownloadPrefix),
		      "no release request: " + request.url);
	}
}
