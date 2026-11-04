// UI tests of running games (task 22.5; Requirements 10.1, 10.4-10.9, 10.11-10.13, 11.18, 12.7;
// design.md LFI-11, LFI-40; phase 7 plan D27), each on Wayland and on X11. Every case runs the real
// KytyApplication (UiContext::RunApplication) over the fixture games with the fake Emulator
// (tests/ui/fake-kyty-emulator.sh, the fake_emulator helper behind it, told what to do through
// KYTY_UI_FAKE_ARGS), or with an Emulator file the case writes itself.
//
// - run_arguments (10.1, 12.7): the arguments are the core's for the configuration at the moment
//   of activation, and the working directory is the Data_Directory.
// - run_log_lines (10.5): both streams, in order, with U+FFFD for invalid UTF-8, in the log within
//   1 s, in batches; run_log_after_exit (10.7): the unterminated last line and the end of the run
//   within 1 s, also while a grandchild keeps the pipes open (the 1 s EOF wait).
// - run_stop (10.6), run_kill (10.11), run_abnormal_exit (10.12), run_save_log (10.8, 10.13).
// - run_emulator_missing, run_emulator_not_executable, run_spawn_failure (10.9): the error names
//   the Emulator's absolute path and the reason, Run stays on, nothing runs.
// - run_quit_question_signal (SIGINT and SIGTERM while "Stop the running game and quit?" is open)
//   and run_quit_question_game_exits (the game ends by itself while it is open, D27).
// - run_log_hidden_then_shown and run_log_scrolled_up_hidden (D27: a hidden pane that followed the
//   end shows the newest line when it is shown; one the user scrolled up comes back where it was),
//   run_log_tooltip (D27), run_log_placeholder (D27), and run_running_indicator (10.4, 11.18,
//   LFI-11).

#include "launcher/core/atomicFile.h"
#include "launcher/core/emulatorArguments.h"
#include "launcher/core/emulatorProcess.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/gameCell.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/logView.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/runController.h"
#include "launcher/gtk/settingsWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"
#include "ui/support/uiWindows.h"

#include <adwaita.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using KytyUi::Check;
using KytyUi::kAlphaTitle;
using KytyUi::kBetaTitle;

double Milliseconds(int64_t from_us, int64_t to_us) {
	return static_cast<double>(to_us - from_us) / 1000.0;
}

// The arguments the fake Emulator was started with (NUL-separated in <tree>/emulator-args).
std::vector<std::string> RecordedArguments(KytyUi::UiContext& ui) {
	const std::string        bytes = KytyUi::ReadText(ui.Root() / "emulator-args");
	std::vector<std::string> args;
	std::size_t              start = 0;
	for (std::size_t end = bytes.find('\0'); end != std::string::npos;
	     end             = bytes.find('\0', start)) {
		args.push_back(bytes.substr(start, end - start));
		start = end + 1;
	}
	return args;
}

// The value after option in args ("" when it is not there).
std::string Option(const std::vector<std::string>& args, std::string_view option) {
	const auto found = std::find(args.begin(), args.end(), option);
	return found != args.end() && found + 1 != args.end() ? *(found + 1) : "";
}

kyty::gtk::RunController& Run(KytyMainWindow* window) {
	return kyty::gtk::MainWindowRun(window);
}

// Selects the game titled title and activates game.run (as Enter, Ctrl+R, or the Run button do).
bool StartGame(KytyApplication* application, KytyMainWindow* window, std::string_view title) {
	return KytyUi::SelectTitle(KytyUi::ContextOf(application).library, title) &&
	       KytyUi::ActivateGameAction(window, "run");
}

bool WaitRunning(KytyMainWindow* window, bool running, int timeout_ms = 8000) {
	return KytyUi::WaitFor(
	    [window, running]() { return kyty::gtk::MainWindowGameRunning(window) == running; },
	    timeout_ms, running ? "the game to run" : "the game to end");
}

// Waits for the line in the log model; the time it was there first (g_get_monotonic_time()), or 0
// after a Fail.
int64_t WaitForLine(KytyMainWindow* window, std::string_view line, int timeout_ms = 5000) {
	int64_t seen = 0;
	KytyUi::WaitFor(
	    [&]() {
		    if (KytyUi::HasLine(KytyUi::LogLines(window), line)) {
			    seen = g_get_monotonic_time();
			    return true;
		    }
		    return false;
	    },
	    timeout_ms, "the line \"" + std::string(line) + "\" in the log");
	return seen;
}

std::size_t IndexOf(const std::vector<std::string>& lines, std::string_view line) {
	return static_cast<std::size_t>(std::find(lines.begin(), lines.end(), line) - lines.begin());
}

// The game's process got exactly these signals (the fake's --record-signals file, one number per
// line).
std::string Signals(const fs::path& file) {
	return KytyUi::ReadText(file);
}

// An Emulator of the case's own at <app>/kyty_emulator: script runs for a game; without arguments
// (the version probe) it prints a version banner.
bool WriteEmulatorScript(KytyUi::UiContext& ui, std::string_view script) {
	const fs::path  emulator = ui.AppDirectory() / "kyty_emulator";
	std::error_code ec;
	fs::remove(emulator, ec);
	const std::string text = "#!/bin/sh\nif [ \"$#\" -eq 0 ]; then\n\tprintf 'exe_name: "
	                         "kyty_emulator\\n'\n\texit 0\nfi\n" +
	                         std::string(script);
	if (!KytyUi::WriteText(emulator, text)) {
		return false;
	}
	fs::permissions(emulator,
	                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
	                    fs::perms::others_read | fs::perms::others_exec,
	                fs::perm_options::replace, ec);
	return Check(!ec, "make " + emulator.string() + " executable");
}

// The error alert "The game cannot start": its text, after it was closed ("" after a Fail).
std::string CannotStartText() {
	GtkWindow* alert = KytyUi::FindAlert("The game cannot start");
	if (alert == nullptr) {
		return "";
	}
	const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
	KytyUi::CloseError(alert);
	return text;
}

// After a refused run: nothing runs, Run stays on, no game shows the running indicator.
void CheckNothingRuns(KytyApplication* application, KytyMainWindow* window,
                      const std::string& when) {
	auto& library = KytyUi::ContextOf(application).library;
	Check(!kyty::gtk::MainWindowGameRunning(window), when + ": no game runs");
	Check(g_action_group_get_action_enabled(kyty::gtk::MainWindowGameActions(window), "run") !=
	          FALSE,
	      when + ": Run stays on");
	GListModel* games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game = kyty::gtk::GObjectPtr<KytyGame>::Adopt(
		    static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		Check(!kyty_game_get_running(game.get()),
		      when + ": " + kyty_game_get_entry(game.get()).title_name + " is not marked running");
	}
}

// The mapped title cell (list) or tile (grid) of title below root that shows the running state:
// its running emblem is visible.
bool ShowsRunning(GtkWidget* root, std::string_view title) {
	for (GtkWidget* cell: KytyUi::MappedCells(root, title)) {
		GtkWidget* emblem = kyty_game_cell_get_running_emblem(KYTY_GAME_CELL(cell));
		if (emblem != nullptr && gtk_widget_get_visible(emblem) != FALSE) {
			return true;
		}
	}
	return false;
}

bool TitleBold(GtkWidget* root, std::string_view title) {
	for (GtkWidget* cell: KytyUi::MappedCells(root, title)) {
		if (gtk_widget_has_css_class(cell, "game-running") != FALSE) {
			return true;
		}
	}
	return false;
}

// The log list has more rows than fit (its vertical adjustment has a range, so it was laid out with
// them) and is scrolled to the bottom.
bool LogAtEnd(KytyMainWindow* window) {
	GtkWidget* scrolled =
	    gtk_widget_get_ancestor(GTK_WIDGET(KytyUi::LogList(window)), GTK_TYPE_SCROLLED_WINDOW);
	if (scrolled == nullptr) {
		return false;
	}
	GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
	const double   bottom =
	    gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
	return bottom > 2 && gtk_adjustment_get_value(adjustment) >= bottom - 2;
}

// Stops a game that still runs when the case's drive function returns, so that the application's
// quit does not ask (a failed case then ends at once instead of at the 10 s watchdog).
class StopAtEnd {
public:
	explicit StopAtEnd(KytyMainWindow* window): m_window(window) {}
	~StopAtEnd() {
		if (kyty::gtk::MainWindowGameRunning(m_window)) {
			kyty::gtk::MainWindowRun(m_window).Stop();
			KytyUi::WaitFor([this]() { return !kyty::gtk::MainWindowGameRunning(m_window); }, 8000,
			                "the game to stop at the end of the case");
		}
	}
	StopAtEnd(const StopAtEnd&)            = delete;
	StopAtEnd& operator=(const StopAtEnd&) = delete;

private:
	KytyMainWindow* m_window;
};

std::vector<std::string> RowTexts(KytyMainWindow* window) {
	std::vector<std::string> texts;
	for (GtkWidget* row: KytyUi::LogRows(window)) {
		texts.push_back(KytyUi::WidgetText(row));
	}
	return texts;
}

} // namespace

// ---- 10.1, 12.7: the arguments and the working directory

KYTY_UI_CASE(run_arguments) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const KytyUi::FakeEmulatorBehavior fake("--stdout first-run --print-cwd --exit 0");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&      context      = KytyUi::ContextOf(application);
		KytyGame*  alpha        = KytyUi::GameByTitle(context.library, kAlphaTitle);
		const auto expected_for = [&]() {
			const auto& entry = kyty_game_get_entry(alpha);
			return core::BuildEmulatorArguments(
			    entry, core::ResolveEffectiveConfiguration(
			               context.settings.loaded.settings, entry,
			               core::FindCheatPlan(ui.Services().environment, context.paths.data_dir,
			                                   entry.title_id)));
		};
		if (!Check(alpha != nullptr, "Alpha Game is listed") ||
		    !StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the first run to end");
		const auto first = RecordedArguments(ui);
		Check(!first.empty() && first == expected_for() &&
		          Run(window).Stats().last_arguments == first,
		      "the Emulator gets the core's arguments for the Global_Configuration (" +
		          std::to_string(first.size()) + " arguments)");
		Check(Option(first, "--game") == kyty_game_get_entry(alpha).path.string() &&
		          Option(first, "--user-name") == "Kyty",
		      "with the game's path and the global user name");
		Check(KytyUi::HasLine(KytyUi::LogLines(window), ui.AppDirectory().string()) &&
		          Run(window).Stats().last_working_directory == ui.AppDirectory(),
		      "it runs in the Data_Directory " + ui.AppDirectory().string() + " (12.7)");

		// Change the game's configuration in its editor; the next run uses it (10.1) and starts
		// with a cleared log.
		if (!KytyUi::ActivateGameAction(window, "edit-config")) {
			return;
		}
		GtkWindow* editor = KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "Alpha's editor");
		if (editor == nullptr) {
			return;
		}
		gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(GTK_WIDGET(editor), "User name")),
		                      "Second Run");
		adw_switch_row_set_active(
		    ADW_SWITCH_ROW(KytyUi::RowTitled(GTK_WIDGET(editor), "Run in fullscreen")), TRUE);
		KytyUi::Activate(KytyUi::FindByBuildableId(GTK_WIDGET(editor), "save_button"));
		KytyUi::WaitClosed(editor, "Save to close the editor");
		g_setenv("KYTY_UI_FAKE_ARGS", "--stdout second-run --exit 0", TRUE);
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 2; }, 8000,
		                "the second run to end");
		const auto second = RecordedArguments(ui);
		Check(second == expected_for() && Option(second, "--user-name") == "Second Run" &&
		          std::find(second.begin(), second.end(), "--fullscreen") != second.end() &&
		          std::find(first.begin(), first.end(), "--fullscreen") == first.end(),
		      "the second run gets the game's new configuration");
		const auto lines = KytyUi::LogLines(window);
		Check(KytyUi::HasLine(lines, "second-run") && !KytyUi::HasLine(lines, "first-run"),
		      "the second run starts with a cleared log");
	});
}

// ---- 10.5: the output in the log

KYTY_UI_CASE(run_log_lines) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	// A long line, one word (the fake's arguments are split at spaces).
	std::string long_line = "long-line:";
	while (long_line.size() < 300) {
		long_line += "-the-whole-line-is-in-the-log-and-in-its-tooltip";
	}
	const std::string args = "--stdout out-1 --stderr err-1 --stdout out-2 --stdout-hex 41ff420a "
	                         "--stderr err-2 --stdout " +
	                         long_line + " --sleep-ms 300 --stdout out-late --wait";
	const KytyUi::FakeEmulatorBehavior fake(args.c_str());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!KytyUi::ShowLogPane(window, true)) {
			return;
		}
		// Count the model's insertions: lines that arrive together come in one batch.
		std::vector<guint>     added;
		kyty::gtk::SignalScope batches = kyty::gtk::SignalScope::Connect(
		    kyty::gtk::MainWindowLog(window), "items-changed",
		    G_CALLBACK(+[](GListModel*, guint, guint, guint count, gpointer data) {
			    if (count > 0) {
				    static_cast<std::vector<guint>*>(data)->push_back(count);
			    }
		    }),
		    &added);
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		const int64_t started  = Run(window).Stats().started_us;
		const int64_t first    = WaitForLine(window, "out-1");
		const int64_t late     = WaitForLine(window, "out-late");
		const double  first_ms = Milliseconds(started, first);
		const double  late_ms  = Milliseconds(started, late);
		g_print("run_log_lines: first line after %.1f ms, the line written 300 ms later after "
		        "%.1f ms; batches:",
		        first_ms, late_ms);
		for (const guint count: added) {
			g_print(" %u", count);
		}
		g_print("\n");
		Check(first > 0 && first_ms <= 1000,
		      "the first line is in the log within 1 s of the start (" + std::to_string(first_ms) +
		          " ms)");
		Check(late > 0 && late_ms >= 300 && late_ms <= 1300,
		      "a line written 300 ms after the others is there within 1 s of its write (" +
		          std::to_string(late_ms) + " ms after the start)");
		Check(!added.empty() && added.front() == 6,
		      "the six lines written at once arrive in one batch (the 100 ms sync)");
		const auto        lines = KytyUi::LogLines(window);
		const auto        at    = [&lines](std::string_view line) { return IndexOf(lines, line); };
		const std::string replaced = "A\xEF\xBF\xBD"
		                             "B";
		Check(at("out-1") < at("out-2") && at("out-2") < at(replaced) &&
		          at(replaced) < at(long_line) && at(long_line) < at("out-late") &&
		          at("out-late") < lines.size(),
		      "standard output keeps its order, with U+FFFD for the invalid byte");
		Check(at("err-1") < at("err-2") && at("err-2") < lines.size(),
		      "standard error keeps its order");
		KytyUi::WaitFor(
		    [&]() {
			    const auto rows = RowTexts(window);
			    return std::find(rows.begin(), rows.end(), "out-late") != rows.end();
		    },
		    2000, "the log list to show the last line");
		Check(KytyUi::Capture(GTK_WINDOW(window), "log"), "the main window with the log renders");
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
	});
}

// ---- 10.7: after the exit

KYTY_UI_CASE(run_log_after_exit) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path pid_file = ui.Root() / "grandchild.pid";
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
		auto&         context = KytyUi::ContextOf(application);
		// The last line has no line break: it is in the log when the run has ended.
		g_setenv("KYTY_UI_FAKE_ARGS",
		         "--stdout complete-line --stdout-hex 6c6173742d6c696e65 --exit 0", TRUE);
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the game to end");
		const auto&  stats = Run(window).Stats();
		const double ended = Milliseconds(stats.started_us, stats.exited_us);
		g_print("run_log_after_exit: run to end of the exit flow %.1f ms\n", ended);
		Check(KytyUi::HasLine(KytyUi::LogLines(window), "last-line"),
		      "the unterminated last line is in the log");
		Check(ended <= 1000, "the exit flow ends within 1 s of the start (and so of the exit): " +
		                         std::to_string(ended) + " ms");
		KytyGame* alpha = KytyUi::GameByTitle(context.library, kAlphaTitle);
		Check(!kyty_game_get_running(alpha) &&
		          g_action_group_get_action_enabled(actions, "run") != FALSE &&
		          g_action_group_get_action_enabled(actions, "stop") == FALSE,
		      "the running indicator is gone, Run is on, Stop is off");
		KytyUi::Pump(1500);
		Check(KytyUi::HasLine(KytyUi::LogLines(window), "complete-line") &&
		          KytyUi::HasLine(KytyUi::LogLines(window), "last-line"),
		      "the log keeps the lines after the exit");

		// A grandchild keeps both pipes open: the exit flow still ends after the 1 s EOF wait.
		const std::string script = "printf 'grandchild-start\\n'\nsleep 20 &\necho $! > '" +
		                           pid_file.string() + "'\nprintf 'partial-at-exit'\nexit 0\n";
		if (!WriteEmulatorScript(ui, script) || !StartGame(application, window, kAlphaTitle)) {
			return;
		}
		Check(!KytyUi::HasLine(KytyUi::LogLines(window), "complete-line"),
		      "the next run clears the log");
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 2; }, 8000,
		                "the run with the grandchild to end");
		const double waited = Milliseconds(stats.started_us, stats.exited_us);
		g_print("run_log_after_exit: with a grandchild holding the pipes, run to end of the exit "
		        "flow %.1f ms\n",
		        waited);
		Check(waited >= 900 && waited <= 1500,
		      "the exit flow waits at most about 1 s for the pipes (" + std::to_string(waited) +
		          " ms)");
		Check(KytyUi::HasLine(KytyUi::LogLines(window), "grandchild-start") &&
		          KytyUi::HasLine(KytyUi::LogLines(window), "partial-at-exit"),
		      "the lines before the exit and the unterminated one are in the log");
		const std::string pid_text = KytyUi::ReadText(pid_file);
		const pid_t       pid      = static_cast<pid_t>(std::atol(pid_text.c_str()));
		if (Check(pid > 1, "the grandchild's pid was written")) {
			::kill(pid, SIGTERM);
		}
	});
	g_unsetenv("KYTY_UI_FAKE_ARGS");
}

// ---- 10.6: Stop sends SIGTERM

KYTY_UI_CASE(run_stop) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path    signals = ui.Root() / "signals";
	const std::string args    = "--record-signals " + signals.string() + " --stdout ready --wait";
	const KytyUi::FakeEmulatorBehavior fake(args.c_str());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!StartGame(application, window, kAlphaTitle) || !WaitRunning(window, true) ||
		    WaitForLine(window, "ready") == 0) {
			return;
		}
		Check(g_action_group_get_action_enabled(kyty::gtk::MainWindowGameActions(window), "stop") !=
		          FALSE,
		      "Stop is on while the game runs");
		gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_period, GDK_CONTROL_MASK),
		      "Ctrl+Period is handled");
		WaitRunning(window, false);
		const auto&  stats   = Run(window).Stats();
		const double stopped = Milliseconds(stats.stop_requested_us, stats.exited_us);
		g_print("run_stop: stop to exit %.1f ms\n", stopped);
		Check(Signals(signals) == "15\n", "the game got SIGTERM, once (" + Signals(signals) + ")");
		Check(WIFSIGNALED(stats.last_wait_status) && WTERMSIG(stats.last_wait_status) == SIGTERM,
		      "the game ended by SIGTERM");
		Check(stopped < 5000,
		      "it ended before the SIGKILL deadline (" + std::to_string(stopped) + " ms)");
		Check(stats.last_error.empty() && KytyUi::ToplevelCount() == 1,
		      "a stop the user asked for shows no error");
	});
}

// ---- 10.11: SIGKILL 5 s after SIGTERM

KYTY_UI_CASE(run_kill) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path    signals = ui.Root() / "signals";
	const std::string args    = "--record-signals " + signals.string() +
	                            " --ignore-term --stdout ready --wait --sleep-ms 60000";
	const KytyUi::FakeEmulatorBehavior fake(args.c_str());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!StartGame(application, window, kAlphaTitle) || !WaitRunning(window, true) ||
		    WaitForLine(window, "ready") == 0) {
			return;
		}
		KytyUi::ActivateGameAction(window, "stop");
		KytyUi::WaitFor([&]() { return Signals(signals) == "15\n"; }, 2000,
		                "the game to get SIGTERM");
		KytyUi::Pump(1000);
		Check(kyty::gtk::MainWindowGameRunning(window), "a game that ignores SIGTERM still runs");
		WaitRunning(window, false, 10000);
		const auto&  stats  = Run(window).Stats();
		const double killed = Milliseconds(stats.stop_requested_us, stats.exited_us);
		g_print("run_kill: stop to exit %.1f ms\n", killed);
		Check(WIFSIGNALED(stats.last_wait_status) && WTERMSIG(stats.last_wait_status) == SIGKILL,
		      "the game ended by SIGKILL");
		Check(killed >= 4900 && killed <= 6500,
		      "SIGKILL came 5 s after SIGTERM (" + std::to_string(killed) + " ms)");
		Check(Signals(signals) == "15\n", "SIGTERM was sent once");
		Check(stats.last_error.empty(), "a stop the user asked for shows no error");
	});
}

// ---- 10.12: an exit the user did not ask for

KYTY_UI_CASE(run_abnormal_exit) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		KytyUi::ShowLogPane(window, true);
		int        runs  = 0;
		const auto ended = [&](const char* args, const std::string& expected,
		                       const std::function<void()>& after_start, bool capture) {
			g_setenv("KYTY_UI_FAKE_ARGS", args, TRUE);
			const std::string last_error = Run(window).Stats().last_error;
			if (!StartGame(application, window, kAlphaTitle)) {
				return;
			}
			runs++;
			if (after_start) {
				after_start();
			}
			KytyUi::WaitFor([&]() { return Run(window).Stats().exits == runs; }, 8000,
			                std::string("the game to end (") + args + ")");
			if (expected.empty()) {
				KytyUi::Pump(300);
				Check(Run(window).Stats().last_error == last_error && KytyUi::ToplevelCount() == 1,
				      std::string("a clean exit shows no error (") + args + ")");
				for (const auto& line: KytyUi::LogLines(window)) {
					Check(line.find("The emulator") == std::string::npos,
					      "a clean exit adds no line (" + line + ")");
				}
				return;
			}
			GtkWindow* alert = KytyUi::FindAlert("The game ended unexpectedly");
			if (alert != nullptr) {
				Check(KytyUi::AllText(GTK_WIDGET(alert)).find(expected) != std::string::npos,
				      "the alert says \"" + expected + "\"");
				if (capture) {
					Check(KytyUi::Capture(alert, "alert"), "the alert renders");
					Check(KytyUi::Capture(GTK_WINDOW(window), "log"),
					      "the main window with the log line renders");
				}
				KytyUi::CloseError(alert);
			}
			Check(KytyUi::HasLine(KytyUi::LogLines(window), expected),
			      "the log has the line \"" + expected + "\"");
		};
		ended("--stdout about-to-fail --exit 3", "The emulator exited with exit code 3.", {}, true);
		ended("--stdout about-to-crash --raise 10", "The emulator was ended by signal SIGUSR1.", {},
		      false);
		// SIGTERM from someone else than the user: an unexpected end too.
		ended(
		    "--stdout ready --wait", "The emulator was ended by signal SIGTERM.",
		    [window]() {
			    if (WaitForLine(window, "ready") != 0) {
				    ::kill(Run(window).Stats().pid, SIGTERM);
			    }
		    },
		    false);
		ended("--stdout fine --exit 0", "", {}, false);
	});
	g_unsetenv("KYTY_UI_FAKE_ARGS");
}

// ---- 10.8, 10.13: Save log

KYTY_UI_CASE(run_save_log) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path saved = ui.Root() / "saved.log";
	KytyUi::WriteText(saved, "previous content that is replaced\n");
	const KytyUi::FakeEmulatorBehavior fake(
	    "--stdout first-line --stdout-hex 41ff420a --stderr second-line --exit 0");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto& chooser = ui.Services().file_chooser;
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the game to end");
		const auto  lines = KytyUi::LogLines(window);
		std::string expected;
		for (const auto& line: lines) {
			expected += line + "\n";
		}
		Check(lines.size() == 3, "the log has three lines (" + std::to_string(lines.size()) + ")");
		// Ctrl+S, the scripted chooser, the file replaced with the log's lines as UTF-8.
		chooser.file_answers.push_back(saved);
		gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_s, GDK_CONTROL_MASK),
		      "Ctrl+S is handled");
		KytyUi::WaitFor([&]() { return KytyUi::ReadText(saved) == expected; }, 3000,
		                "the log to be written to " + saved.string());
		Check(!chooser.calls.empty() && chooser.calls.back() == "SaveFile Save log | kyty.log",
		      "the chooser asks where to save, suggesting kyty.log");
		Check(KytyUi::ReadText(saved).find("A\xEF\xBF\xBD"
		                                   "B\n") != std::string::npos,
		      "the replacement character is written as UTF-8");
		// A write that fails (injected) names the file and the reason; the file and the log stay.
		const std::string before = KytyUi::ReadText(saved);
		chooser.file_answers.push_back(saved);
		core::SetAtomicFileFaultForTesting([](core::AtomicFileStep step) {
			return step == core::AtomicFileStep::Write ? ENOSPC : 0;
		});
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_s, GDK_CONTROL_MASK), "Ctrl+S again");
		if (GtkWindow* alert = KytyUi::FindAlert("The log could not be saved"); alert != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
			Check(text.find("Cannot write " + saved.string() + ": " + g_strerror(ENOSPC)) !=
			          std::string::npos,
			      "the error names the file and the reason (" + text + ")");
			KytyUi::CloseError(alert);
		}
		core::SetAtomicFileFaultForTesting({});
		Check(KytyUi::ReadText(saved) == before, "the file is unchanged");
		Check(KytyUi::LogLines(window) == lines, "the log is unchanged");
	});
}

// ---- 10.9, LFI-40: the Emulator is missing

KYTY_UI_CASE(run_emulator_missing) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path                     emulator = ui.AppDirectory() / "kyty_emulator";
	const KytyUi::FakeEmulatorBehavior fake("--stdout kept-line --exit 0");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the first run to end");
		// Gone after the start: the run-time check.
		std::error_code ec;
		fs::remove(emulator, ec);
		gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_r, GDK_CONTROL_MASK),
		      "Ctrl+R is handled");
		const std::string text = CannotStartText();
		Check(text.find("Cannot start " + emulator.string() + ": " + g_strerror(ENOENT)) !=
		          std::string::npos,
		      "the error names the Emulator's absolute path and the reason (" + text + ")");
		Check(Run(window).Stats().runs_refused == 1 && Run(window).Stats().runs_started == 1,
		      "nothing started");
		CheckNothingRuns(application, window, "a missing Emulator");
		Check(KytyUi::HasLine(KytyUi::LogLines(window), "kept-line"),
		      "the log of the last run stays (the check comes before the log is cleared)");
	});
	// Missing at startup: the banner, and the same error.
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		if (KytyUi::WaitForLibrary(window)) {
			GtkWidget* banner = KytyUi::FindByBuildableId(GTK_WIDGET(window), "emulator_banner");
			Check(banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE &&
			          std::string(adw_banner_get_title(ADW_BANNER(banner))) ==
			              "Emulator not found at " + emulator.string(),
			      "the banner names the missing Emulator");
			if (StartGame(application, window, kAlphaTitle)) {
				const std::string text = CannotStartText();
				Check(text.find(emulator.string()) != std::string::npos,
				      "Run shows the error instead of quitting (" + text + ")");
				CheckNothingRuns(application, window, "an Emulator missing at startup");
			}
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the second run ends with status 0");
}

// ---- 10.9: an Emulator that cannot be executed

KYTY_UI_CASE(run_emulator_not_executable) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path emulator = ui.AppDirectory() / "kyty_emulator";
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		std::error_code ec;
		// A regular file without execute permission (written as any file is: no chmod).
		fs::remove(emulator, ec);
		KytyUi::WriteText(emulator, "not a program\n");
		if (StartGame(application, window, kAlphaTitle)) {
			const std::string text = CannotStartText();
			Check(text.find("Cannot start " + emulator.string() + ": " + g_strerror(EACCES)) !=
			          std::string::npos,
			      "a file without execute permission: the path and the reason (" + text + ")");
			CheckNothingRuns(application, window, "a file without execute permission");
		}
		// A directory where the Emulator should be.
		fs::remove(emulator, ec);
		fs::create_directory(emulator, ec);
		if (StartGame(application, window, kAlphaTitle)) {
			const std::string text = CannotStartText();
			Check(text.find("Cannot start " + emulator.string() + ": it is not a regular file") !=
			          std::string::npos,
			      "a directory: the path and the reason (" + text + ")");
			CheckNothingRuns(application, window, "a directory");
		}
		Check(Run(window).Stats().runs_started == 0 && Run(window).Stats().runs_refused == 2,
		      "nothing started");
	});
}

// ---- 10.9: the spawn fails after the checks

KYTY_UI_CASE(run_spawn_failure) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path                     emulator = ui.AppDirectory() / "kyty_emulator";
	const KytyUi::FakeEmulatorBehavior fake("--stdout old-line --exit 0");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the first run to end");
		core::SetSpawnFaultForTesting(
		    [](core::SpawnStep step) { return step == core::SpawnStep::Spawn ? EAGAIN : 0; });
		const bool started = StartGame(application, window, kAlphaTitle);
		core::SetSpawnFaultForTesting({});
		if (!started) {
			return;
		}
		const std::string text  = CannotStartText();
		const auto        lines = KytyUi::LogLines(window);
		Check(text.find("Cannot start " + emulator.string() + ": " + g_strerror(EAGAIN)) !=
		              std::string::npos &&
		          text.find("(working directory " + ui.AppDirectory().string() + ")") !=
		              std::string::npos,
		      "the error names the Emulator, the reason, and the working directory (" + text + ")");
		Check(lines.size() == 1 && text.find(lines.front()) != std::string::npos,
		      "the log was cleared for the run and has the error as its one line");
		Check(Run(window).Stats().runs_refused == 1 && Run(window).Stats().runs_started == 1,
		      "nothing started");
		CheckNothingRuns(application, window, "a failed spawn");
		// The next run works again.
		g_setenv("KYTY_UI_FAKE_ARGS", "--stdout recovered --exit 0", TRUE);
		if (StartGame(application, window, kAlphaTitle)) {
			KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 2; }, 8000,
			                "the next run to end");
			Check(KytyUi::HasLine(KytyUi::LogLines(window), "recovered"), "the next run works");
		}
	});
}

// ---- SIGINT and SIGTERM while the quit question is open

KYTY_UI_CASE(run_quit_question_signal) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	// As main() does first.
	kyty::gtk::InstallQuitSignalHandlers();
	for (const auto& [number, name]: {std::pair<int, const char*> {SIGINT, "SIGINT"},
	                                  std::pair<int, const char*> {SIGTERM, "SIGTERM"}}) {
		const fs::path    signals = ui.Root() / (std::string("signals-") + name);
		const std::string args = "--record-signals " + signals.string() + " --stdout ready --wait";
		const KytyUi::FakeEmulatorBehavior fake(args.c_str());
		bool                               asked = false;
		const int status = ui.RunApplication([&](KytyApplication* application,
		                                         KytyMainWindow*  window) {
			if (!KytyUi::WaitForLibrary(window) || !StartGame(application, window, kAlphaTitle) ||
			    !WaitRunning(window, true) || WaitForLine(window, "ready") == 0) {
				KytyUi::QuitApplication(application);
				return;
			}
			KytyUi::QuitApplication(application);
			GtkWindow* question = KytyUi::FindAlert("Stop the running game and quit?");
			asked               = question != nullptr;
			::kill(::getpid(), number);
			// The launcher stops the game without asking again, and the question closes.
			KytyUi::WaitFor(
			    [question]() { return question == nullptr || !KytyUi::IsToplevel(question); }, 8000,
			    std::string("the question to close after ") + name);
			Check(!kyty::gtk::MainWindowGameRunning(window),
			      std::string("the game has ended after ") + name);
		});
		Check(asked, std::string("the question was open when ") + name + " came");
		Check(status == 0, std::string("the launcher ends with status 0 after ") + name + " (" +
		                       std::to_string(status) + ")");
		Check(Signals(signals) == "15\n",
		      std::string("the game got SIGTERM after ") + name + " (" + Signals(signals) + ")");
	}
}

// ---- D27: the game ends by itself while the quit question is open

KYTY_UI_CASE(run_quit_question_game_exits) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	// app.quit, then the game exits with 0; closing the window, then the game exits with 3.
	for (const bool close_window: {false, true}) {
		const fs::path    signals = ui.Root() / (close_window ? "signals-close" : "signals-quit");
		const std::string args    = "--record-signals " + signals.string() +
		                            " --stdout ready --wait --exit " + (close_window ? "3" : "0");
		const KytyUi::FakeEmulatorBehavior fake(args.c_str());
		const char*                        how   = close_window ? "closing the window" : "app.quit";
		bool                               asked = false;
		bool                               closed = false;
		const int status = ui.RunApplication([&](KytyApplication* application,
		                                         KytyMainWindow*  window) {
			if (!KytyUi::WaitForLibrary(window) || !StartGame(application, window, kAlphaTitle) ||
			    !WaitRunning(window, true) || WaitForLine(window, "ready") == 0) {
				KytyUi::QuitApplication(application);
				return;
			}
			GtkWindow* main = GTK_WINDOW(window);
			if (close_window) {
				gtk_window_close(main);
			} else {
				KytyUi::QuitApplication(application);
			}
			GtkWindow* question = KytyUi::FindAlert("Stop the running game and quit?");
			asked               = question != nullptr;
			if (!asked) {
				KytyUi::QuitApplication(application);
				return;
			}
			// The game ends by itself (SIGUSR1 ends its --wait).
			::kill(Run(window).Stats().pid, SIGUSR1);
			closed = KytyUi::WaitFor([question]() { return !KytyUi::IsToplevel(question); }, 8000,
			                         std::string("the question to close when the game ends (") +
			                             how + ")");
			if (close_window) {
				// The window closes, and with it the application.
				KytyUi::WaitClosed(main, "the main window to close", 8000);
			}
		});
		Check(asked, std::string("the question was asked (") + how + ")");
		Check(closed, std::string("the question closed by itself (") + how + ")");
		Check(status == 0, std::string("the quit went on to status 0 (") + how + ", " +
		                       std::to_string(status) + ")");
		Check(Signals(signals) == "10\n", std::string("the launcher sent the game no signal (") +
		                                      how + ": " + Signals(signals) + ")");
	}
}

// ---- The log pane hidden and shown (D27)

namespace {

// The fake Emulator's arguments for 100 lines in three parts: line-1 ... line-60, then (after a
// SIGUSR1) line-61 ... line-80, then line-81 ... line-100; it waits after each part.
std::string ThreePartLines(const fs::path& signals) {
	std::string args = "--record-signals " + signals.string();
	for (int i = 1; i <= 100; i++) {
		args += " --stdout line-" + std::to_string(i);
		if (i == 60 || i == 80 || i == 100) {
			args += " --wait";
		}
	}
	return args;
}

bool RowShown(KytyMainWindow* window, std::string_view text) {
	const auto rows = RowTexts(window);
	return std::find(rows.begin(), rows.end(), text) != rows.end();
}

double LogScrollValue(KytyMainWindow* window) {
	GtkWidget* scrolled =
	    gtk_widget_get_ancestor(GTK_WIDGET(KytyUi::LogList(window)), GTK_TYPE_SCROLLED_WINDOW);
	return scrolled != nullptr ? gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(
	                                 GTK_SCROLLED_WINDOW(scrolled)))
	                           : -1;
}

// The rows and the scroll position, for failure messages.
std::string Describe(KytyMainWindow* window) {
	const auto rows = RowTexts(window);
	return std::to_string(rows.size()) + " rows " + (rows.empty() ? "-" : rows.front()) + " ... " +
	       (rows.empty() ? "-" : rows.back()) + ", scrolled to " +
	       std::to_string(LogScrollValue(window));
}

} // namespace

// Shown while it follows the end: the first painted frame (ShowLogPane waits for it) has the
// newest row at the bottom, and the view follows new output.
KYTY_UI_CASE(run_log_hidden_then_shown) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path                     signals = ui.Root() / "signals";
	const std::string                  args    = ThreePartLines(signals);
	const KytyUi::FakeEmulatorBehavior fake(args.c_str());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		const StopAtEnd stop(window);
		Check(gtk_widget_get_visible(KytyUi::LogPane(window)) == FALSE, "the log is hidden");
		if (!StartGame(application, window, kAlphaTitle) || WaitForLine(window, "line-60") == 0) {
			return;
		}
		Check(KytyUi::LogRows(window).empty(), "the hidden log shows no row");
		// Shown for the first time during the run.
		if (!KytyUi::ShowLogPane(window, true)) {
			return;
		}
		Check(RowShown(window, "line-60") && LogAtEnd(window),
		      "the first frame of the shown log has the newest line at the bottom (" +
		          Describe(window) + ")");
		Check(KytyUi::Capture(GTK_WINDOW(window), "log-shown"), "the shown log renders");
		// More lines while it is shown: it follows them.
		::kill(Run(window).Stats().pid, SIGUSR1);
		WaitForLine(window, "line-80");
		KytyUi::WaitFor([&]() { return RowShown(window, "line-80") && LogAtEnd(window); }, 3000,
		                "the log to follow the new lines to the end");
		// Hidden at the bottom, more lines while hidden, shown again: the newest line.
		KytyUi::ShowLogPane(window, false);
		::kill(Run(window).Stats().pid, SIGUSR1);
		WaitForLine(window, "line-100");
		if (KytyUi::ShowLogPane(window, true)) {
			Check(RowShown(window, "line-100") && LogAtEnd(window),
			      "shown again, it has the line written while it was hidden at the bottom (" +
			          Describe(window) + ")");
		}
		Check(KytyUi::LogLines(window).size() == 100, "all 100 lines are in the log");
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
		// Hidden during the next run, then shown: the next run's line (cleared since).
		KytyUi::ShowLogPane(window, false);
		g_setenv("KYTY_UI_FAKE_ARGS", "--stdout next-run --exit 0", TRUE);
		if (StartGame(application, window, kAlphaTitle)) {
			KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 2; }, 8000,
			                "the next run to end");
			if (KytyUi::ShowLogPane(window, true)) {
				Check(RowTexts(window) == std::vector<std::string> {"next-run"},
				      "the shown log has the next run's line only (" + Describe(window) + ")");
			}
		}
	});
	g_unsetenv("KYTY_UI_FAKE_ARGS");
}

// Scrolled up, hidden, more output, shown: the position is kept and the view does not follow.
KYTY_UI_CASE(run_log_scrolled_up_hidden) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path                     signals = ui.Root() / "signals";
	const std::string                  args    = ThreePartLines(signals);
	const KytyUi::FakeEmulatorBehavior fake(args.c_str());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		const StopAtEnd stop(window);
		if (!KytyUi::ShowLogPane(window, true) || !StartGame(application, window, kAlphaTitle) ||
		    WaitForLine(window, "line-60") == 0) {
			return;
		}
		KytyUi::WaitFor([&]() { return RowShown(window, "line-60") && LogAtEnd(window); }, 3000,
		                "the shown log to follow the first lines to the end");
		// The user scrolls to the top.
		GtkWidget* scrolled =
		    gtk_widget_get_ancestor(GTK_WIDGET(KytyUi::LogList(window)), GTK_TYPE_SCROLLED_WINDOW);
		if (!Check(scrolled != nullptr, "the log list is in a scrolled window")) {
			return;
		}
		gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)),
		                         0);
		KytyUi::WaitForPaintedFrame(KytyUi::LogPane(window));
		const auto top = RowTexts(window);
		if (!Check(!top.empty() && top.front() == "line-1" && !LogAtEnd(window),
		           "scrolled up, the log shows its first line (" + Describe(window) + ")")) {
			return;
		}
		// Hidden, more lines, shown again: where the user left it, without following.
		KytyUi::ShowLogPane(window, false);
		::kill(Run(window).Stats().pid, SIGUSR1);
		WaitForLine(window, "line-80");
		if (KytyUi::ShowLogPane(window, true)) {
			const auto rows = RowTexts(window);
			Check(!rows.empty() && rows.front() == "line-1" && LogScrollValue(window) < 1 &&
			          !RowShown(window, "line-80") && !LogAtEnd(window),
			      "shown again, the log is where the user left it (" + Describe(window) + ")");
		}
		// More lines while it is shown: it still does not follow.
		::kill(Run(window).Stats().pid, SIGUSR1);
		WaitForLine(window, "line-100");
		KytyUi::Pump(300);
		const auto rows = RowTexts(window);
		Check(!rows.empty() && rows.front() == "line-1" && !RowShown(window, "line-100") &&
		          !LogAtEnd(window),
		      "new lines do not move a view the user scrolled up (" + Describe(window) + ")");
		Check(KytyUi::LogLines(window).size() == 100, "all 100 lines are in the log");
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
	});
}

// ---- D27: the whole line in each row's tooltip

KYTY_UI_CASE(run_log_tooltip) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	std::string long_line = "tooltip:";
	while (long_line.size() < 400) {
		long_line += "-a-line-much-wider-than-the-log-pane";
	}
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!KytyUi::ShowLogPane(window, true)) {
			return;
		}
		const std::string args =
		    "--stdout " + long_line + " --stdout-hex 0a --stdout short --exit 0";
		g_setenv("KYTY_UI_FAKE_ARGS", args.c_str(), TRUE);
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 1; }, 8000,
		                "the game to end");
		const auto tooltip = [](GtkWidget* row) -> std::string {
			const char* text = gtk_widget_get_tooltip_text(row);
			return text != nullptr ? text : "(none)";
		};
		const auto row_of = [window](std::string_view text) -> GtkWidget* {
			for (GtkWidget* row: KytyUi::LogRows(window)) {
				if (KytyUi::WidgetText(row) == text) {
					return row;
				}
			}
			return nullptr;
		};
		KytyUi::WaitFor([&]() { return row_of("short") != nullptr; }, 3000, "the rows to be shown");
		GtkWidget* long_row = row_of(long_line);
		Check(long_row != nullptr && tooltip(long_row) == long_line,
		      "the long row's tooltip is its whole line");
		Check(long_row != nullptr &&
		          gtk_widget_get_width(long_row) < static_cast<int>(long_line.size()) * 4,
		      "the long row is narrower than its text (it is ellipsized)");
		GtkWidget* short_row = row_of("short");
		Check(short_row != nullptr && tooltip(short_row) == "short",
		      "a short row's tooltip is its line");
		GtkWidget* empty_row = row_of("");
		Check(empty_row != nullptr && gtk_widget_get_tooltip_text(empty_row) == nullptr,
		      "an empty line has no tooltip");
		// The rows are reused for the next run's lines: the tooltips follow.
		g_setenv("KYTY_UI_FAKE_ARGS", "--stdout next-first --stdout next-second --exit 0", TRUE);
		if (StartGame(application, window, kAlphaTitle)) {
			KytyUi::WaitFor([window]() { return Run(window).Stats().exits == 2; }, 8000,
			                "the next run to end");
			KytyUi::WaitFor([&]() { return row_of("next-second") != nullptr; }, 3000,
			                "the next run's rows");
			for (const char* line: {"next-first", "next-second"}) {
				GtkWidget* row = row_of(line);
				Check(row != nullptr && tooltip(row) == line,
				      std::string("the reused row of ") + line + " has its tooltip");
			}
			Check(row_of(long_line) == nullptr, "no row shows the old long line");
		}
	});
	g_unsetenv("KYTY_UI_FAKE_ARGS");
}

// ---- D27: "No output yet"

KYTY_UI_CASE(run_log_placeholder) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		if (!KytyUi::ShowLogPane(window, true)) {
			return;
		}
		GtkWidget* placeholder = kyty::gtk::MainWindowLogView(window).Placeholder();
		const auto shown       = [placeholder]() {
			return placeholder != nullptr && gtk_widget_get_mapped(placeholder) != FALSE;
		};
		Check(shown() && KytyUi::WidgetText(placeholder) == "No output yet" &&
		          gtk_widget_has_css_class(placeholder, "dim-label") != FALSE &&
		          gtk_widget_is_ancestor(placeholder, KytyUi::LogPane(window)) != FALSE,
		      "the empty log pane shows a dimmed \"No output yet\"");
		Check(KytyUi::Capture(GTK_WINDOW(window), "log-empty"), "the empty log pane renders");
		g_setenv("KYTY_UI_FAKE_ARGS", "--stdout first --wait", TRUE);
		if (!StartGame(application, window, kAlphaTitle) || WaitForLine(window, "first") == 0) {
			return;
		}
		KytyUi::WaitFor([&]() { return !shown(); }, 2000, "the first line to hide the placeholder");
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
		Check(!shown(), "the placeholder stays hidden while the log keeps the lines");
		// The next run clears the log: the placeholder is back until its first line.
		g_setenv("KYTY_UI_FAKE_ARGS", "--sleep-ms 1500 --stdout late --wait", TRUE);
		if (!StartGame(application, window, kAlphaTitle)) {
			return;
		}
		Check(KytyUi::LogLines(window).empty(), "the next run cleared the log");
		KytyUi::WaitFor([&]() { return shown(); }, 1000,
		                "the placeholder to show for the cleared log");
		WaitForLine(window, "late");
		KytyUi::WaitFor([&]() { return !shown(); }, 2000, "the late line to hide it again");
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
	});
	g_unsetenv("KYTY_UI_FAKE_ARGS");
}

// ---- 10.4, 11.18, LFI-11: the running indicator

KYTY_UI_CASE(run_running_indicator) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const KytyUi::FakeEmulatorBehavior fake("--stdout ready --wait");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&         library = KytyUi::ContextOf(application).library;
		GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
		GtkWidget*    list    = GTK_WIDGET(KytyUi::ListViewOf(window));
		const auto    enabled = [actions](const char* action) {
			return g_action_group_get_action_enabled(actions, action) != FALSE;
		};
		KytyUi::SelectTitle(library, kAlphaTitle);
		Check(enabled("compress") && enabled("remove-save-data") && enabled("run") &&
		          !enabled("stop"),
		      "before the run Alpha Game can be compressed, its save data removed, and run");
		if (!StartGame(application, window, kAlphaTitle) || !WaitRunning(window, true)) {
			return;
		}
		KytyUi::WaitFor([&]() { return ShowsRunning(list, kAlphaTitle); }, 2000,
		                "Alpha Game's row to show the running emblem");
		Check(TitleBold(list, kAlphaTitle), "Alpha Game's title is bold");
		Check(!ShowsRunning(list, kBetaTitle) && !TitleBold(list, kBetaTitle),
		      "Beta Quest shows no running indicator");
		Check(!enabled("run") && enabled("stop") && !enabled("compress") &&
		          !enabled("remove-save-data"),
		      "for the running game: Run, Compress, and Remove save data off, Stop on (11.18)");
		KytyUi::SelectTitle(library, kBetaTitle);
		Check(!enabled("run") && enabled("compress") && enabled("remove-save-data"),
		      "for another game: Run off (one game at a time), Compress and Remove save data on");
		// The grid view's tile shows it too.
		if (KytyUi::ShowGridView(application, window, true)) {
			GtkWidget* grid = GTK_WIDGET(KytyUi::GridViewOf(window));
			KytyUi::WaitFor([&]() { return ShowsRunning(grid, kAlphaTitle); }, 2000,
			                "Alpha Game's tile to show the running emblem");
			Check(!ShowsRunning(grid, kBetaTitle), "Beta Quest's tile shows none");
			KytyUi::ShowGridView(application, window, false);
		}
		KytyUi::ActivateGameAction(window, "stop");
		WaitRunning(window, false);
		KytyUi::WaitFor([&]() { return !ShowsRunning(list, kAlphaTitle); }, 2000,
		                "the running emblem to go away");
		Check(!TitleBold(list, kAlphaTitle), "the title is no longer bold");
		KytyUi::SelectTitle(library, kAlphaTitle);
		Check(enabled("run") && !enabled("stop") && enabled("compress") &&
		          enabled("remove-save-data"),
		      "after the exit everything is on again for Alpha Game");
	});
}
