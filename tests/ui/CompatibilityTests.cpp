// UI tests of the compatibility statuses (task 22.4; Requirements 8.1-8.6; LFI-21; phase 7 plan
// D15, P7-HC-14), each on Wayland and on X11. The database comes from the loopback server
// (LoopbackHttpClient over the real SoupHttpClient) or, for the retry schedule, from a
// FakeHttpClient on a FakeScheduler's virtual clock (plan D12); nothing reaches the network.
// - compat_statuses_within_1s: every listed game shows the status of the database entry whose
//   title ID matches (trimmed, any letter case) within 1 s of the load, Unknown otherwise, without
//   a rescan; a game found by a later rescan gets its status without another download (8.5).
// - compat_cache_fallback: every download fails, the cache in the Data_Directory parses: its
//   statuses are shown and the cache stays as it was (8.3).
// - compat_offline_warning: every download fails and there is no cache: Unknown everywhere,
//   exactly one 8.4 warning (declared with ExpectLog), and the launcher keeps working (8.4).
// - compat_retry_schedule: a network failure, two server errors, then a database: the retries
//   wait 750, 1500, and 2250 ms, and with four failures no fifth request is made (8.2).

#include "launcher/core/compatibility.h"
#include "launcher/core/dataPaths.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/testServices.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using KytyUi::Check;
using Status = core::CompatibilityStatus;

constexpr const char* kOfflineWarning = "No compatibility database is available";

// The fixture games' statuses in kDatabase (title IDs written as a maintainer might).
constexpr const char* kDatabase =
    "{\"PPSA01001\": {\"status\": \"InGame\", \"comment\": \"Plays well\"},"
    " \"ppsa01002\": {\"status\": \"Main menu\"},"
    " \" PPSA01004 \": {\"status\": \"Logo\", \"comment\": \"Stops at the logo\"},"
    " \"PPSA01005\": {\"status\": \"DoesntBoot\"}}";

const std::map<std::string, Status> kExpected = {{"Alpha Game", Status::InGame},
                                                 {"Beta Quest", Status::MainMenu},
                                                 {"Gamma Archive", Status::Logo},
                                                 {"Broken Metadata", Status::Unknown}};

// Every game of the library has its expected status (Unknown when it is not in expected).
bool StatusesShown(kyty::gtk::GameListModel& library, const std::map<std::string, Status>& expected,
                   std::string* wrong = nullptr) {
	GListModel* games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game = kyty::gtk::GObjectPtr<KytyGame>::Adopt(
		    static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		const std::string& title = kyty_game_get_entry(game.get()).title_name;
		const auto         found = expected.find(title);
		const Status       want  = found != expected.end() ? found->second : Status::Unknown;
		if (kyty_game_get_status(game.get()) != want) {
			if (wrong != nullptr) {
				*wrong = title;
			}
			return false;
		}
	}
	return true;
}

// The list's Status cells show the statuses (what the user sees).
bool StatusTextsDrawn(KytyMainWindow* window, kyty::gtk::GameListModel& library) {
	for (const auto& title: KytyUi::ShownTitles(library)) {
		const auto cells = KytyUi::RowCells(KytyUi::ListViewOf(window), title);
		KytyGame*  game  = KytyUi::GameByTitle(library, title);
		if (cells.size() != 8 ||
		    KytyUi::WidgetText(cells[6]) != kyty::gtk::StatusText(kyty_game_get_status(game))) {
			return false;
		}
	}
	return true;
}

bool WaitForCompatibility(KytyMainWindow* window, int timeout_ms) {
	auto& library = kyty::gtk::MainWindowLibrary(window);
	return KytyUi::WaitFor([&library]() { return library.Stats().compatibility_finished; },
	                       timeout_ms, "the compatibility loader to finish");
}

// Every request of the Compatibility_Database fails with status 503.
void ServeUnavailable(KytyUi::UiContext& ui) {
	KytyTests::LoopbackResponse response;
	response.status = 503;
	response.body   = "Service Unavailable";
	ui.Server().Script(core::kCompatibilityUrl, std::move(response));
}

double MillisecondsSince(gint64 start_us) {
	return static_cast<double>(g_get_monotonic_time() - start_us) / 1000.0;
}

} // namespace

KYTY_UI_CASE(compat_statuses_within_1s) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	// The database arrives after the first scan was applied.
	KytyTests::LoopbackResponse response;
	response.body     = kDatabase;
	response.delay_ms = 1000;
	ui.Server().Script(core::kCompatibilityUrl, std::move(response));
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto& library    = KytyUi::ContextOf(application).library;
		auto& controller = kyty::gtk::MainWindowLibrary(window);
		if (!KytyUi::WaitFor([&]() { return controller.Stats().scans_applied >= 1; }, 5000,
		                     "the first scan") ||
		    !Check(!controller.Stats().compatibility_finished,
		           "the games are listed before the database arrives") ||
		    !WaitForCompatibility(window, 10000)) {
			KytyUi::QuitApplication(application);
			return;
		}
		const gint64 loaded_us = controller.Stats().compatibility_finished_us;
		std::string  wrong;
		const bool   shown = KytyUi::WaitFor(
		    [&]() {
			    return StatusesShown(library, kExpected) && StatusTextsDrawn(window, library);
		    },
		    1000, "every status to be shown");
		const double elapsed = MillisecondsSince(loaded_us);
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		const double painted = MillisecondsSince(loaded_us);
		std::printf("compat_statuses_within_1s: statuses shown %.1f ms and painted %.1f ms after "
		            "the load\n",
		            elapsed, painted);
		Check(shown && painted <= 1000.0,
		      "every status is shown within 1 s of the load (" + std::to_string(painted) + " ms)");
		StatusesShown(library, kExpected, &wrong);
		Check(wrong.empty(), "the status of " + wrong + " matches the database");
		Check(controller.Stats().scans_started == 1 &&
		          controller.Stats().compatibility_source == "download",
		      "the database was applied without a rescan");
		Check(kyty_game_get_comment(KytyUi::GameByTitle(library, "Gamma Archive")) ==
		          "Stops at the logo",
		      "the comment comes with the status");
		// A game found later gets its status from the loaded database.
		if (KytyUi::WriteGameWithParam(games / "Late Game",
		                               KytyUi::ParamJson("Late Game", "ppsa01005", "01.000"))) {
			const auto applied = controller.Stats().scans_applied;
			gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
			Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F5), "F5 is handled");
			KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > applied; }, 8000,
			                "the rescan");
			KytyGame* late = KytyUi::GameByTitle(library, "Late Game");
			Check(late != nullptr && kyty_game_get_status(late) == Status::DoesntBoot,
			      "the game found by the rescan shows Doesn't boot");
			Check(ui.Server().Hits(core::kCompatibilityUrl) == 1,
			      "the database was downloaded once");
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	Check(KytyUi::ReadText(core::CompatibilityCachePath(ui.AppDirectory())) == kDatabase,
	      "the downloaded database replaced the cache in the Data_Directory");
}

KYTY_UI_CASE(compat_cache_fallback) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path    cache = core::CompatibilityCachePath(ui.AppDirectory());
	const std::string cached =
	    "{\"PPSA01001\": {\"status\": \"Logo\", \"comment\": \"From the cache\"}}";
	if (!KytyUi::WriteText(cache, cached)) {
		return;
	}
	ServeUnavailable(ui);
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto& library    = KytyUi::ContextOf(application).library;
		auto& controller = kyty::gtk::MainWindowLibrary(window);
		if (WaitForCompatibility(window, 20000)) {
			Check(controller.Stats().compatibility_source == "cache",
			      "the cached database is used after every download failed");
			Check(KytyUi::WaitFor(
			          [&]() {
				          return StatusesShown(library, {{"Alpha Game", Status::Logo}}) &&
				                 StatusTextsDrawn(window, library);
			          },
			          1000, "the cached statuses to be shown"),
			      "Alpha Game shows Logo from the cache, the other games Unknown");
			Check(kyty_game_get_comment(KytyUi::GameByTitle(library, "Alpha Game")) ==
			          "From the cache",
			      "with the cached comment");
			Check(ui.Server().Hits(core::kCompatibilityUrl) == 4,
			      "four download attempts were made (" +
			          std::to_string(ui.Server().Hits(core::kCompatibilityUrl)) + ")");
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	Check(KytyUi::ReadText(cache) == cached, "the cache file is unchanged");
}

KYTY_UI_CASE(compat_offline_warning) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	ServeUnavailable(ui);
	KytyUi::ExpectLog("kyty-launcher", G_LOG_LEVEL_WARNING, kOfflineWarning);
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto& library    = KytyUi::ContextOf(application).library;
		auto& controller = kyty::gtk::MainWindowLibrary(window);
		if (WaitForCompatibility(window, 20000)) {
			Check(controller.Stats().compatibility_source.empty(), "no database was applied");
			Check(StatusesShown(library, {}) && StatusTextsDrawn(window, library),
			      "every game shows Unknown");
			// The launcher keeps working: a rescan, a selection, the game actions.
			const auto applied = controller.Stats().scans_applied;
			gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
			Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F5), "F5 is handled");
			KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > applied; }, 8000,
			                "the rescan");
			KytyUi::SelectTitle(library, KytyUi::kAlphaTitle);
			Check(g_action_group_get_action_enabled(kyty::gtk::MainWindowGameActions(window),
			                                        "run") != FALSE,
			      "Run is available for the selected game");
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	std::error_code ec;
	Check(!fs::exists(core::CompatibilityCachePath(ui.AppDirectory()), ec),
	      "no cache file was written");
}

KYTY_UI_CASE(compat_retry_schedule) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	using std::chrono::milliseconds;
	const std::vector<milliseconds> schedule = {milliseconds(750), milliseconds(1500),
	                                            milliseconds(2250)};
	// 1. A network failure, two server errors, then the database.
	{
		KytyTests::FakeScheduler  scheduler;
		KytyTests::FakeHttpClient http(scheduler);
		KytyTests::TestServices   services(http, &scheduler);
		KytyUi::UseCaseEnvironment(ui, services);
		KytyTests::FakeResponse failure;
		failure.network_failure = true;
		http.AddResponse(core::kCompatibilityUrl, failure);
		KytyTests::FakeResponse error;
		error.status = 500;
		http.AddResponse(core::kCompatibilityUrl, error);
		error.status = 404;
		http.AddResponse(core::kCompatibilityUrl, error);
		KytyTests::FakeResponse database;
		database.body = kDatabase;
		http.AddResponse(core::kCompatibilityUrl, database);
		KytyUi::OwnMainWindow session(ui, services.Get());
		if (session.Ok()) {
			auto& controller = kyty::gtk::MainWindowLibrary(session.Window());
			Check(http.Requests().size() == 1, "the first request is sent at startup");
			// Each retry waits its delay on the virtual clock, not a millisecond less.
			std::size_t sent = 1;
			for (const milliseconds delay: schedule) {
				scheduler.AdvanceBy(delay - milliseconds(1));
				Check(http.Requests().size() == sent,
				      "no retry before " + std::to_string(delay.count()) + " ms");
				scheduler.AdvanceBy(milliseconds(1));
				sent++;
				Check(http.Requests().size() == sent,
				      "a retry after " + std::to_string(delay.count()) + " ms");
			}
			scheduler.RunUntilIdle();
			Check(scheduler.RequestedDelays() == schedule,
			      "the loader asked for exactly 750, 1500, and 2250 ms");
			for (const auto& request: http.Requests()) {
				Check(request.url == core::kCompatibilityUrl &&
				          request.total_timeout == core::kCompatibilityTimeout &&
				          request.max_bytes == core::kMaxCompatibilityBytes,
				      "each attempt asks for the database with the 15 s timeout and the size "
				      "limit");
			}
			KytyUi::WaitForLibrary(session.Window());
			Check(controller.Stats().compatibility_source == "download" &&
			          StatusesShown(session.Context().library, kExpected),
			      "the statuses of the fourth attempt's database are shown");
		}
	}
	// 2. Four failures: no fifth attempt, Unknown everywhere, the 8.4 warning once.
	{
		KytyTests::FakeScheduler  scheduler;
		KytyTests::FakeHttpClient http(scheduler);
		KytyTests::TestServices   services(http, &scheduler);
		KytyUi::UseCaseEnvironment(ui, services);
		std::error_code ec;
		fs::remove(core::CompatibilityCachePath(ui.AppDirectory()), ec);
		KytyUi::ExpectLog("kyty-launcher", G_LOG_LEVEL_WARNING, kOfflineWarning);
		KytyUi::OwnMainWindow session(ui, services.Get());
		if (session.Ok()) {
			auto& controller = kyty::gtk::MainWindowLibrary(session.Window());
			scheduler.AdvanceBy(milliseconds(60000));
			Check(http.Requests().size() == 4,
			      "four attempts and no more (" + std::to_string(http.Requests().size()) + ")");
			Check(scheduler.RequestedDelays() == schedule, "with the same waits");
			KytyUi::WaitForLibrary(session.Window());
			Check(controller.Stats().compatibility_finished &&
			          controller.Stats().compatibility_source.empty() &&
			          StatusesShown(session.Context().library, {}),
			      "every game is Unknown");
		}
	}
}
