// UI tests of the title sound (task 22.4; Requirements 8.7-8.10; LFI-09; phase 7 plan D15), each
// on Wayland and on X11. A main window over a context of the case's own (KytyUi::OwnMainWindow:
// the application's finalize quits SDL while the services' TitleSoundPlayer still holds its
// stream) selects games whose sce_sys/snd0.at9 is a synthetic ATRAC9 file; SDL plays it on its
// dummy audio driver (with-display.sh sets SDL_AUDIO_DRIVER=dummy), and TitleSoundPlayer's
// PlayingEntry() says which sound is on the device.
// - title_sound_start_within_2s: a directory game's and a .zar game's sound start within 2 s of
//   the selection while no game runs (8.7).
// - title_sound_stop_within_100ms: a selection change (to a game without a sound, to another
//   sound, to no selection) and a run start stop the sound within 100 ms, a sound still decoding
//   for the previous selection is discarded, and nothing plays while the game runs (8.8, 8.10).
// - title_sound_failures: a game without a sound, with one over 32 MiB, with one that does not
//   decode, and with a valid one while there is no audio output (an audio driver SDL does not
//   have) play nothing, stay selected with the game actions available, and log the 8.9 warnings
//   (declared with ExpectLog) for the last three.

#include "launcher/core/entryId.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/services/titleSoundPlayer.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <string>
#include <system_error>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using KytyUi::Check;

constexpr int    kSuperframes = 400; // about 8.5 s
constexpr double kStartLimit  = 2000.0;
constexpr double kStopLimit   = 100.0;

double MillisecondsSince(gint64 start_us) {
	return static_cast<double>(g_get_monotonic_time() - start_us) / 1000.0;
}

bool WriteSound(const fs::path& game) {
	return KytyUi::WriteText(game / "sce_sys" / "snd0.at9", KytyUi::At9TitleSound(kSuperframes));
}

// The fixture games, Alpha Game and Beta Quest with a title sound, and "Delta Sound.zar" (a .zar
// game with one); returns the games' folder (empty after a Fail).
fs::path PrepareSoundLibrary(KytyUi::UiContext& ui) {
	const auto games = KytyUi::PrepareLibrary(ui);
	if (games.empty() || !WriteSound(games / "Alpha Game") ||
	    !WriteSound(games / "Beta" / "Beta Quest")) {
		return {};
	}
	const fs::path staging = games / "Delta Sound";
	if (!KytyUi::WriteGameWithParam(staging,
	                                KytyUi::ParamJson("Delta Sound", "PPSA05001", "01.000")) ||
	    !WriteSound(staging) || KytyUi::PackGame(staging).empty()) {
		return {};
	}
	return games;
}

// The path of the game titled title.
fs::path PathOf(kyty::gtk::GameListModel& library, std::string_view title) {
	KytyGame* game = KytyUi::GameByTitle(library, title);
	return game != nullptr ? kyty_game_get_entry(game).path : fs::path();
}

// Selects title and waits until its sound plays; the time from the selection, or -1 after a Fail.
double PlayTime(kyty::gtk::TitleSoundPlayer& sound, kyty::gtk::GameListModel& library,
                std::string_view title) {
	const fs::path path  = PathOf(library, title);
	const gint64   start = g_get_monotonic_time();
	if (!KytyUi::SelectTitle(library, title) ||
	    !KytyUi::WaitFor([&]() { return sound.PlayingEntry() == core::EntryId(path); }, 5000,
	                     "the title sound of " + std::string(title) + " to play")) {
		return -1;
	}
	return MillisecondsSince(start);
}

// Waits until no sound plays; the time since start_us, or -1 after a Fail.
double StopTime(kyty::gtk::TitleSoundPlayer& sound, gint64 start_us, const std::string& what) {
	if (!KytyUi::WaitFor([&]() { return !sound.Playing(); }, 2000, "the sound to stop " + what)) {
		return -1;
	}
	return MillisecondsSince(start_us);
}

} // namespace

KYTY_UI_CASE(title_sound_start_within_2s) {
	KytyUi::UiContext ui;
	if (PrepareSoundLibrary(ui).empty()) {
		return;
	}
	auto&                 sound = ui.Services().title_sound;
	KytyUi::OwnMainWindow session(ui, ui.Services().Get());
	if (!session.Ok() || !KytyUi::WaitForLibrary(session.Window())) {
		return;
	}
	auto& library = session.Context().library;
	for (const char* title: {"Alpha Game", "Delta Sound", "Beta Quest", "Alpha Game"}) {
		const double ms = PlayTime(sound, library, title);
		std::printf("title_sound_start_within_2s: %s: the sound of %s started %.1f ms after the "
		            "selection\n",
		            g_getenv("GDK_BACKEND"), title, ms);
		Check(ms >= 0 && ms <= kStartLimit,
		      std::string(title) + "'s sound starts within 2 s (" + std::to_string(ms) + " ms)");
	}
	KytyGame* delta = KytyUi::GameByTitle(library, "Delta Sound");
	Check(delta != nullptr && kyty_game_get_entry(delta).kind == core::GameEntry::Kind::Archive,
	      "Delta Sound is a .zar game");
	gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(library.Selection()));
	KytyUi::WaitFor([&]() { return !sound.Playing(); }, 1000, "the sound to stop");
}

KYTY_UI_CASE(title_sound_stop_within_100ms) {
	KytyUi::UiContext ui;
	if (PrepareSoundLibrary(ui).empty()) {
		return;
	}
	auto&                 sound = ui.Services().title_sound;
	KytyUi::OwnMainWindow session(ui, ui.Services().Get());
	if (!session.Ok() || !KytyUi::WaitForLibrary(session.Window())) {
		return;
	}
	auto&           library = session.Context().library;
	KytyMainWindow* window  = session.Window();
	const auto      stop    = [&](const std::string& what, const std::function<void()>& change) {
		const gint64 start = g_get_monotonic_time();
		change();
		const double ms = StopTime(sound, start, what);
		std::printf("title_sound_stop_within_100ms: the sound stopped %.1f ms after %s\n", ms,
		            what.c_str());
		Check(ms >= 0 && ms <= kStopLimit,
		      "the sound stops within 100 ms " + what + " (" + std::to_string(ms) + " ms)");
	};
	// To a game without a sound.
	PlayTime(sound, library, "Alpha Game");
	stop("on the selection of a game without a sound",
	     [&]() { KytyUi::SelectTitle(library, "Gamma Archive"); });
	// To another game with a sound: the previous one stops at once, the new one follows.
	PlayTime(sound, library, "Alpha Game");
	stop("on the selection of another game with a sound",
	     [&]() { KytyUi::SelectTitle(library, "Beta Quest"); });
	KytyUi::WaitFor(
	    [&]() { return sound.PlayingEntry() == core::EntryId(PathOf(library, "Beta Quest")); },
	    5000, "Beta Quest's sound to play next");
	// To no selection.
	stop("on clearing the selection",
	     [&]() { gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(library.Selection())); });
	// A sound still decoding for the previous selection is discarded: Delta Sound, then at once
	// Gamma Archive (no sound): nothing plays afterwards.
	KytyUi::SelectTitle(library, "Delta Sound");
	KytyUi::SelectTitle(library, "Gamma Archive");
	KytyUi::Pump(1500);
	Check(!sound.Playing(), "the sound that was loading for Delta Sound is discarded");
	// A run starts (8.10).
	{
		const KytyUi::FakeEmulatorBehavior emulator("--stdout ready --wait");
		PlayTime(sound, library, "Alpha Game");
		stop("when the game starts", [&]() {
			g_action_group_activate_action(G_ACTION_GROUP(kyty::gtk::MainWindowGameActions(window)),
			                               "run", nullptr);
		});
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "the game to run");
		KytyUi::SelectTitle(library, "Beta Quest");
		KytyUi::Pump(1000);
		Check(!sound.Playing(), "no title sound plays while the game runs");
		g_action_group_activate_action(G_ACTION_GROUP(kyty::gtk::MainWindowGameActions(window)),
		                               "stop", nullptr);
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
	}
}

KYTY_UI_CASE(title_sound_failures) {
	// No audio output device: SDL refuses an audio driver it does not have ("Audio target ... not
	// available") instead of falling back to another one, and TitleSoundPlayer initializes SDL's
	// audio only at its first Play. Set before anything in this process uses SDL (SDL keeps its
	// own copy of the environment from its first use).
	g_setenv("SDL_AUDIO_DRIVER", "kyty-ui-no-such-driver", TRUE);
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	// Beta Quest: over 32 MiB (sparse); Gamma's place is taken by "Bad Sound": not ATRAC9;
	// "Quiet Output" has a valid sound but there is no audio output.
	const fs::path  large = games / "Beta" / "Beta Quest" / "sce_sys" / "snd0.at9";
	std::error_code ec;
	if (!KytyUi::WriteText(large, "RIFF")) {
		return;
	}
	fs::resize_file(large, (uint64_t {32} << 20u) + 1, ec);
	if (!Check(!ec, "make the 32 MiB + 1 title sound") ||
	    !KytyUi::WriteGameWithParam(games / "Bad Sound",
	                                KytyUi::ParamJson("Bad Sound", "PPSA05002", "01.000")) ||
	    !KytyUi::WriteText(games / "Bad Sound" / "sce_sys" / "snd0.at9",
	                       "this is not a RIFF ATRAC9 file") ||
	    !KytyUi::WriteGameWithParam(games / "Quiet Output",
	                                KytyUi::ParamJson("Quiet Output", "PPSA05003", "01.000")) ||
	    !WriteSound(games / "Quiet Output")) {
		return;
	}
	auto& sound = ui.Services().title_sound;
	KytyUi::ExpectLog("kyty-launcher", G_LOG_LEVEL_WARNING,
	                  "No title sound for " + (games / "Beta" / "Beta Quest").string() +
	                      ": the title sound file is larger than 32 MiB");
	KytyUi::ExpectLog("kyty-launcher", G_LOG_LEVEL_WARNING,
	                  "No title sound for " + (games / "Bad Sound").string() +
	                      ": the title sound file is not a valid RIFF ATRAC9 file");
	KytyUi::ExpectLog("kyty-launcher", G_LOG_LEVEL_WARNING,
	                  "No title sound for " + (games / "Quiet Output").string() +
	                      ": no audio output: ");
	KytyUi::OwnMainWindow session(ui, ui.Services().Get());
	if (!session.Ok() || !KytyUi::WaitForLibrary(session.Window())) {
		return;
	}
	auto&         library = session.Context().library;
	GActionGroup* actions = kyty::gtk::MainWindowGameActions(session.Window());
	for (const char* title: {"Alpha Game", "Beta Quest", "Bad Sound", "Quiet Output"}) {
		KytyUi::SelectTitle(library, title);
		KytyUi::Pump(1500);
		Check(!sound.Playing(), std::string(title) + ": no sound plays");
		Check(library.Selected() == KytyUi::GameByTitle(library, title),
		      std::string(title) + " stays selected");
		Check(g_action_group_get_action_enabled(actions, "run") != FALSE &&
		          g_action_group_get_action_enabled(actions, "open-folder") != FALSE,
		      std::string(title) + ": the game actions stay available");
	}
}
