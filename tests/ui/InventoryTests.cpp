// UI tests of the Launcher_Feature_Inventory (task 22.3; Requirement 6.2; design.md
// "Launcher_Feature_Inventory"; phase 7 plan D10, D16, D17, D21, D22), each on Wayland and on X11:
// one case lfi_NN per row LFI-01 ... LFI-47. Each case opens the window that the row's
// "GTK_Launcher location" names, reaches the function through the control, menu, or keys of the
// row's "Keys" column (KeyPress and MoveFocus emulate GTK's key dispatch, plan D10, G4), performs
// it once against the fakes (scripted file chooser, recording desktop launcher, fake controller,
// loopback HTTP, the fake Emulator), and checks what the user would see or what it wrote. Most
// cases run the real KytyApplication (UiContext::RunApplication, plan D13) over the fixture games;
// the cases that must change the context before the main window exists (LFI-21's --local mode,
// LFI-35's installed release, LFI-37's portable Settings_Store) and LFI-09 (the title sound: the
// application's finalize quits SDL, under the test's player) open a main window over a context
// of their own. Rows that the GTK launcher replaced or does by itself still get a case:
// - LFI-41 (no terminal run mode): the log pane receives the fake Emulator's standard output and
//   standard error;
// - LFI-46 (no theme selector): the colors follow the desktop's color-scheme (the fake portal), a
//   stored Qt theme changes nothing, and no editor offers a theme;
// - LFI-37: the Settings_Store is the user file without a Kyty.ini in the working directory and
//   that Kyty.ini (portable mode) with one, through the context's Environment;
// - LFI-04, LFI-47: the stored values come back after an application restart in the same case.
// LFI-30 checks the scrollable save data failure list (W3), LFI-35 the IEC units of the update
// download progress (W2).

#include "launcher/core/cheats.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/gameLibrary.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/inputMapping.h"
#include "launcher/core/saveData.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/additionalContentWindow.h"
#include "launcher/gtk/bindingCaptureDialog.h"
#include "launcher/gtk/byteSize.h"
#include "launcher/gtk/cheatsWindow.h"
#include "launcher/gtk/compressionDialog.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gamePickerDialog.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/inputMappingWindow.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/logModel.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/runController.h"
#include "launcher/gtk/services/titleSoundPlayer.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/trophyDetailsDialog.h"
#include "launcher/gtk/trophyOverviewWindow.h"
#include "launcher/gtk/trophyWindow.h"
#include "launcher/gtk/updateProgressWindow.h"
#include "launcher/gtk/updateUi.h"
#include "support/updaterGenerators.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "updater/core/updatePolicy.h"
#include "updater/core/updateResult.h"

#include <adwaita.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtk/gtk.h>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;
using KytyUi::Check;

using Drive = std::function<void(KytyApplication*, KytyMainWindow*)>;

constexpr const char* kAlpha = "Alpha Game";
constexpr const char* kBeta  = "Beta Quest";

// ---- Files and the Settings_Store

std::string ReadText(const fs::path& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

bool WriteText(const fs::path& path, std::string_view text) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	return Check(static_cast<bool>(out), "write " + path.string());
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::exists(path, ec);
}

// What Kyty.ini holds now (the defaults when it does not exist).
core::LauncherSettings Stored(KytyUi::UiContext& ui) {
	return core::OpenSettingsStore(ui.SettingsFile()).loaded.settings;
}

// Changes Kyty.ini (before the application or the context reads it).
bool Store(KytyUi::UiContext& ui, const std::function<void(core::LauncherSettings&)>& change) {
	auto store = core::OpenSettingsStore(ui.SettingsFile());
	auto next  = store.loaded.settings;
	change(next);
	const auto written = core::SaveSettingsStore(store, next);
	return Check(static_cast<bool>(written), "write " + ui.SettingsFile().string() +
	                                             (written ? "" : ": " + written.error().message));
}

core::GameEntry FixtureEntry(const fs::path& games, std::string_view title) {
	core::GameEntry entry;
	if (title == kAlpha) {
		entry.path     = games / "Alpha Game";
		entry.title_id = "PPSA01001";
	} else {
		entry.path     = games / "Beta" / "Beta Quest";
		entry.title_id = "PPSA01002";
	}
	entry.title_name = std::string(title);
	return entry;
}

// The game's own settings in Kyty.ini: the global configuration with user_name.
bool StoreCustomUserName(KytyUi::UiContext& ui, const core::GameEntry& entry,
                         const std::string& user_name) {
	return Store(ui, [&](core::LauncherSettings& settings) {
		auto configuration      = core::EnableCustomSettings(settings, entry);
		configuration.user_name = user_name;
		Check(static_cast<bool>(core::SaveCustomSettings(settings, entry, configuration)),
		      "give " + entry.title_name + " its own settings");
	});
}

// The user name the game runs with according to Kyty.ini.
std::string StoredUserName(KytyUi::UiContext& ui, const core::GameEntry& entry) {
	return core::EnableCustomSettings(Stored(ui), entry).user_name;
}

// The fixture games in a Game_Folder of Kyty.ini and the compatibility database on the server;
// returns the games' folder (empty after a Fail).
fs::path PrepareLibrary(KytyUi::UiContext& ui) {
	const auto games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return {};
	}
	ui.ServeCompatibility();
	return games;
}

// ---- The application

void Quit(KytyApplication* application) {
	g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
}

// The first scan, the games' details, and the compatibility statuses are there.
bool WaitForLibrary(KytyMainWindow* window) {
	auto& library = kyty::gtk::MainWindowLibrary(window);
	return KytyUi::WaitFor(
	    [&library]() {
		    const auto& stats = library.Stats();
		    return stats.scans_applied >= 1 && stats.details_done >= 1 &&
		           stats.compatibility_finished;
	    },
	    8000, "the games, their details, and the compatibility statuses");
}

// RunApplication: drive runs once the library is there, then the application quits; the case
// fails unless it ends with status 0.
void RunWithLibrary(KytyUi::UiContext& ui, const Drive& drive,
                    kyty::updater::UpdateUiMode mode = kyty::updater::UpdateUiMode::Other) {
	const int status = ui.RunApplication(
	    [&drive](KytyApplication* application, KytyMainWindow* window) {
		    if (WaitForLibrary(window)) {
			    drive(application, window);
		    }
		    Quit(application);
	    },
	    mode);
	Check(status == 0, "the application ends with status 0 (" + std::to_string(status) + ")");
}

kyty::gtk::LauncherContext& ContextOf(KytyApplication* application) {
	return *kyty::gtk::ApplicationContext(application);
}

KytyGame* GameByTitle(kyty::gtk::LauncherContext& context, std::string_view title) {
	GListModel* games = context.library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		if (kyty_game_get_entry(game.get()).title_name == title) {
			return game.get(); // the store keeps its reference
		}
	}
	return nullptr;
}

// Selects the listed game titled title (as a click on its row does); false after a Fail.
bool SelectTitle(kyty::gtk::LauncherContext& context, std::string_view title) {
	KytyGame* game = GameByTitle(context, title);
	return Check(game != nullptr && context.library.Select(kyty_game_get_entry(game).path) &&
	                 context.library.Selected() == game,
	             "select " + std::string(title));
}

std::vector<std::string> SortedTitles(const kyty::gtk::GameListModel& library) {
	std::vector<std::string> titles;
	GListModel*              sorted = library.Sorted();
	for (guint i = 0; i < g_list_model_get_n_items(sorted); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(sorted, i)));
		titles.push_back(kyty_game_get_entry(game.get()).title_name);
	}
	return titles;
}

// The titles in the order core::SortRows gives for the library's games.
std::vector<std::string> ExpectedTitles(const kyty::gtk::GameListModel& library,
                                        core::Column column, bool descending) {
	std::vector<core::GameRow> rows;
	std::vector<std::string>   titles;
	GListModel*                games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		rows.push_back(kyty_game_get_row(game.get()));
		titles.push_back(kyty_game_get_entry(game.get()).title_name);
	}
	std::vector<std::string> expected;
	for (const std::size_t index: core::SortRows(rows, column, descending)) {
		expected.push_back(titles[index]);
	}
	return expected;
}

GtkColumnView* ListView(KytyMainWindow* window) {
	return GTK_COLUMN_VIEW(KytyUi::FindByType(GTK_WIDGET(window), GTK_TYPE_COLUMN_VIEW));
}

GtkColumnViewColumn* ColumnById(GtkColumnView* view, std::string_view id) {
	GListModel* columns = gtk_column_view_get_columns(view);
	for (guint i = 0; i < g_list_model_get_n_items(columns); i++) {
		auto column = GObjectPtr<GtkColumnViewColumn>::Adopt(
		    static_cast<GtkColumnViewColumn*>(g_list_model_get_item(columns, i)));
		if (g_strcmp0(gtk_column_view_column_get_id(column.get()), std::string(id).c_str()) == 0) {
			return column.get(); // the view keeps its reference
		}
	}
	return nullptr;
}

std::string Describe(GtkWidget* widget) {
	if (widget == nullptr) {
		return "(no focus)";
	}
	return KytyUi::RoleName(widget) + " \"" + KytyUi::ControlName(widget) + "\" (" +
	       G_OBJECT_TYPE_NAME(widget) + ")";
}

// Tab to the list view, the arrow keys to the row of title, and Space to select it unless it is
// selected (Space toggles); the focused row, or nullptr after a Fail.
GtkWidget* FocusGameRow(KytyMainWindow* window, std::string_view title) {
	GtkWindow*     gtk_window = GTK_WINDOW(window);
	GtkColumnView* view       = ListView(window);
	const auto     in_view    = [gtk_window, view]() -> GtkWidget* {
		GtkWidget* focus = gtk_window_get_focus(gtk_window);
		return focus != nullptr && view != nullptr &&
		               gtk_widget_is_ancestor(focus, GTK_WIDGET(view)) != FALSE
		           ? focus
		           : nullptr;
	};
	for (int i = 0; i < 40 && in_view() == nullptr; i++) {
		KytyUi::MoveFocus(gtk_window, GTK_DIR_TAB_FORWARD);
	}
	if (!Check(in_view() != nullptr, "Tab reaches the game list")) {
		return nullptr;
	}
	for (const guint key: {GDK_KEY_Down, GDK_KEY_Up}) {
		for (int i = 0; i < 10; i++) {
			GtkWidget* row = in_view();
			if (row != nullptr && KytyUi::AccessibleName(row) == title) {
				if ((gtk_widget_get_state_flags(row) & GTK_STATE_FLAG_SELECTED) == 0) {
					Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_space),
					      "Space selects the row of " + std::string(title));
				}
				return row;
			}
			KytyUi::KeyPress(GTK_WIDGET(window), key);
			KytyUi::Pump(20);
		}
	}
	KytyUi::Fail("the arrow keys never reach the row of " + std::string(title) +
	             " (the focus is on " + Describe(in_view()) + ")");
	return nullptr;
}

// The mapped toplevel of type, or nullptr.
GtkWindow* MappedToplevel(GType type) {
	GListModel* toplevels = gtk_window_get_toplevels();
	for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
		auto window = GObjectPtr<GtkWindow>::Adopt(
		    static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
		if (G_TYPE_CHECK_INSTANCE_TYPE(window.get(), type) &&
		    gtk_widget_get_mapped(GTK_WIDGET(window.get())) != FALSE) {
			return window.get();
		}
	}
	return nullptr;
}

// Waits for a mapped and painted toplevel of type; nullptr after a Fail.
GtkWindow* WaitForWindow(GType type, const std::string& what, int timeout_ms = 5000) {
	GtkWindow* found = nullptr;
	if (KytyUi::WaitFor(
	        [&found, type]() {
		        found = MappedToplevel(type);
		        return found != nullptr;
	        },
	        timeout_ms, what) &&
	    KytyUi::WaitForPaintedFrame(GTK_WIDGET(found))) {
		return found;
	}
	return nullptr;
}

// Escape (on the window itself: a GtkText of the window would need a pointer device) closes it.
bool CloseWithEscape(GtkWindow* window, const std::string& what) {
	Check(KytyUi::KeyPressOn(GTK_WIDGET(window), GDK_KEY_Escape), "Escape is handled in " + what);
	return KytyUi::WaitFor([window]() { return !KytyUi::IsToplevel(window); }, 3000,
	                       "Escape to close " + what);
}

// The mapped button below root whose action is action, or nullptr.
GtkWidget* ButtonWithAction(GtkWidget* root, std::string_view action) {
	return KytyUi::FindFirst(root, [action](GtkWidget* widget) {
		if (!GTK_IS_BUTTON(widget) || gtk_widget_get_mapped(widget) == FALSE) {
			return false;
		}
		const char* name = gtk_actionable_get_action_name(GTK_ACTIONABLE(widget));
		return name != nullptr && action == name;
	});
}

// The button below root labelled with one of labels (its own text), or nullptr.
GtkWidget* ButtonLabelled(GtkWidget* root, std::initializer_list<std::string_view> labels) {
	return KytyUi::FindFirst(root, [labels](GtkWidget* widget) {
		const std::string text = KytyUi::WidgetText(widget);
		return GTK_IS_BUTTON(widget) &&
		       std::find(labels.begin(), labels.end(), text) != labels.end();
	});
}

// The preferences row below root titled title, or nullptr.
GtkWidget* RowTitled(GtkWidget* root, std::string_view title) {
	return KytyUi::FindFirst(root, [title](GtkWidget* widget) {
		return ADW_IS_PREFERENCES_ROW(widget) && KytyUi::WidgetText(widget) == title;
	});
}

// The scale below root whose accessible label is name (its row has the same title), or nullptr.
GtkRange* ScaleNamed(GtkWidget* root, std::string_view name) {
	GtkWidget* found = KytyUi::FindFirst(root, [name](GtkWidget* widget) {
		return GTK_IS_SCALE(widget) &&
		       KytyUi::AccessibleProperty(widget, GTK_ACCESSIBLE_PROPERTY_LABEL) ==
		           std::optional<std::string>(std::string(name));
	});
	return found != nullptr ? GTK_RANGE(found) : nullptr;
}

// The page name of the view stack is shown (it exists and is visible).
bool PageShown(GtkWidget* root, const char* name) {
	auto*      stack = ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"));
	GtkWidget* child = stack != nullptr ? adw_view_stack_get_child_by_name(stack, name) : nullptr;
	return child != nullptr &&
	       adw_view_stack_page_get_visible(adw_view_stack_get_page(stack, child)) != FALSE;
}

// Closes an error alert with its Close button (GtkAlertDialog's own, "_Close" or "Close").
bool CloseError(GtkWindow* alert) {
	return KytyUi::CloseAlert(
	    alert, ButtonLabelled(GTK_WIDGET(alert), {"_Close"}) != nullptr ? "_Close" : "Close");
}

// Waits for a mapped toplevel other than except whose text contains text (an alert over a window
// that may show the same text, such as the update banner); nullptr after a Fail.
GtkWindow* FindDialog(std::string_view text, GtkWindow* except, int timeout_ms = 3000) {
	GtkWindow* found = nullptr;
	KytyUi::WaitFor(
	    [&]() {
		    GListModel* toplevels = gtk_window_get_toplevels();
		    for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
			    auto window = GObjectPtr<GtkWindow>::Adopt(
			        static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
			    if (window.get() != except &&
			        gtk_widget_get_mapped(GTK_WIDGET(window.get())) != FALSE &&
			        KytyUi::AllText(GTK_WIDGET(window.get())).find(text) != std::string::npos) {
				    found = window.get();
				    return true;
			    }
		    }
		    return false;
	    },
	    timeout_ms, "a window with the text \"" + std::string(text) + "\"");
	return found;
}

// The game's action (the game menu's item, the header bar button, the key all activate it).
bool ActivateGameAction(KytyMainWindow* window, const char* action) {
	GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
	if (!Check(g_action_group_get_action_enabled(actions, action) != FALSE,
	           std::string("game.") + action + " is enabled")) {
		return false;
	}
	g_action_group_activate_action(actions, action, nullptr);
	return true;
}

// Popovers need a grab with autohide, and the headless sessions have no seat (plan D10).
void DisableAutohide(GtkWidget* root) {
	for (GtkWidget* widget:
	     KytyUi::FindAll(root, [](GtkWidget* candidate) { return GTK_IS_POPOVER(candidate); })) {
		gtk_popover_set_autohide(GTK_POPOVER(widget), FALSE);
	}
}

// The mapped popover below root whose text contains text, or nullptr.
GtkPopover* MappedPopover(GtkWidget* root, std::string_view text) {
	GtkWidget* found = KytyUi::FindFirst(root, [text](GtkWidget* widget) {
		return GTK_IS_POPOVER(widget) && gtk_widget_get_mapped(widget) != FALSE &&
		       KytyUi::AllText(widget).find(text) != std::string::npos;
	});
	return found != nullptr ? GTK_POPOVER(found) : nullptr;
}

// The menu item below root (a GtkModelButton of a popover menu) whose label is label.
GtkWidget* MenuItem(GtkWidget* root, std::string_view label) {
	return KytyUi::FindFirst(root, [label](GtkWidget* widget) {
		if (g_strcmp0(G_OBJECT_TYPE_NAME(widget), "GtkModelButton") != 0) {
			return false;
		}
		const std::string text = KytyUi::AllText(widget);
		return text.substr(0, text.find('\n')) == label;
	});
}

// Opens the menu button's popover (autohide off), clicks its item labelled label, and waits until
// the popover is closed again. False when the popover or the item is missing.
bool ClickMenuItem(GtkWidget* button, std::string_view label) {
	if (button == nullptr || !GTK_IS_MENU_BUTTON(button)) {
		return false;
	}
	GtkPopover* popover = KytyUi::OpenPopover(GTK_MENU_BUTTON(button));
	if (popover == nullptr) {
		return false;
	}
	GtkWidget* item = MenuItem(GTK_WIDGET(popover), label);
	if (item != nullptr) {
		g_signal_emit_by_name(item, "clicked");
	}
	if (gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE) {
		gtk_popover_popdown(popover);
	}
	KytyUi::WaitFor([popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) == FALSE; },
	                2000, "the menu to close after \"" + std::string(label) + "\"");
	return item != nullptr;
}

// Every item of a menu model, sections and submenus included: (label, action).
void MenuItems(GMenuModel* model, std::vector<std::pair<std::string, std::string>>& items) {
	for (int i = 0; i < g_menu_model_get_n_items(model); i++) {
		gchar* label  = nullptr;
		gchar* action = nullptr;
		g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
		g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
		if (action != nullptr) {
			items.emplace_back(label != nullptr ? label : "", action);
		}
		g_free(label);
		g_free(action);
		for (const char* link: {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU}) {
			if (GMenuModel* linked = g_menu_model_get_item_link(model, i, link);
			    linked != nullptr) {
				MenuItems(linked, items);
				g_object_unref(linked);
			}
		}
	}
}

// The text lines of the log pane's model.
std::vector<std::string> LogLines(KytyMainWindow* window) {
	std::vector<std::string> lines;
	GListModel*              log = G_LIST_MODEL(kyty::gtk::MainWindowLog(window));
	for (guint i = 0; i < g_list_model_get_n_items(log); i++) {
		auto line = GObjectPtr<GtkStringObject>::Adopt(
		    static_cast<GtkStringObject*>(g_list_model_get_item(log, i)));
		lines.emplace_back(gtk_string_object_get_string(line.get()));
	}
	return lines;
}

bool HasLine(const std::vector<std::string>& lines, std::string_view line) {
	return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// Sets the fake Emulator's behavior for the next runs (fake-kyty-emulator.sh); unset again by the
// destructor.
class FakeEmulatorArgs {
public:
	explicit FakeEmulatorArgs(const char* args) { g_setenv("KYTY_UI_FAKE_ARGS", args, TRUE); }
	~FakeEmulatorArgs() { g_unsetenv("KYTY_UI_FAKE_ARGS"); }
	FakeEmulatorArgs(const FakeEmulatorArgs&)            = delete;
	FakeEmulatorArgs& operator=(const FakeEmulatorArgs&) = delete;
};

// ---- A main window over a context of the case's own

// For the cases that set the context up before the main window exists: the context over the
// case's services (the launcher directory UiContext::AppDirectory()), a main window over it
// without an application (its app.* actions are then its own), and both destroyed in order.
class OwnSession {
public:
	OwnSession(KytyUi::UiContext& ui, kyty::updater::UpdateUiMode mode, bool local,
	           const std::function<void(kyty::gtk::LauncherContext&)>& prepare = {}) {
		ui.ParentWindow(); // the launcher's style sheet and icons
		m_context = kyty::gtk::CreateLauncherContext({.working_directory  = ui.WorkingDirectory(),
		                                              .launcher_directory = ui.AppDirectory(),
		                                              .update_mode        = mode,
		                                              .local              = local},
		                                             ui.Services().Get());
		if (prepare) {
			prepare(*m_context);
		}
		m_window     = kyty_main_window_new(*m_context, nullptr);
		m_gtk_window = GTK_WINDOW(m_window);
		gtk_window_present(m_gtk_window);
		if (!KytyUi::WaitFor(
		        [this]() { return gtk_widget_get_mapped(GTK_WIDGET(m_window)) != FALSE; }, 5000,
		        "the main window to be mapped") ||
		    !KytyUi::WaitForPaintedFrame(GTK_WIDGET(m_window))) {
			m_failed = true;
		}
	}
	~OwnSession() {
		// By address: the window may be gone (a cast of a destroyed window is a critical).
		if (m_gtk_window != nullptr && KytyUi::IsToplevel(m_gtk_window)) {
			gtk_window_destroy(m_gtk_window);
		}
		KytyUi::Pump(100);
		m_context.reset();
		KytyUi::Pump(50);
	}
	OwnSession(const OwnSession&)            = delete;
	OwnSession& operator=(const OwnSession&) = delete;

	[[nodiscard]] bool                        Ok() const { return !m_failed; }
	[[nodiscard]] kyty::gtk::LauncherContext& Context() { return *m_context; }
	[[nodiscard]] KytyMainWindow*             Window() { return m_window; }

private:
	std::unique_ptr<kyty::gtk::LauncherContext> m_context;
	KytyMainWindow*                             m_window     = nullptr;
	GtkWindow*                                  m_gtk_window = nullptr;
	bool                                        m_failed     = false;
};

// ---- A synthetic ATRAC9 title sound (the builder of tests/launcher_core/TitleSoundDecodeTests.cpp
// and of the phase 6 harness): 48 kHz, 256 samples per frame, 4 frames in a 384-byte superframe.

namespace at9 {

constexpr std::size_t kSuperframeBytes = 384;
constexpr int         kFrameSamples    = 256;
constexpr int         kFrames          = 4;

void AppendLe16(std::string& out, uint32_t value) {
	out += static_cast<char>(value & 0xffu);
	out += static_cast<char>((value >> 8u) & 0xffu);
}

void AppendLe32(std::string& out, uint32_t value) {
	AppendLe16(out, value & 0xffffu);
	AppendLe16(out, value >> 16u);
}

std::string Chunk(std::string_view id, const std::string& payload) {
	std::string chunk(id);
	AppendLe32(chunk, static_cast<uint32_t>(payload.size()));
	chunk += payload;
	if (payload.size() % 2 != 0) {
		chunk += '\0';
	}
	return chunk;
}

// A mono file of superframes superframes (about 8.5 s for 400).
std::string File(int superframes) {
	static const uint8_t kMono[4] = {0xfe, 0x70, 0x0b, 0xf0};
	std::string          fmt;
	AppendLe16(fmt, 0xfffe);
	AppendLe16(fmt, 1);
	AppendLe32(fmt, 48000);
	AppendLe32(fmt, 48000 * kSuperframeBytes / (kFrames * kFrameSamples));
	AppendLe16(fmt, kSuperframeBytes);
	AppendLe16(fmt, 0);
	AppendLe16(fmt, 0x22);
	AppendLe16(fmt, kFrameSamples);
	AppendLe32(fmt, 0x4u);
	static const uint8_t kGuid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                  0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	fmt.append(reinterpret_cast<const char*>(kGuid), sizeof(kGuid));
	AppendLe32(fmt, 1);
	fmt.append(reinterpret_cast<const char*>(kMono), 4);
	fmt.resize(0x34, '\0');
	static const uint8_t kMonoBlock[36] = {0, 0, 0x04, 0x20, 0x04, 0xc0, 0, 0, 0x1f, 0xc0};
	std::string          data;
	for (int superframe = 0; superframe < superframes; superframe++) {
		std::string bytes;
		for (int frame = 0; frame < kFrames; frame++) {
			std::string block(reinterpret_cast<const char*>(kMonoBlock), sizeof(kMonoBlock));
			if (frame != 0) {
				block[0] = static_cast<char>(block[0] | 0x80);
			}
			bytes += block;
		}
		bytes.resize(kSuperframeBytes, '\0');
		data += bytes;
	}
	const std::string body = Chunk("fmt ", fmt) + Chunk("data", data);
	std::string       file = "RIFF";
	AppendLe32(file, static_cast<uint32_t>(4 + body.size()));
	file += "WAVE";
	return file + body;
}

} // namespace at9

// ---- LFI-30: save data removal with failures injected (never chmod)

// Fails for every save file (*.bin); everything else is removed on the host.
class FailingSaveFiles final: public core::PathRemover {
public:
	std::error_code Remove(const fs::path& path) override {
		if (path.extension() == ".bin") {
			return std::make_error_code(std::errc::permission_denied);
		}
		return core::HostPathRemover().Remove(path);
	}
};

} // namespace

// ---- LFI-01 ... LFI-47 (design.md "Launcher_Feature_Inventory")

// LFI-01: the list view with its eight columns, each game's values, and the row tooltip; Tab into
// the list and the arrow keys.
KYTY_UI_CASE(lfi_01) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&          context = ContextOf(application);
		GtkColumnView* view    = ListView(window);
		if (!Check(view != nullptr, "the main window has its list view")) {
			return;
		}
		std::vector<std::string> titles;
		GListModel*              columns = gtk_column_view_get_columns(view);
		for (guint i = 0; i < g_list_model_get_n_items(columns); i++) {
			auto column = GObjectPtr<GtkColumnViewColumn>::Adopt(
			    static_cast<GtkColumnViewColumn*>(g_list_model_get_item(columns, i)));
			titles.emplace_back(gtk_column_view_column_get_title(column.get()));
		}
		Check(titles == std::vector<std::string> {"Game", "Serial", "Game Version",
		                                          "Firmware Version", "Size", "Path", "Status",
		                                          "Comments"},
		      "the columns Game, Serial, Game Version, Firmware Version, Size, Path, Status, "
		      "Comments");
		KytyGame* alpha = GameByTitle(context, kAlpha);
		if (!Check(alpha != nullptr, "Alpha Game is listed")) {
			return;
		}
		const core::GameRow& row  = kyty_game_get_row(alpha);
		const std::string    text = KytyUi::AllText(GTK_WIDGET(view));
		const auto           size = kyty_game_get_size(alpha);
		for (const std::string& value:
		     {std::string(kAlpha), row.title_id, row.game_version, row.firmware_version,
		      size ? kyty::gtk::FormatByteSize(*size) : std::string("?"), row.path,
		      std::string(kyty::gtk::StatusText(core::CompatibilityStatus::InGame)),
		      std::string("UI test fixture")}) {
			Check(!value.empty() && text.find(value) != std::string::npos,
			      "the list shows Alpha Game's \"" + value + "\"");
		}
		Check(row.title_id == "PPSA01001" && row.game_version == "01.000",
		      "Alpha Game's serial and version come from its param.json");
		// The row tooltip (one query-tooltip handler on the view, gameViewSupport.h).
		const std::string tooltip = kyty::gtk::GameTooltip(alpha);
		Check(gtk_widget_get_has_tooltip(GTK_WIDGET(view)) != FALSE &&
		          tooltip.find("Serial ID: PPSA01001") != std::string::npos &&
		          tooltip.find("Game version: 01.000") != std::string::npos &&
		          tooltip.find("Path: " + row.path) != std::string::npos,
		      "the row tooltip has the details: " + tooltip);
		// Tab into the list, the arrow keys to a row.
		GtkWidget* focused = FocusGameRow(window, kBeta);
		Check(focused != nullptr && context.library.Selected() == GameByTitle(context, kBeta),
		      "Tab and the arrow keys select Beta Quest's row");
		FocusGameRow(window, kAlpha);
		Check(context.library.Selected() == alpha, "the Up arrow selects Alpha Game's row");
	});
}

// LFI-02: the grid view (Ctrl+G) shows each game's icon and name and shares the list's selection.
KYTY_UI_CASE(lfi_02) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      context = ContextOf(application);
		GtkWidget* root    = GTK_WIDGET(window);
		GtkWidget* views   = KytyUi::FindByBuildableId(root, "views_stack");
		SelectTitle(context, kAlpha);
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK) && views != nullptr &&
		          g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(views)), "grid") == 0,
		      "Ctrl+G shows the grid view");
		auto* grid = GTK_GRID_VIEW(KytyUi::FindByType(root, GTK_TYPE_GRID_VIEW));
		if (!Check(grid != nullptr && gtk_widget_get_mapped(GTK_WIDGET(grid)) != FALSE,
		           "the grid view is shown")) {
			return;
		}
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(grid));
		GtkSelectionModel* model = gtk_grid_view_get_model(grid);
		Check(model == GTK_SELECTION_MODEL(context.library.Selection()),
		      "the grid view shows the list's selection model");
		Check(gtk_single_selection_get_selected_item(context.library.Selection()) ==
		          GameByTitle(context, kAlpha),
		      "Alpha Game, selected in the list, is selected in the grid");
		const std::string text = KytyUi::AllText(GTK_WIDGET(grid));
		Check(text.find(kAlpha) != std::string::npos && text.find(kBeta) != std::string::npos,
		      "the grid shows the games' names");
		Check(kyty_game_get_texture(GameByTitle(context, kAlpha)) != nullptr &&
		          KytyUi::FindFirst(GTK_WIDGET(grid),
		                            [](GtkWidget* widget) {
			                            return GTK_IS_PICTURE(widget) || GTK_IS_IMAGE(widget);
		                            }) != nullptr,
		      "the grid items show the games' icons");
		// A selection in the grid is the list's selection.
		const guint position = [&]() {
			GListModel* sorted = context.library.Sorted();
			for (guint i = 0; i < g_list_model_get_n_items(sorted); i++) {
				auto game = GObjectPtr<KytyGame>::Adopt(
				    static_cast<KytyGame*>(g_list_model_get_item(sorted, i)));
				if (kyty_game_get_entry(game.get()).title_name == kBeta) {
					return i;
				}
			}
			return GTK_INVALID_LIST_POSITION;
		}();
		gtk_selection_model_select_item(model, position, TRUE);
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK) &&
		          g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(views)), "list") == 0,
		      "Ctrl+G shows the list view again");
		Check(context.library.Selected() == GameByTitle(context, kBeta) &&
		          gtk_column_view_get_model(ListView(window)) ==
		              GTK_SELECTION_MODEL(context.library.Selection()),
		      "Beta Quest, selected in the grid, is selected in the list");
	});
}

// LFI-03: the icon width slider, 128-320 in steps of 16, default 224, only in the grid view; Tab
// and the arrow keys.
KYTY_UI_CASE(lfi_03) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root  = GTK_WIDGET(window);
		GtkWidget* scale = KytyUi::FindByBuildableId(root, "icon_width_scale");
		if (!Check(scale != nullptr && GTK_IS_SCALE(scale), "the header bar has the slider")) {
			return;
		}
		Check(gtk_widget_get_visible(scale) == FALSE, "the slider is hidden in the list view");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-grid", nullptr);
		KytyUi::WaitFor([scale]() { return gtk_widget_get_mapped(scale) != FALSE; }, 2000,
		                "the slider to show in the grid view");
		GtkAdjustment* adjustment = gtk_range_get_adjustment(GTK_RANGE(scale));
		Check(gtk_adjustment_get_lower(adjustment) == 128 &&
		          gtk_adjustment_get_upper(adjustment) == 320 &&
		          gtk_adjustment_get_step_increment(adjustment) == 16,
		      "the slider goes from 128 to 320 in steps of 16");
		Check(gtk_range_get_value(GTK_RANGE(scale)) == 224, "the default icon width is 224");
		Check(!KytyUi::AccessibleName(scale).empty(), "the slider has an accessible name");
		// Tab reaches it; the Right arrow adds one step.
		gtk_window_set_focus(GTK_WINDOW(window), nullptr);
		bool reached = false;
		for (int i = 0; i < 40 && !reached; i++) {
			GtkWidget* focus = KytyUi::MoveFocus(GTK_WINDOW(window), GTK_DIR_TAB_FORWARD);
			reached = focus == scale || (focus != nullptr && gtk_widget_is_ancestor(focus, scale));
		}
		Check(reached, "Tab reaches the slider");
		KytyUi::KeyPress(root, GDK_KEY_Right);
		Check(gtk_range_get_value(GTK_RANGE(scale)) == 240, "Right makes the icons 240 wide");
		// A value between the steps snaps to the nearest step.
		gtk_range_set_value(GTK_RANGE(scale), 250);
		Check(gtk_range_get_value(GTK_RANGE(scale)) == 256, "250 snaps to 256");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-grid", nullptr);
		Check(gtk_widget_get_visible(scale) == FALSE, "the slider hides with the list view");
	});
}

// LFI-04: the view mode and the icon width are stored ([Launcher] grid_view, grid_icon_width) and
// come back after a restart.
KYTY_UI_CASE(lfi_04) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [&ui](KytyApplication* /*application*/, KytyMainWindow* window) {
		GtkWidget* root = GTK_WIDGET(window);
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK), "Ctrl+G is handled");
		Check(Stored(ui).grid_view, "Ctrl+G stores grid_view=true at once");
		GtkWidget* scale = KytyUi::FindByBuildableId(root, "icon_width_scale");
		gtk_range_set_value(GTK_RANGE(scale), 288);
		KytyUi::WaitFor([&ui]() { return Stored(ui).grid_icon_width == 288; }, 3000,
		                "the icon width 288 to be stored after the slider stopped");
	});
	Check(Stored(ui).grid_view && Stored(ui).grid_icon_width == 288,
	      "Kyty.ini holds the grid view and the icon width 288");
	// The restart.
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root  = GTK_WIDGET(window);
		GtkWidget* views = KytyUi::FindByBuildableId(root, "views_stack");
		GtkWidget* scale = KytyUi::FindByBuildableId(root, "icon_width_scale");
		Check(views != nullptr &&
		          g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(views)), "grid") == 0,
		      "after the restart the grid view is shown");
		GVariant* state =
		    g_action_group_get_action_state(G_ACTION_GROUP(application), "toggle-grid");
		Check(state != nullptr && g_variant_get_boolean(state) != FALSE,
		      "app.toggle-grid is checked after the restart");
		if (state != nullptr) {
			g_variant_unref(state);
		}
		Check(scale != nullptr && gtk_range_get_value(GTK_RANGE(scale)) == 288 &&
		          gtk_widget_get_visible(scale) != FALSE,
		      "the slider shows the stored width 288 after the restart");
	});
}

// LFI-05: the search by name or serial (Ctrl+F focuses it, Escape clears it).
KYTY_UI_CASE(lfi_05) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      library = ContextOf(application).library;
		GtkWidget* root    = GTK_WIDGET(window);
		GtkWidget* search  = KytyUi::FindByBuildableId(root, "search_entry");
		if (!Check(search != nullptr && GTK_IS_SEARCH_ENTRY(search),
		           "the header bar has a search entry (with its clear button)")) {
			return;
		}
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_f, GDK_CONTROL_MASK), "Ctrl+F is handled");
		GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window));
		Check(focus != nullptr && (focus == search || gtk_widget_is_ancestor(focus, search)),
		      "Ctrl+F focuses the search entry");
		const auto shown = [&library]() { return SortedTitles(library); };
		gtk_editable_set_text(GTK_EDITABLE(search), "beta");
		KytyUi::WaitFor([&]() { return shown() == std::vector<std::string> {kBeta}; }, 2000,
		                "a search by name (\"beta\") to show Beta Quest only");
		gtk_editable_set_text(GTK_EDITABLE(search), "PPSA01004");
		KytyUi::WaitFor([&]() { return shown() == std::vector<std::string> {"Gamma Archive"}; },
		                2000, "a search by serial (PPSA01004) to show Gamma Archive only");
		gtk_editable_set_text(GTK_EDITABLE(search), "no such game");
		KytyUi::WaitFor([&]() { return shown().empty(); }, 2000,
		                "a search that matches nothing to show no game");
		Check(KytyUi::KeyPressOn(search, GDK_KEY_Escape), "Escape is handled in the search");
		KytyUi::WaitFor([&]() { return shown().size() == 5; }, 2000,
		                "Escape to clear the search and show the 5 games");
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(search))).empty(),
		      "Escape empties the search entry");
	});
}

// LFI-06: sorting by a column header (a click on another column sorts ascending, on the sort
// column it reverses), the title ascending by default, and the keyboard path of plan D18 (the
// primary menu's Sort by, whose keys keyboard_sort drives).
KYTY_UI_CASE(lfi_06) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&          library = ContextOf(application).library;
		GtkColumnView* view    = ListView(window);
		Check(library.SortColumn() == core::Column::Title && !library.SortDescending() &&
		          SortedTitles(library) == ExpectedTitles(library, core::Column::Title, false),
		      "the default sort is the title, ascending");
		const auto click = [view](const char* id) {
			auto*      sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(view));
			auto*      column = ColumnById(view, id);
			const bool on     = gtk_column_view_sorter_get_primary_sort_column(sorter) == column;
			const auto order =
			    on && gtk_column_view_sorter_get_primary_sort_order(sorter) == GTK_SORT_ASCENDING
			        ? GTK_SORT_DESCENDING
			        : GTK_SORT_ASCENDING;
			gtk_column_view_sort_by_column(view, column, order);
		};
		click("serial");
		Check(library.SortColumn() == core::Column::TitleId && !library.SortDescending() &&
		          SortedTitles(library) == ExpectedTitles(library, core::Column::TitleId, false),
		      "a click on the Serial header sorts by serial, ascending");
		click("serial");
		Check(library.SortColumn() == core::Column::TitleId && library.SortDescending() &&
		          SortedTitles(library) == ExpectedTitles(library, core::Column::TitleId, true),
		      "a second click on the Serial header reverses the order");
		click("size");
		Check(library.SortColumn() == core::Column::Size && !library.SortDescending() &&
		          SortedTitles(library) == ExpectedTitles(library, core::Column::Size, false),
		      "a click on the Size header sorts by size, ascending");
		// The menu's actions (F10, Sort by: keyboard_sort drives the keys).
		g_action_group_activate_action(G_ACTION_GROUP(application), "sort-column",
		                               g_variant_new_string("path"));
		Check(library.SortColumn() == core::Column::Path &&
		          SortedTitles(library) == ExpectedTitles(library, core::Column::Path, false),
		      "Sort by, Path sorts by path");
		auto* sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(view));
		Check(gtk_column_view_sorter_get_primary_sort_column(sorter) == ColumnById(view, "path"),
		      "the Path header shows the sort chosen in the menu");
	});
}

// LFI-07: Refresh (F5 and the header bar button) lists games added on disk.
KYTY_UI_CASE(lfi_07) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	RunWithLibrary(ui, [&games](KytyApplication* application, KytyMainWindow* window) {
		auto&      library    = ContextOf(application).library;
		auto&      controller = kyty::gtk::MainWindowLibrary(window);
		GtkWidget* root       = GTK_WIDGET(window);
		const auto count      = [&library]() { return g_list_model_get_n_items(library.Games()); };
		Check(count() == 5, "5 games before the refresh");
		Check(KytyUi::WriteSyntheticGames(games, 1) == 1, "a new game on disk");
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		const auto applied = controller.Stats().scans_applied;
		Check(KytyUi::KeyPress(root, GDK_KEY_F5), "F5 is handled");
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > applied; }, 8000,
		                "F5 to rescan");
		Check(count() == 6 && GameByTitle(ContextOf(application), "Synthetic Game 1") != nullptr,
		      "F5 lists the new game");
		Check(KytyUi::WriteSyntheticGames(games / "more", 2) == 2, "two more games on disk");
		GtkWidget* refresh = ButtonWithAction(root, "app.refresh");
		Check(refresh != nullptr && gtk_widget_get_tooltip_text(refresh) != nullptr,
		      "the header bar has the Refresh button with a tooltip");
		const auto again = controller.Stats().scans_applied;
		Check(KytyUi::Activate(refresh), "a click on Refresh");
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > again; }, 8000,
		                "the Refresh button to rescan");
		Check(count() == 8,
		      "the Refresh button lists the two new games (" + std::to_string(count()) + " games)");
	});
}

// LFI-08: the selected game's background art (sce_sys/pic0.png) behind the list, with the scrim.
KYTY_UI_CASE(lfi_08) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      context = ContextOf(application);
		GtkWidget* root    = GTK_WIDGET(window);
		GtkWidget* art     = KytyUi::FindByBuildableId(root, "background_art");
		GtkWidget* scrim   = KytyUi::FindByBuildableId(root, "art_scrim");
		if (!Check(art != nullptr && GTK_IS_PICTURE(art) && scrim != nullptr,
		           "the library has the background picture and its scrim")) {
			return;
		}
		Check(gtk_widget_get_visible(art) == FALSE, "no art without a selection");
		SelectTitle(context, kAlpha);
		KytyUi::WaitFor(
		    [art]() {
			    return gtk_widget_get_visible(art) != FALSE &&
			           gtk_picture_get_paintable(GTK_PICTURE(art)) != nullptr;
		    },
		    5000, "Alpha Game's pic0.png behind the list");
		GdkPaintable* paintable = gtk_picture_get_paintable(GTK_PICTURE(art));
		Check(paintable != nullptr && gdk_paintable_get_intrinsic_width(paintable) == 960 &&
		          gdk_paintable_get_intrinsic_height(paintable) == 540,
		      "the art is Alpha Game's 960x540 pic0.png");
		Check(gtk_widget_get_visible(scrim) != FALSE, "the translucent scrim is shown with it");
		Check(KytyUi::Capture(GTK_WINDOW(window), "art"), "the main window with the art renders");
		SelectTitle(context, "Broken Metadata");
		KytyUi::WaitFor([art]() { return gtk_widget_get_visible(art) == FALSE; }, 5000,
		                "the art to go for a game without pic0.png");
		Check(gtk_widget_get_visible(scrim) == FALSE, "the scrim goes with it");
	});
}

// LFI-09: the title sound (snd0.at9) plays when a game is selected and no game runs (SDL's dummy
// audio driver).
KYTY_UI_CASE(lfi_09) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty() ||
	    !WriteText(games / "Alpha Game" / "sce_sys" / "snd0.at9", at9::File(400))) {
		return;
	}
	auto&                       sound   = ui.Services().title_sound;
	kyty::gtk::LauncherContext& context = ui.Context();
	ui.ParentWindow();
	KytyMainWindow* window = kyty_main_window_new(context, nullptr);
	gtk_window_present(GTK_WINDOW(window));
	if (KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                    5000, "the main window to be mapped") &&
	    WaitForLibrary(window)) {
		const fs::path alpha = games / "Alpha Game";
		SelectTitle(context, kAlpha);
		KytyUi::WaitFor([&]() { return sound.PlayingEntry() == core::EntryId(alpha); }, 5000,
		                "Alpha Game's title sound to play when it is selected");
		SelectTitle(context, kBeta);
		KytyUi::WaitFor([&]() { return !sound.Playing(); }, 1000,
		                "the sound to stop for a game without snd0.at9");
		SelectTitle(context, kAlpha);
		KytyUi::WaitFor([&]() { return sound.PlayingEntry() == core::EntryId(alpha); }, 5000,
		                "the sound to play again");
		{
			// No sound while a game runs (Requirement 8.10).
			const FakeEmulatorArgs args("--stdout ready --wait");
			ActivateGameAction(window, "run");
			KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
			                "the game to run");
			Check(!sound.Playing(), "the title sound stops when the game starts");
			SelectTitle(context, kBeta);
			SelectTitle(context, kAlpha);
			KytyUi::Pump(500);
			Check(!sound.Playing(), "no title sound plays while the game runs");
			ActivateGameAction(window, "stop");
			KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
			                "the game to stop");
		}
		gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(context.library.Selection()));
		KytyUi::WaitFor([&]() { return !sound.Playing(); }, 1000,
		                "the sound to stop without a selection");
	}
	gtk_window_destroy(GTK_WINDOW(window));
}

// LFI-10: Run (row activation as a double-click does, the header bar's Run button, Enter, Ctrl+R)
// starts the selected game.
KYTY_UI_CASE(lfi_10) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		auto&          context = ContextOf(application);
		auto&          run     = kyty::gtk::MainWindowRun(window);
		GtkWidget*     root    = GTK_WIDGET(window);
		GtkColumnView* view    = ListView(window);
		KytyGame*      alpha   = GameByTitle(context, kAlpha);
		const auto     ran     = [&](uint64_t runs, const std::string& how) {
			KytyUi::WaitFor(
			    [&]() { return run.Stats().runs_started >= runs && run.Stats().exits >= runs; },
			    8000, how + " to run the game and the game to exit");
			const std::string args = ReadText(ui.Root() / "emulator-args");
			Check(args.find(kyty_game_get_entry(alpha).path.string()) != std::string::npos,
			      how + ": the Emulator got Alpha Game's path");
			fs::remove(ui.Root() / "emulator-args");
		};
		// The row's activation (a double-click or Enter on a row emits it).
		guint       position = GTK_INVALID_LIST_POSITION;
		GListModel* sorted   = context.library.Sorted();
		for (guint i = 0; i < g_list_model_get_n_items(sorted); i++) {
			auto game = GObjectPtr<KytyGame>::Adopt(
			    static_cast<KytyGame*>(g_list_model_get_item(sorted, i)));
			if (game.get() == alpha) {
				position = i;
			}
		}
		g_signal_emit_by_name(view, "activate", position);
		ran(1, "activating the row");
		Check(context.library.Selected() == alpha, "activating the row selects the game");
		// The header bar's Run button.
		Check(KytyUi::Activate(ButtonWithAction(root, "game.run")), "a click on Run");
		ran(2, "the Run button");
		// Enter on the focused row, Ctrl+R.
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(root, GDK_KEY_Return), "Enter is handled");
		ran(3, "Enter");
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(root, GDK_KEY_r, GDK_CONTROL_MASK), "Ctrl+R is handled");
		ran(4, "Ctrl+R");
		Check(run.Stats().runs_started == 4 && run.Stats().runs_refused == 0,
		      "four runs, none refused");
	});
}

// LFI-11: while a game runs its row shows it (the bold title, the running flag), Run is off for
// every game, and Stop is on.
KYTY_UI_CASE(lfi_11) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const FakeEmulatorArgs args("--stdout ready --wait");
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&         context = ContextOf(application);
		GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
		KytyGame*     alpha   = GameByTitle(context, kAlpha);
		SelectTitle(context, kAlpha);
		Check(g_action_group_get_action_enabled(actions, "run") != FALSE &&
		          g_action_group_get_action_enabled(actions, "stop") == FALSE,
		      "before the run: Run on, Stop off");
		ActivateGameAction(window, "run");
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "the game to run");
		Check(kyty_game_get_running(alpha), "Alpha Game is marked running");
		KytyUi::WaitFor(
		    [window]() {
			    return KytyUi::FindFirst(GTK_WIDGET(ListView(window)), [](GtkWidget* widget) {
				           return GTK_IS_LABEL(widget) &&
				                  gtk_widget_has_css_class(widget, "game-running") != FALSE &&
				                  KytyUi::WidgetText(widget) == kAlpha;
			           }) != nullptr;
		    },
		    2000, "Alpha Game's title to be shown as running (bold)");
		Check(g_action_group_get_action_enabled(actions, "run") == FALSE &&
		          g_action_group_get_action_enabled(actions, "stop") != FALSE,
		      "while it runs: Run off, Stop on");
		SelectTitle(context, kBeta);
		Check(g_action_group_get_action_enabled(actions, "run") == FALSE,
		      "Run stays off for another game while one runs");
		Check(!kyty_game_get_running(GameByTitle(context, kBeta)), "Beta Quest is not running");
		Check(KytyUi::Capture(GTK_WINDOW(window), "running"), "the running game's row renders");
		ActivateGameAction(window, "stop");
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
		Check(!kyty_game_get_running(alpha) &&
		          g_action_group_get_action_enabled(actions, "run") != FALSE &&
		          g_action_group_get_action_enabled(actions, "stop") == FALSE,
		      "after the exit: not running, Run on, Stop off");
	});
}

// LFI-12: the game menu (Menu key or Shift+F10, and the header bar's game menu button) has every
// per-game action; its items run them.
KYTY_UI_CASE(lfi_12) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	RunWithLibrary(ui, [&ui, &games](KytyApplication* application, KytyMainWindow* window) {
		auto&      context = ContextOf(application);
		GtkWidget* root    = GTK_WIDGET(window);
		DisableAutohide(root);
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(root, GDK_KEY_Menu), "the Menu key is handled");
		GtkPopover* menu = nullptr;
		KytyUi::WaitFor(
		    [&]() {
			    menu = MappedPopover(root, "Open game folder");
			    return menu != nullptr;
		    },
		    2000, "the Menu key to open the game menu");
		if (menu == nullptr) {
			return;
		}
		std::vector<std::pair<std::string, std::string>> items;
		MenuItems(gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu)), items);
		std::vector<std::string> actions;
		for (const auto& [label, action]: items) {
			actions.push_back(action);
		}
		for (const char* action:
		     {"game.run", "game.open-folder", "game.trophies", "game.cheats",
		      "game.additional-content", "game.compress", "game.remove-save-data",
		      "game.edit-config", "game.clear-config"}) {
			Check(std::find(actions.begin(), actions.end(), action) != actions.end(),
			      std::string("the game menu has ") + action);
		}
		// The header bar's game menu has the same items.
		GtkWidget* button = KytyUi::FindByBuildableId(root, "game_menu_button");
		std::vector<std::pair<std::string, std::string>> header;
		if (Check(button != nullptr, "the header bar has the game menu button")) {
			MenuItems(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(button)), header);
		}
		Check(header == items, "the header bar's game menu has the same items");
		// An item runs its action: Open game folder.
		GtkWidget* open = MenuItem(GTK_WIDGET(menu), "Open game folder");
		if (Check(open != nullptr, "the menu shows Open game folder")) {
			g_signal_emit_by_name(open, "clicked");
		}
		const auto& opened = ui.Services().desktop_launcher.calls;
		KytyUi::WaitFor([&opened]() { return !opened.empty(); }, 3000,
		                "Open game folder to open the folder");
		Check(!opened.empty() && opened.back() == "OpenFolder " + (games / "Alpha Game").string(),
		      "the item opens Alpha Game's folder");
		if (gtk_widget_get_mapped(GTK_WIDGET(menu)) != FALSE) {
			gtk_popover_popdown(menu);
		}
		// Shift+F10; Cheats only for a PPSA title ID.
		SelectTitle(context, "Broken Metadata");
		FocusGameRow(window, "Broken Metadata");
		Check(KytyUi::KeyPress(root, GDK_KEY_F10, GDK_SHIFT_MASK), "Shift+F10 is handled");
		KytyUi::WaitFor(
		    [&]() {
			    menu = MappedPopover(root, "Open game folder");
			    return menu != nullptr;
		    },
		    2000, "Shift+F10 to open the game menu");
		Check(g_action_group_get_action_enabled(kyty::gtk::MainWindowGameActions(window),
		                                        "cheats") == FALSE,
		      "no cheats for a game without a PPSA title ID");
		if (menu != nullptr) {
			GtkWidget* cheats = MenuItem(GTK_WIDGET(menu), "Cheats (experimental)…");
			Check(cheats == nullptr || gtk_widget_get_mapped(cheats) == FALSE ||
			          gtk_widget_get_sensitive(cheats) == FALSE,
			      "the Cheats item is not offered for it");
			gtk_popover_popdown(menu);
		}
	});
}

// LFI-13: the global settings from the header bar button, Ctrl+comma, the primary menu, and the
// menu on the empty space of the list.
KYTY_UI_CASE(lfi_13) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* /*application*/, KytyMainWindow* window) {
		GtkWidget* root   = GTK_WIDGET(window);
		const auto opened = [](const std::string& how) {
			GtkWindow* settings =
			    WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, how + " to open the global settings");
			if (settings != nullptr) {
				Check(KytyUi::AllText(GTK_WIDGET(settings)).find("Game folders") !=
				          std::string::npos,
				      how + " opens the global editor (with its Folders page)");
				CloseWithEscape(settings, "the global settings");
			}
		};
		Check(KytyUi::Activate(ButtonWithAction(root, "app.global-settings")),
		      "a click on the header bar's Global settings");
		opened("the header bar button");
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_comma, GDK_CONTROL_MASK), "Ctrl+comma is handled");
		opened("Ctrl+comma");
		// The primary menu's Global settings… item.
		GtkWidget* primary = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_MENU_BUTTON(widget) &&
			       gtk_menu_button_get_primary(GTK_MENU_BUTTON(widget)) != FALSE;
		});
		std::vector<std::pair<std::string, std::string>> items;
		if (Check(primary != nullptr, "the main window has the primary menu")) {
			MenuItems(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(primary)), items);
		}
		Check(std::find(items.begin(), items.end(),
		                std::pair<std::string, std::string>("Global settings…",
		                                                    "app.global-settings")) != items.end(),
		      "the primary menu has Global settings… (app.global-settings)");
		if (Check(ClickMenuItem(primary, "Global settings…"),
		          "a click on the primary menu's Global settings…")) {
			opened("the primary menu");
		}
		// A right-click on the list's empty space (its secondary-button gesture).
		DisableAutohide(root);
		GtkColumnView* view        = ListView(window);
		GtkGesture*    click       = nullptr;
		GListModel*    controllers = gtk_widget_observe_controllers(GTK_WIDGET(view));
		for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
			auto controller = GObjectPtr<GtkEventController>::Adopt(
			    static_cast<GtkEventController*>(g_list_model_get_item(controllers, i)));
			if (GTK_IS_GESTURE_CLICK(controller.get()) &&
			    gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(controller.get())) ==
			        GDK_BUTTON_SECONDARY) {
				click = GTK_GESTURE(controller.get());
			}
		}
		g_object_unref(controllers);
		if (!Check(click != nullptr, "the list has its right-click gesture")) {
			return;
		}
		const double x = 40;
		const double y = gtk_widget_get_height(GTK_WIDGET(view)) - 20;
		g_signal_emit_by_name(click, "pressed", 1, x, y);
		GtkPopover* menu = nullptr;
		KytyUi::WaitFor(
		    [&]() {
			    menu = MappedPopover(root, "Global settings…");
			    return menu != nullptr &&
			           KytyUi::AllText(GTK_WIDGET(menu)).find("Run") == std::string::npos;
		    },
		    2000, "a right-click on the empty space to open its menu");
		if (menu != nullptr) {
			GtkWidget* item = MenuItem(GTK_WIDGET(menu), "Global settings…");
			if (Check(item != nullptr, "the empty space menu has Global settings…")) {
				g_signal_emit_by_name(item, "clicked");
				opened("the empty space menu");
			}
			if (gtk_widget_get_mapped(GTK_WIDGET(menu)) != FALSE) {
				gtk_popover_popdown(menu);
			}
		}
	});
}

// LFI-14: the header bar's buttons (Refresh, Global settings, Input mapping, Edit game config,
// Clear game config, Trophy overview) have tooltips and accessible names, Tab reaches them, and
// each runs its action.
KYTY_UI_CASE(lfi_14) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty() || !StoreCustomUserName(ui, FixtureEntry(games, kAlpha), "Alpha Player")) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root = GTK_WIDGET(window);
		// Edit and Clear game config need a selected game with its own settings.
		SelectTitle(ContextOf(application), kAlpha);
		KytyUi::Pump(50);
		const char* actions[] = {"app.refresh",      "app.global-settings", "app.input-mapping",
		                         "game.edit-config", "game.clear-config",   "app.trophy-overview"};
		gtk_window_set_focus(GTK_WINDOW(window), nullptr);
		std::vector<GtkWidget*> stops;
		for (int i = 0; i < 60; i++) {
			GtkWidget* focus = KytyUi::MoveFocus(GTK_WINDOW(window), GTK_DIR_TAB_FORWARD);
			if (focus == nullptr || std::find(stops.begin(), stops.end(), focus) != stops.end()) {
				break;
			}
			stops.push_back(focus);
		}
		for (const char* action: actions) {
			GtkWidget* button = ButtonWithAction(root, action);
			if (!Check(button != nullptr,
			           std::string("the header bar has the button of ") + action)) {
				continue;
			}
			const char* tooltip = gtk_widget_get_tooltip_text(button);
			Check(tooltip != nullptr && *tooltip != '\0',
			      std::string("the button of ") + action + " has a tooltip");
			Check(!KytyUi::AccessibleName(button).empty(),
			      std::string("the button of ") + action + " has an accessible name (\"" +
			          KytyUi::AccessibleName(button) + "\")");
			Check(std::find(stops.begin(), stops.end(), button) != stops.end(),
			      std::string("Tab reaches the button of ") + action);
		}
		// Each opens its window: Input mapping, Trophy overview, Edit game config.
		const std::pair<const char*, GType> windows[] = {
		    {"app.input-mapping", KYTY_TYPE_INPUT_MAPPING_WINDOW},
		    {"app.trophy-overview", KYTY_TYPE_TROPHY_OVERVIEW_WINDOW},
		    {"game.edit-config", KYTY_TYPE_SETTINGS_WINDOW}};
		for (const auto& [action, type]: windows) {
			Check(KytyUi::Activate(ButtonWithAction(root, action)),
			      std::string("a click on the button of ") + action);
			if (GtkWindow* opened =
			        WaitForWindow(type, std::string(action) + " to open its window");
			    opened != nullptr) {
				CloseWithEscape(opened, std::string("the window of ") + action);
			}
		}
		Check(KytyUi::Activate(ButtonWithAction(root, "game.clear-config")),
		      "a click on Clear game config");
		if (GtkWindow* alert = KytyUi::FindAlert("Clear the game config?"); alert != nullptr) {
			KytyUi::CloseAlert(alert, "Cancel");
		}
	});
}

// LFI-15: Edit game config (Ctrl+E) opens the game's editor with its effective values; Save
// stores them as its own settings.
KYTY_UI_CASE(lfi_15) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	const auto        alpha = FixtureEntry(games, kAlpha);
	if (games.empty() || !StoreCustomUserName(ui, alpha, "Alpha Player")) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* /*application*/, KytyMainWindow* window) {
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_e, GDK_CONTROL_MASK),
		      "Ctrl+E is handled");
		GtkWindow* editor = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "Ctrl+E to open the editor");
		if (editor == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(editor);
		Check(KytyUi::AllText(root).find(kAlpha) != std::string::npos,
		      "the editor names Alpha Game");
		Check(PageShown(root, "general") && !PageShown(root, "folders") &&
		          !PageShown(root, "controller"),
		      "the game's editor has the game's pages, not the global Folders and Controller");
		GtkWidget* name = RowTitled(root, "User name");
		if (!Check(name != nullptr && ADW_IS_ENTRY_ROW(name), "the editor has the User name row")) {
			return;
		}
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(name))) == "Alpha Player",
		      "the editor shows the game's own user name");
		gtk_editable_set_text(GTK_EDITABLE(name), "Alpha Two");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		KytyUi::WaitFor([editor]() { return !KytyUi::IsToplevel(editor); }, 3000,
		                "Save to close the editor");
	});
	Check(StoredUserName(ui, alpha) == "Alpha Two", "Kyty.ini has Alpha Game's new user name");
	Check(Stored(ui).global.user_name == "Kyty", "the global user name is unchanged");
}

// LFI-16: Clear game config (Delete in the list) asks first; then the game uses the global
// settings.
KYTY_UI_CASE(lfi_16) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	const auto        alpha = FixtureEntry(games, kAlpha);
	if (games.empty() || !StoreCustomUserName(ui, alpha, "Alpha Player")) {
		return;
	}
	RunWithLibrary(ui, [&ui, &alpha](KytyApplication* /*application*/, KytyMainWindow* window) {
		GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
		FocusGameRow(window, kAlpha);
		Check(g_action_group_get_action_enabled(actions, "clear-config") != FALSE,
		      "Clear game config is on for a game with its own settings");
		// Cancel keeps them.
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Delete), "Delete is handled");
		if (GtkWindow* alert = KytyUi::FindAlert("Clear the game config?"); alert != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(alert)).find("Alpha Game") != std::string::npos,
			      "the confirmation names the game");
			KytyUi::CloseAlert(alert, "Cancel");
		}
		Check(core::HasCustomSettings(Stored(ui), alpha), "Cancel keeps the game's settings");
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Delete), "Delete again");
		if (GtkWindow* alert = KytyUi::FindAlert("Clear the game config?"); alert != nullptr) {
			KytyUi::CloseAlert(alert, "Clear game config");
		}
		KytyUi::WaitFor([&]() { return !core::HasCustomSettings(Stored(ui), alpha); }, 3000,
		                "Clear game config to remove the game's settings from Kyty.ini");
		KytyUi::WaitFor(
		    [actions]() {
			    return g_action_group_get_action_enabled(actions, "clear-config") == FALSE;
		    },
		    2000, "Clear game config to be off afterwards");
	});
	Check(StoredUserName(ui, alpha) == "Kyty", "Alpha Game uses the global settings again");
}

// LFI-17: the global settings' Game configs menu: Export writes one chosen game's <TITLE_ID>.json,
// Import applies a file to every matching game after a confirmation.
KYTY_UI_CASE(lfi_17) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path exported = ui.Root() / "export" / "PPSA01002.json";
	const fs::path import   = ui.Root() / "import.json";
	WriteText(import, "{\"PPSA01001\": {\"user_name\": \"Imported\"}, \"PPSA01002\": "
	                  "{\"user_name\": \"Imported\"}, \"PPSA09999\": {\"user_name\": \"Other\"}}");
	std::error_code ec;
	fs::create_directories(exported.parent_path(), ec);
	RunWithLibrary(ui, [&](KytyApplication* /*application*/, KytyMainWindow* window) {
		Check(KytyUi::Activate(ButtonWithAction(GTK_WIDGET(window), "app.global-settings")),
		      "open the global settings");
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings == nullptr) {
			return;
		}
		GtkWidget* menu = KytyUi::FindByBuildableId(GTK_WIDGET(settings), "game_configs_button");
		Check(menu != nullptr && GTK_IS_MENU_BUTTON(menu) && !KytyUi::AccessibleName(menu).empty(),
		      "the editor has the Game configs menu");
		std::vector<std::pair<std::string, std::string>> items;
		if (menu != nullptr) {
			MenuItems(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(menu)), items);
		}
		Check(items.size() == 2 && items[0].second == "settings.import-game-configs" &&
		          items[1].second == "settings.export-game-config",
		      "its items are Import… and Export…");
		// Export (the menu's item): the picker, then the file.
		auto& chooser = ui.Services().file_chooser;
		chooser.file_answers.push_back(exported);
		Check(ClickMenuItem(menu, "Export…"), "a click on the Game configs menu's Export…");
		GtkWindow* picker =
		    WaitForWindow(KYTY_TYPE_GAME_PICKER_DIALOG, "Export… to open the game picker");
		if (picker != nullptr) {
			GtkWidget* row = KytyUi::FindFirst(GTK_WIDGET(picker), [](GtkWidget* widget) {
				return GTK_IS_LIST_BOX_ROW(widget) &&
				       KytyUi::AllText(widget).find("PPSA01002") != std::string::npos;
			});
			if (Check(row != nullptr, "the picker lists Beta Quest")) {
				gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(row)),
				                        GTK_LIST_BOX_ROW(row));
				Check(KytyUi::Activate(
				          KytyUi::FindByBuildableId(GTK_WIDGET(picker), "accept_button")),
				      "a click on Export");
			}
		}
		KytyUi::WaitFor([&]() { return Exists(exported); }, 3000, "the export to be written");
		Check(!chooser.calls.empty() &&
		          chooser.calls.back() == "SaveFile Export game config | PPSA01002.json",
		      "the chooser suggests PPSA01002.json");
		const auto read = core::ReadGameSettingsFile(exported);
		Check(read && read->by_title.size() == 1 && read->by_title.contains("PPSA01002"),
		      "the file holds Beta Quest's config");
		// Import (the menu's item): the file, the confirmation, the games' own settings.
		chooser.file_answers.push_back(import);
		Check(ClickMenuItem(menu, "Import…"), "a click on the Game configs menu's Import…");
		if (GtkWindow* alert = KytyUi::FindAlert("Import game configs?"); alert != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
			Check(text.find("PPSA01001") != std::string::npos &&
			          text.find("PPSA01002") != std::string::npos &&
			          text.find("Games not found: PPSA09999") != std::string::npos,
			      "the confirmation lists the matching games and the missing one");
			KytyUi::CloseAlert(alert, "Import");
		}
		KytyUi::WaitFor(
		    [&]() {
			    return StoredUserName(ui, FixtureEntry(games, kAlpha)) == "Imported" &&
			           StoredUserName(ui, FixtureEntry(games, kBeta)) == "Imported";
		    },
		    3000, "the import to give both games the file's user name");
		if (KytyUi::IsToplevel(settings)) {
			CloseWithEscape(settings, "the global settings");
		}
	});
}

// LFI-18: Reset restores the default settings in the editor (the folders stay); Save writes them.
KYTY_UI_CASE(lfi_18) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty() || !Store(ui, [](core::LauncherSettings& settings) {
		    settings.global.user_name               = "Custom Name";
		    settings.global.fullscreen_enabled      = true;
		    settings.controller.vibration_intensity = 40;
	    })) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* /*window*/) {
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root       = GTK_WIDGET(settings);
		GtkWidget* name       = RowTitled(root, "User name");
		GtkWidget* fullscreen = RowTitled(root, "Run in fullscreen");
		GtkRange*  vibration  = ScaleNamed(root, "Vibration intensity");
		if (!Check(name != nullptr && fullscreen != nullptr && vibration != nullptr,
		           "the editor has the rows")) {
			return;
		}
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(name))) == "Custom Name" &&
		          adw_switch_row_get_active(ADW_SWITCH_ROW(fullscreen)) != FALSE &&
		          gtk_range_get_value(vibration) == 40,
		      "the editor shows the stored values");
		GtkWidget* reset = KytyUi::FindByBuildableId(root, "reset_button");
		Check(reset != nullptr && gtk_widget_get_tooltip_text(reset) != nullptr,
		      "the footer has Reset with a tooltip");
		Check(KytyUi::Activate(reset), "a click on Reset");
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(name))) == "Kyty" &&
		          adw_switch_row_get_active(ADW_SWITCH_ROW(fullscreen)) == FALSE &&
		          gtk_range_get_value(vibration) == 100,
		      "Reset shows the defaults");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		KytyUi::WaitFor([settings]() { return !KytyUi::IsToplevel(settings); }, 3000,
		                "Save to close the editor");
	});
	const auto stored = Stored(ui);
	Check(stored.global.user_name == "Kyty" && !stored.global.fullscreen_enabled &&
	          stored.controller.vibration_intensity == 100,
	      "Kyty.ini has the defaults after Reset and Save");
	Check(stored.game_folders.size() == 1 && stored.game_folders[0] == games.string(),
	      "Reset keeps the game folders");
}

// LFI-19: the editor's pages General, Network, Debug with their fields, dependent fields enabled
// only with their direction File, and the validation of a mandatory field.
KYTY_UI_CASE(lfi_19) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* /*window*/) {
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root  = GTK_WIDGET(settings);
		auto*      stack = ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"));
		// The pages through the view switcher (its buttons are the pages' tabs; Tab reaches them).
		// design.md's Ctrl+PageUp/PageDown for the pages are not bound (a recorded deviation).
		for (const auto& [title, page]: {std::pair<const char*, const char*> {"Debug", "debug"},
		                                 {"Network", "network"},
		                                 {"General", "general"}}) {
			GtkWidget* tab = KytyUi::FindFirst(root, [title = title](GtkWidget* widget) {
				return GTK_IS_TOGGLE_BUTTON(widget) &&
				       gtk_widget_get_ancestor(widget, ADW_TYPE_VIEW_SWITCHER) != nullptr &&
				       KytyUi::ControlName(widget) == title;
			});
			Check(tab != nullptr && KytyUi::Activate(tab), std::string("the tab ") + title);
			Check(stack != nullptr &&
			          g_strcmp0(adw_view_stack_get_visible_child_name(stack), page) == 0,
			      std::string("the tab ") + title + " shows its page");
		}
		for (const char* field: {"User name", "Console language", "Screen resolution", "GPU",
		                         "Enable network connection", "Vulkan validation", "Shader logging",
		                         "Printf output"}) {
			Check(RowTitled(root, field) != nullptr, std::string("the editor has ") + field);
		}
		// A dependent field: the shader log folder only while shader logs go to a file.
		GtkWidget* direction = RowTitled(root, "Shader logging");
		GtkWidget* folder    = RowTitled(root, "Shader log folder");
		if (Check(direction != nullptr && folder != nullptr, "the shader logging rows")) {
			adw_combo_row_set_selected(ADW_COMBO_ROW(direction), 0);
			Check(gtk_widget_get_sensitive(folder) == FALSE,
			      "the shader log folder is off while the logs are Silent");
			adw_combo_row_set_selected(ADW_COMBO_ROW(direction), 2);
			Check(gtk_widget_get_sensitive(folder) != FALSE,
			      "the shader log folder is on when the logs go to a File");
		}
		// A mandatory field: an empty user name is refused and the editor stays open.
		GtkWidget* name = RowTitled(root, "User name");
		gtk_editable_set_text(GTK_EDITABLE(name), "");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		if (GtkWindow* alert = KytyUi::FindAlert("The settings could not be saved");
		    alert != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(alert)).find("User name (user_name)") !=
			          std::string::npos,
			      "the error names the field");
			CloseError(alert);
		}
		Check(KytyUi::IsToplevel(settings) && gtk_widget_has_css_class(name, "error") != FALSE,
		      "the editor stays open and marks the field");
		Check(g_strcmp0(adw_view_stack_get_visible_child_name(stack), "general") == 0,
		      "the editor shows the field's page");
		// Cancel writes nothing.
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "cancel_button")),
		      "a click on Cancel");
		KytyUi::WaitFor([settings]() { return !KytyUi::IsToplevel(settings); }, 3000,
		                "Cancel to close the editor");
	});
	Check(Stored(ui).global == core::GameConfiguration {},
	      "nothing of the refused edit was stored (the global settings are the defaults)");
}

// LFI-20: the Folders page: add several game folders at once, remove the selected one, and the
// add-ons folder (entry and chooser).
KYTY_UI_CASE(lfi_20) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path  first  = ui.Root() / "more-games-1";
	const fs::path  second = ui.Root() / "more-games-2";
	const fs::path  addons = ui.Root() / "addons";
	std::error_code ec;
	for (const auto& directory: {first, second, addons}) {
		fs::create_directories(directory, ec);
	}
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto& chooser = ui.Services().file_chooser;
		g_action_group_activate_action(G_ACTION_GROUP(application), "add-game-folder", nullptr);
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(settings);
		Check(g_strcmp0(adw_view_stack_get_visible_child_name(
		                    ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"))),
		                "folders") == 0,
		      "app.add-game-folder opens the Folders page");
		GtkWidget* list   = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_LIST_BOX(widget) &&
			       gtk_list_box_get_selection_mode(GTK_LIST_BOX(widget)) == GTK_SELECTION_MULTIPLE;
		});
		const auto row_of = [list](const fs::path& folder) {
			return KytyUi::FindFirst(list, [&folder](GtkWidget* widget) {
				return ADW_IS_ACTION_ROW(widget) && KytyUi::WidgetText(widget) == folder.string();
			});
		};
		if (!Check(list != nullptr && row_of(games) != nullptr,
		           "the folder list shows the stored folder")) {
			return;
		}
		chooser.folder_answers.push_back({first, second});
		Check(KytyUi::Activate(ButtonLabelled(root, {"Add folders…"})), "a click on Add folders…");
		KytyUi::WaitFor([&]() { return row_of(first) != nullptr && row_of(second) != nullptr; },
		                3000, "both chosen folders to be listed");
		Check(!chooser.calls.empty() && chooser.calls.back() == "SelectFolders Add game folders",
		      "the chooser asks for several folders");
		GtkWidget* remove = ButtonLabelled(root, {"Remove"});
		Check(remove != nullptr && gtk_widget_get_sensitive(remove) == FALSE,
		      "Remove is off without a selection");
		gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row_of(first)));
		Check(KytyUi::Activate(remove), "a click on Remove");
		Check(row_of(first) == nullptr && row_of(second) != nullptr && row_of(games) != nullptr,
		      "Remove removes the selected folder only");
		chooser.folder_answers.push_back({addons});
		Check(KytyUi::Activate(KytyUi::FindByAccessibleLabel(root, "Select add-ons folder")),
		      "a click on the add-ons folder's chooser button");
		GtkWidget* entry = RowTitled(root, "Add-ons folder");
		KytyUi::WaitFor(
		    [&]() {
			    return entry != nullptr &&
			           addons.string() == gtk_editable_get_text(GTK_EDITABLE(entry));
		    },
		    3000, "the chosen add-ons folder in its entry");
		auto&      controller = kyty::gtk::MainWindowLibrary(window);
		const auto scans      = controller.Stats().scans_applied;
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > scans; }, 8000,
		                "the changed folders to be scanned");
	});
	const auto stored = Stored(ui);
	Check(stored.game_folders == std::vector<std::string> {games.string(), second.string()},
	      "Kyty.ini has the stored and the second folder");
	Check(stored.addons_dir == addons.string(), "Kyty.ini has the add-ons folder");
}

// LFI-21: the compatibility Status and Comment from the database; with --local the list edits
// them and saves ./compatibility_db.json.
KYTY_UI_CASE(lfi_21) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&     context = ContextOf(application);
		KytyGame* alpha   = GameByTitle(context, kAlpha);
		Check(kyty::gtk::MainWindowLibrary(window).Stats().compatibility_source == "download",
		      "the database was downloaded");
		Check(kyty_game_get_status(alpha) == core::CompatibilityStatus::InGame &&
		          kyty_game_get_comment(alpha) == "UI test fixture",
		      "Alpha Game has the database's status and comment");
		Check(kyty_game_get_status(GameByTitle(context, kBeta)) ==
		          core::CompatibilityStatus::Unknown,
		      "a game the database does not list is Unknown");
		Check(KytyUi::FindFirst(GTK_WIDGET(ListView(window)),
		                        [](GtkWidget* widget) {
			                        return GTK_IS_DROP_DOWN(widget) || GTK_IS_ENTRY(widget);
		                        }) == nullptr,
		      "without --local the columns cannot be edited");
	});
	// --local: the working directory's compatibility_db.json.
	const fs::path  previous = fs::current_path();
	std::error_code ec;
	fs::current_path(ui.WorkingDirectory(), ec);
	const fs::path local = ui.WorkingDirectory() / core::kLocalCompatibilityFileName;
	{
		OwnSession session(ui, kyty::updater::UpdateUiMode::Other, true);
		if (session.Ok() && WaitForLibrary(session.Window())) {
			KytyMainWindow* window = session.Window();
			Check(kyty::gtk::MainWindowLibrary(window).Stats().compatibility_source == "local",
			      "--local uses the local file");
			KytyUi::Pump(200);
			GtkWidget* status = KytyUi::FindByAccessibleLabel(GTK_WIDGET(ListView(window)),
			                                                  "Compatibility status of Alpha Game");
			GtkWidget* comment = KytyUi::FindByAccessibleLabel(
			    GTK_WIDGET(ListView(window)), "Compatibility comment for Alpha Game");
			if (Check(status != nullptr && GTK_IS_DROP_DOWN(status) && comment != nullptr,
			          "--local shows a status drop-down and a comment entry")) {
				gtk_drop_down_set_selected(GTK_DROP_DOWN(status), 3); // Logo, in the Qt order
				gtk_editable_set_text(GTK_EDITABLE(comment), "Boots to the logo");
				g_signal_emit_by_name(comment, "activate");
				KytyUi::WaitFor(
				    [&]() {
					    const std::string text = ReadText(local);
					    return text.find("PPSA01001") != std::string::npos &&
					           text.find("Boots to the logo") != std::string::npos;
				    },
				    3000, "the edits to be saved in compatibility_db.json");
				const auto loaded = core::LoadLocalCompatibility(local);
				Check(static_cast<bool>(loaded), "the saved file loads again");
				KytyGame* alpha = GameByTitle(session.Context(), kAlpha);
				Check(kyty_game_get_status(alpha) == core::CompatibilityStatus::Logo,
				      "Alpha Game's status is Logo");
			}
		}
	}
	fs::current_path(previous, ec);
}

// LFI-22: the global settings' controller group: the lightbar color with its live preview,
// vibration, speaker volume, and keyboard player.
KYTY_UI_CASE(lfi_22) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* /*window*/) {
		auto& controller = ui.Services().controller;
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		auto* settings =
		    KYTY_SETTINGS_WINDOW(WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings"));
		if (settings == nullptr) {
			return;
		}
		kyty_settings_window_show_page(settings, "controller");
		GtkWidget* root   = GTK_WIDGET(settings);
		GtkWidget* color  = KytyUi::FindByType(root, GTK_TYPE_COLOR_DIALOG_BUTTON);
		GtkRange*  shake  = ScaleNamed(root, "Vibration intensity");
		GtkRange*  volume = ScaleNamed(root, "Speaker volume");
		GtkWidget* player = RowTitled(root, "Keyboard player");
		if (!Check(color != nullptr && shake != nullptr && volume != nullptr && player != nullptr,
		           "the Controller page has its four controls")) {
			return;
		}
		GdkRGBA red {};
		gdk_rgba_parse(&red, "#ff0000");
		gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color), &red);
		KytyUi::WaitFor(
		    [&controller]() {
			    return controller.Lightbar() == kyty::gtk::LightbarColor {255, 0, 0};
		    },
		    2000, "the chosen color to light the controller at once (the live preview)");
		gtk_range_set_value(shake, 70);
		gtk_range_set_value(volume, 30);
		adw_combo_row_set_selected(ADW_COMBO_ROW(player), 2);
		// Compared by address once it is gone (a cast of a destroyed window is a critical).
		GtkWindow* closing = GTK_WINDOW(settings);
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		KytyUi::WaitFor([closing]() { return !KytyUi::IsToplevel(closing); }, 3000,
		                "Save to close the editor");
	});
	const auto stored = Stored(ui).controller;
	Check(g_ascii_strcasecmp(stored.color.c_str(), "#ff0000") == 0 &&
	          stored.vibration_intensity == 70 && stored.speaker_volume == 30 &&
	          stored.keyboard_player == 2,
	      "Kyty.ini has the color #ff0000, vibration 70, speaker 30, keyboard player 2 (" +
	          stored.color + ", " + std::to_string(stored.vibration_intensity) + ", " +
	          std::to_string(stored.speaker_volume) + ", " +
	          std::to_string(stored.keyboard_player) + ")");
}

// LFI-23: the stored lightbar color lights the connected controllers while the launcher is open
// and no game runs.
KYTY_UI_CASE(lfi_23) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty() || !Store(ui, [](core::LauncherSettings& settings) {
		    settings.controller.color = "#00ff00";
	    })) {
		return;
	}
	const FakeEmulatorArgs args("--stdout ready --wait");
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		auto&                          controller = ui.Services().controller;
		const kyty::gtk::LightbarColor green {0, 255, 0};
		KytyUi::WaitFor([&]() { return controller.Lightbar() == green; }, 3000,
		                "the stored color on the controller at startup");
		Check(controller.Polling(), "the service keeps the color up (polling)");
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "run");
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "the game to run");
		Check(controller.pause_calls >= 1 && !controller.Polling(),
		      "the launcher leaves the controller to the game while it runs");
		ActivateGameAction(window, "stop");
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
		KytyUi::WaitFor([&]() { return controller.Polling(); }, 2000,
		                "the preview to resume after the game");
		Check(controller.Lightbar() == green, "the color is the stored one again");
	});
}

// LFI-24: the input mapping editor: both tabs, Delete unmaps the focused binding, Enter captures,
// Restore tab defaults, the touch pad options, and Save.
KYTY_UI_CASE(lfi_24) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	std::vector<std::string> saved;
	RunWithLibrary(ui, [&saved](KytyApplication* /*application*/, KytyMainWindow* window) {
		Check(KytyUi::Activate(ButtonWithAction(GTK_WIDGET(window), "app.input-mapping")),
		      "a click on the header bar's Input mapping");
		auto* editor = KYTY_INPUT_MAPPING_WINDOW(
		    WaitForWindow(KYTY_TYPE_INPUT_MAPPING_WINDOW, "the input mapping editor"));
		if (editor == nullptr) {
			return;
		}
		GtkWidget* root  = GTK_WIDGET(editor);
		auto*      stack = ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"));
		Check(stack != nullptr && adw_view_stack_get_child_by_name(stack, "gamepad") != nullptr &&
		          adw_view_stack_get_child_by_name(stack, "keyboard") != nullptr,
		      "the editor has the Gamepad and the Keyboard and mouse tabs");
		for (const char* id: {"sensitivity_row", "skin_dropdown", "cursor_check", "defaults_button",
		                      "save_button"}) {
			Check(KytyUi::FindByBuildableId(root, id) != nullptr,
			      std::string("the editor has ") + id);
		}
		kyty_input_mapping_window_show_tab(editor, "keyboard");
		KytyUi::WaitForPaintedFrame(root);
		const auto bound = [root]() {
			return KytyUi::FindAll(root, [](GtkWidget* widget) {
				return GTK_IS_BUTTON(widget) &&
				       gtk_widget_has_css_class(widget, "binding") != FALSE &&
				       gtk_widget_get_mapped(widget) != FALSE;
			});
		};
		auto buttons = bound();
		if (!Check(buttons.size() >= 2 && KytyUi::WidgetText(buttons[0]) != "None",
		           "the keyboard tab shows bound controls")) {
			return;
		}
		GtkWidget*        binding = buttons[0];
		const std::string key     = KytyUi::WidgetText(binding);
		gtk_widget_grab_focus(binding);
		Check(KytyUi::KeyPress(root, GDK_KEY_Delete), "Delete is handled");
		Check(KytyUi::WidgetText(binding) == "None", "Delete unmaps \"" + key + "\"");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "defaults_button")),
		      "a click on Restore tab defaults");
		Check(KytyUi::WidgetText(bound()[0]) == key, "Restore tab defaults maps it again");
		binding = bound()[0];
		gtk_widget_grab_focus(binding);
		KytyUi::KeyPress(root, GDK_KEY_Delete);
		// Enter on a binding opens the capture; Escape there changes nothing.
		gtk_widget_grab_focus(bound()[1]);
		Check(KytyUi::KeyPress(root, GDK_KEY_Return), "Enter on a binding is handled");
		if (GtkWindow* capture = WaitForWindow(KYTY_TYPE_BINDING_CAPTURE_DIALOG,
		                                       "Enter to open the binding capture");
		    capture != nullptr) {
			KytyUi::KeyPress(GTK_WIDGET(capture), GDK_KEY_Escape);
			KytyUi::WaitFor([capture]() { return !KytyUi::IsToplevel(capture); }, 3000,
			                "Escape to close the capture");
		}
		saved = core::SerializeMapping(kyty_input_mapping_window_get_model(editor));

		GtkWindow* closing = GTK_WINDOW(editor);
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "save_button")), "a click on Save");
		KytyUi::WaitFor([closing]() { return !KytyUi::IsToplevel(closing); }, 3000,
		                "Save to close the editor");
	});
	Check(!saved.empty() && Stored(ui).global.host_input_mapping == saved,
	      "Kyty.ini has the edited global mapping");
}

// LFI-25: the trophy window (game menu, View trophies…): earned, locked, and hidden trophies, the
// hidden trophy's Reveal, and the details dialog of a trophy (Enter on its row).
KYTY_UI_CASE(lfi_25) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.WriteGameContent(games) ||
	    !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "trophies");
		auto* trophies = KYTY_TROPHY_WINDOW(WaitForWindow(KYTY_TYPE_TROPHY_WINDOW, "the trophies"));
		if (trophies == nullptr ||
		    !KytyUi::WaitFor([trophies]() { return kyty_trophy_window_is_loaded(trophies); }, 5000,
		                     "the trophies to be read")) {
			return;
		}
		GtkWidget* root     = GTK_WIDGET(trophies);
		const auto row_name = [root](std::string_view prefix) {
			return KytyUi::FindFirst(root, [prefix](GtkWidget* widget) {
				return GTK_IS_LIST_BOX_ROW(widget) && gtk_widget_get_mapped(widget) != FALSE &&
				       KytyUi::ControlName(widget).starts_with(prefix);
			});
		};
		KytyUi::Pump(200);
		Check(row_name("Gold Rush, ") != nullptr &&
		          KytyUi::ControlName(row_name("Gold Rush, ")).ends_with(", earned"),
		      "the earned trophy is listed as earned");
		Check(row_name("Silver Lining, ") != nullptr &&
		          KytyUi::ControlName(row_name("Silver Lining, ")).ends_with(", not earned"),
		      "the locked trophy is listed as not earned");
		Check(row_name("Hidden trophy, ") != nullptr && row_name("Secret Ending") == nullptr,
		      "the hidden trophy is masked");
		Check(KytyUi::AllText(root).find("Alpha Game Trophies") != std::string::npos,
		      "the window shows the package title");
		GtkWidget* reveal = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_BUTTON(widget) && gtk_widget_get_mapped(widget) != FALSE &&
			       gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW) != nullptr;
		});
		Check(KytyUi::Activate(reveal), "a click on Reveal");
		KytyUi::WaitFor([&]() { return row_name("Secret Ending") != nullptr; }, 2000,
		                "Reveal to show the hidden trophy's name");
		// The details of the earned trophy: Enter on its row (the row's activation).
		GtkWidget* gold = row_name("Gold Rush, ");
		if (gold != nullptr) {
			gtk_widget_grab_focus(gold);
			KytyUi::KeyPress(GTK_WIDGET(trophies), GDK_KEY_Return);
		}
		if (GtkWindow* details =
		        WaitForWindow(KYTY_TYPE_TROPHY_DETAILS_DIALOG, "Enter to open the trophy details");
		    details != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(details));
			Check(text.find("Gold Rush") != std::string::npos &&
			          text.find("Collect 1,000 coins.") != std::string::npos &&
			          text.find("Earned") != std::string::npos,
			      "the details show the name, the details, and that it is earned");
			CloseWithEscape(details, "the trophy details");
		}
		CloseWithEscape(GTK_WINDOW(trophies), "the trophies");
	});
}

// LFI-26: the trophy overview (header bar, Trophy overview) lists every game with trophies:
// name, title ID, earned and total, percentage.
KYTY_UI_CASE(lfi_26) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.WriteGameContent(games) ||
	    !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	RunWithLibrary(ui, [](KytyApplication* /*application*/, KytyMainWindow* window) {
		Check(KytyUi::Activate(ButtonWithAction(GTK_WIDGET(window), "app.trophy-overview")),
		      "a click on Trophy overview");
		auto* overview = KYTY_TROPHY_OVERVIEW_WINDOW(
		    WaitForWindow(KYTY_TYPE_TROPHY_OVERVIEW_WINDOW, "the trophy overview"));
		if (overview == nullptr ||
		    !KytyUi::WaitFor(
		        [overview]() { return kyty_trophy_overview_window_is_loaded(overview); }, 5000,
		        "the overview to be read")) {
			return;
		}
		KytyUi::Pump(200);
		GtkWidget* root = GTK_WIDGET(overview);
		const auto rows = KytyUi::FindAll(root, [](GtkWidget* widget) {
			return GTK_IS_LIST_BOX_ROW(widget) && gtk_widget_get_mapped(widget) != FALSE;
		});
		Check(rows.size() == 1 && KytyUi::ControlName(rows[0]).starts_with("Alpha Game, "),
		      "one row: Alpha Game, the only game with trophies");
		const std::string text = rows.empty() ? "" : KytyUi::AllText(rows[0]);
		Check(text.find("PPSA01001") != std::string::npos &&
		          text.find("Earned 1/3") != std::string::npos &&
		          text.find('%') != std::string::npos,
		      "the row has the title ID, Earned 1/3, and the percentage: " + text);
		CloseWithEscape(GTK_WINDOW(overview), "the trophy overview");
	});
}

// LFI-27: the cheats window: the remote file list, and the local cheats with check boxes, Apply
// selection, Enable all, Disable all.
KYTY_UI_CASE(lfi_27) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.WriteGameContent(games) ||
	    !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const fs::path data = ui.AppDirectory();
	RunWithLibrary(ui, [&data](KytyApplication* application, KytyMainWindow* window) {
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "cheats");
		GtkWindow* cheats = WaitForWindow(KYTY_TYPE_CHEATS_WINDOW, "the cheats window");
		if (cheats == nullptr) {
			return;
		}
		GtkWidget* root  = GTK_WIDGET(cheats);
		auto*      stack = ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"));
		adw_view_stack_set_visible_child_name(stack, "remote");
		KytyUi::WaitFor(
		    [root]() {
			    return KytyUi::FindFirst(root, [](GtkWidget* widget) {
				           return GTK_IS_LIST_BOX_ROW(widget) &&
				                  KytyUi::ControlName(widget).ends_with(
				                      ", PPSA01001_01.000_alpha.json");
			           }) != nullptr;
		    },
		    5000, "the remote list to show Alpha Game's files");
		adw_view_stack_set_visible_child_name(stack, "local");
		KytyUi::WaitForPaintedFrame(root);
		const auto check = [root](std::string_view name) -> GtkCheckButton* {
			GtkWidget* found = KytyUi::FindFirst(root, [name](GtkWidget* widget) {
				return GTK_IS_CHECK_BUTTON(widget) && KytyUi::ControlName(widget) == name;
			});
			return found != nullptr ? GTK_CHECK_BUTTON(found) : nullptr;
		};
		GtkCheckButton* health = check("Infinite Health");
		GtkCheckButton* coins  = check("Max Coins");
		if (!Check(health != nullptr && coins != nullptr, "the local cheats have check boxes")) {
			return;
		}
		Check(gtk_check_button_get_active(health) != FALSE &&
		          gtk_check_button_get_active(coins) == FALSE,
		      "the check boxes show the file's states");
		const auto stored = [&data]() {
			std::vector<std::pair<std::string, bool>> states;
			const auto local = core::LoadLocalCheats(data, "PPSA01001");
			if (local.document) {
				for (const auto& cheat: local.document->cheats) {
					states.emplace_back(cheat.name, cheat.enabled);
				}
			}
			return states;
		};
		// Space toggles the focused check box.
		gtk_widget_grab_focus(GTK_WIDGET(coins));
		KytyUi::KeyPress(GTK_WIDGET(coins), GDK_KEY_space);
		KytyUi::WaitFor([coins]() { return gtk_check_button_get_active(coins) != FALSE; }, 2000,
		                "Space to check Max Coins");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "apply_button")),
		      "a click on Apply selection");
		using States = std::vector<std::pair<std::string, bool>>;
		KytyUi::WaitFor(
		    [&]() { return stored() == States {{"Infinite Health", true}, {"Max Coins", true}}; },
		    3000, "Apply selection to save both cheats on");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "disable_all_button")),
		      "a click on Disable all");
		Check(gtk_check_button_get_active(health) == FALSE &&
		          gtk_check_button_get_active(coins) == FALSE,
		      "Disable all clears the check boxes");
		Check(stored() == States {{"Infinite Health", true}, {"Max Coins", true}},
		      "Disable all writes nothing by itself");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "enable_all_button")),
		      "a click on Enable all");
		Check(gtk_check_button_get_active(health) != FALSE &&
		          gtk_check_button_get_active(coins) != FALSE,
		      "Enable all checks every box");
		CloseWithEscape(cheats, "the cheats window");
	});
}

// LFI-28: the additional content window: the add-on with its title, the status line, Refresh, and
// Open folder.
KYTY_UI_CASE(lfi_28) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.WriteGameContent(games) ||
	    !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "additional-content");
		auto* addons = KYTY_ADDITIONAL_CONTENT_WINDOW(
		    WaitForWindow(KYTY_TYPE_ADDITIONAL_CONTENT_WINDOW, "the additional content window"));
		if (addons == nullptr) {
			return;
		}
		GtkWidget* root   = GTK_WIDGET(addons);
		const auto listed = [root]() {
			return KytyUi::FindFirst(root, [](GtkWidget* widget) {
				       return ADW_IS_ACTION_ROW(widget) &&
				              KytyUi::ControlName(widget).starts_with("Alpha Soundtrack");
			       }) != nullptr;
		};
		KytyUi::WaitFor(
		    [&]() { return kyty_additional_content_window_is_loaded(addons) && listed(); }, 5000,
		    "the add-on to be listed");
		Check(KytyUi::AllText(root).find("1 installed add-on") != std::string::npos,
		      "the status line counts the add-on");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "refresh_button")),
		      "a click on Refresh");
		KytyUi::WaitFor(
		    [&]() { return kyty_additional_content_window_is_loaded(addons) && listed(); }, 5000,
		    "Refresh to list the add-on again");
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(root, "open_button")),
		      "a click on Open folder");
		const auto& calls = ui.Services().desktop_launcher.calls;
		KytyUi::WaitFor([&calls]() { return !calls.empty(); }, 3000, "Open folder to open it");
		Check(!calls.empty() && calls.back() == "OpenFolder " + (ui.Root() / "addons").string(),
		      "Open folder opens the add-ons folder");
		CloseWithEscape(GTK_WINDOW(addons), "the additional content window");
	});
}

// LFI-29: Compress to ZAR: the progress, the offer to delete the original folder, the rescan; the
// warning when the archive exists.
KYTY_UI_CASE(lfi_29) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	RunWithLibrary(ui, [&games](KytyApplication* application, KytyMainWindow* window) {
		auto&          context    = ContextOf(application);
		auto&          controller = kyty::gtk::MainWindowLibrary(window);
		const fs::path folder     = games / "Beta" / "Beta Quest";
		const fs::path archive    = games / "Beta" / "Beta Quest.zar";
		SelectTitle(context, kBeta);
		const auto scans = controller.Stats().scans_applied;
		ActivateGameAction(window, "compress");
		auto* dialog = KYTY_COMPRESSION_DIALOG(
		    WaitForWindow(KYTY_TYPE_COMPRESSION_DIALOG, "the compression dialog"));
		if (dialog == nullptr) {
			return;
		}
		Check(KytyUi::FindByType(GTK_WIDGET(dialog), GTK_TYPE_PROGRESS_BAR) != nullptr,
		      "the dialog has a progress bar");
		if (GtkWindow* question = KytyUi::FindAlert("Delete the original folder?", 20000);
		    question != nullptr) {
			Check(kyty_compression_dialog_get_outcome(dialog) == core::ArchiveOutcome::Completed,
			      "the job completed");
			Check(Exists(archive), "the archive was created");
			KytyUi::CloseAlert(question, "Delete folder");
		}
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > scans; }, 8000,
		                "the rescan after the compression");
		Check(!Exists(folder), "Delete folder removed the original folder");
		KytyGame* beta = GameByTitle(context, kBeta);
		Check(beta != nullptr && kyty_game_get_entry(beta).kind == core::GameEntry::Kind::Archive &&
		          kyty_game_get_entry(beta).path == archive,
		      "the library lists Beta Quest as its archive");
		// An archive that exists already is not replaced.
		WriteText(games / "Alpha Game.zar", "an existing archive");
		SelectTitle(context, kAlpha);
		ActivateGameAction(window, "compress");
		GtkWindow* again = WaitForWindow(KYTY_TYPE_COMPRESSION_DIALOG, "the second dialog");
		if (again != nullptr) {
			KytyUi::WaitFor(
			    [again]() {
				    return KytyUi::AllText(GTK_WIDGET(again)).find("already exists.") !=
				           std::string::npos;
			    },
			    3000, "the dialog to say the archive already exists");
			Check(ReadText(games / "Alpha Game.zar") == "an existing archive",
			      "the existing archive is untouched");
			CloseWithEscape(again, "the compression dialog");
		}
	});
}

// LFI-30: Remove save data: the confirmation lists the directories; what could not be deleted is
// listed with its reason in the scrollable failure list (W3).
KYTY_UI_CASE(lfi_30) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path saves = ui.AppDirectory() / "_SaveData" / "PPSA01001";
	for (int i = 1; i <= 30; i++) {
		WriteText(saves / "1" / ("savedata" + std::to_string(i) + ".bin"), "save");
	}
	WriteText(saves / "1" / "sce_sys" / "param.sfo", "param");
	const fs::path beta_saves = ui.AppDirectory() / "_SaveData" / "PPSA01002";
	WriteText(beta_saves / "1" / "data.dat", "save");
	FailingSaveFiles remover;
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto& context        = ContextOf(application);
		context.path_remover = &remover;
		SelectTitle(context, kAlpha);
		ActivateGameAction(window, "remove-save-data");
		if (GtkWindow* confirm = KytyUi::FindAlert("Remove the save data of Alpha Game?");
		    confirm != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(confirm)).find(saves.string()) != std::string::npos,
			      "the confirmation lists the save data directory");
			KytyUi::CloseAlert(confirm, "Remove save data");
		}
		GtkWindow* failures =
		    KytyUi::FindAlert("Some save data of Alpha Game could not be removed");
		if (failures != nullptr) {
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(failures));
			const auto rows = KytyUi::FindAll(GTK_WIDGET(failures), [](GtkWidget* widget) {
				return GTK_IS_LIST_BOX_ROW(widget);
			});
			Check(rows.size() == 32, "30 files and the two directories above them are listed (" +
			                             std::to_string(rows.size()) + ")");
			const std::string text = KytyUi::AllText(GTK_WIDGET(failures));
			Check(text.find((saves / "1" / "savedata7.bin").string() + ": Permission denied") !=
			          std::string::npos,
			      "each failure names its path and its reason");
			GtkWidget* scrolled =
			    KytyUi::FindByType(GTK_WIDGET(failures), GTK_TYPE_SCROLLED_WINDOW);
			if (Check(scrolled != nullptr, "the list scrolls")) {
				GtkAdjustment* adjustment =
				    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
				Check(gtk_adjustment_get_upper(adjustment) >
				          gtk_adjustment_get_page_size(adjustment),
				      "the list is longer than its view");
			}
			Check(gtk_widget_get_height(GTK_WIDGET(failures)) <= 600,
			      "the failure list fits the screen");
			Check(KytyUi::Capture(failures, "save-data-failures"), "the failure list renders");
			KytyUi::CloseAlert(failures, "_Close");
		}
		Check(Exists(saves / "1" / "savedata1.bin") && !Exists(saves / "1" / "sce_sys"),
		      "what could be removed is gone, the rest stays");
		// Without failures nothing more is shown.
		context.path_remover = &core::HostPathRemover();
		const guint before   = KytyUi::ToplevelCount();
		SelectTitle(context, kBeta);
		ActivateGameAction(window, "remove-save-data");
		if (GtkWindow* confirm = KytyUi::FindAlert("Remove the save data of Beta Quest?");
		    confirm != nullptr) {
			KytyUi::CloseAlert(confirm, "Remove save data");
		}
		KytyUi::WaitFor([&]() { return !Exists(beta_saves); }, 3000,
		                "Beta Quest's save data to be removed");
		KytyUi::Pump(200);
		Check(KytyUi::ToplevelCount() == before, "a complete removal shows nothing more");
	});
}

// LFI-31: Open game folder (Ctrl+O): a directory game's folder; for a .zar game its folder with
// the archive selected.
KYTY_UI_CASE(lfi_31) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	RunWithLibrary(ui, [&](KytyApplication* /*application*/, KytyMainWindow* window) {
		const auto& calls = ui.Services().desktop_launcher.calls;
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_o, GDK_CONTROL_MASK),
		      "Ctrl+O is handled");
		KytyUi::WaitFor([&calls]() { return calls.size() == 1; }, 3000,
		                "Ctrl+O to open the folder");
		Check(!calls.empty() && calls.back() == "OpenFolder " + (games / "Alpha Game").string(),
		      "Ctrl+O opens the directory game's folder");
		FocusGameRow(window, "Gamma Archive");
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_o, GDK_CONTROL_MASK), "Ctrl+O again");
		KytyUi::WaitFor([&calls]() { return calls.size() == 2; }, 3000,
		                "Ctrl+O to show the archive");
		Check(calls.size() == 2 &&
		          calls.back() == "RevealFile " + (games / "Gamma Archive.zar").string(),
		      "Ctrl+O shows the .zar file in its folder");
	});
}

// LFI-32: the footer and the global settings' About this installation: the Settings_Store path,
// the Emulator path, the Emulator version.
KYTY_UI_CASE(lfi_32) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget*        root     = GTK_WIDGET(window);
		const std::string emulator = (ui.AppDirectory() / "kyty_emulator").string();
		GtkWidget*        version  = KytyUi::FindByBuildableId(root, "version_label");
		Check(KytyUi::WidgetText(KytyUi::FindByBuildableId(root, "settings_label")) ==
		          "Settings file: " + ui.SettingsFile().string(),
		      "the footer names the Settings_Store");
		Check(KytyUi::WidgetText(KytyUi::FindByBuildableId(root, "emulator_label")) ==
		          "Emulator: " + emulator,
		      "the footer names the Emulator");
		KytyUi::WaitFor([version]() { return gtk_widget_get_visible(version) != FALSE; }, 6000,
		                "the Emulator version in the footer");
		Check(KytyUi::WidgetText(version) == "Version: KytyPS5 fake emulator (UI tests)",
		      "the footer shows the version the Emulator printed (" + KytyUi::WidgetText(version) +
		          ")");
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		auto* settings =
		    KYTY_SETTINGS_WINDOW(WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings"));
		if (settings == nullptr) {
			return;
		}
		kyty_settings_window_show_page(settings, "about");
		const auto subtitle = [settings](const char* title) -> std::string {
			GtkWidget*  row = RowTitled(GTK_WIDGET(settings), title);
			const char* text =
			    row != nullptr ? adw_action_row_get_subtitle(ADW_ACTION_ROW(row)) : nullptr;
			return text != nullptr ? text : "";
		};
		Check(subtitle("Settings file") == ui.SettingsFile().string() &&
		          subtitle("Data directory") == ui.AppDirectory().string() &&
		          subtitle("Emulator") == emulator &&
		          subtitle("Emulator version") == "KytyPS5 fake emulator (UI tests)",
		      "About this installation names the files and the version");
		CloseWithEscape(GTK_WINDOW(settings), "the global settings");
	});
}

// LFI-33: Check for updates (Official builds: the primary menu item, the footer link, Ctrl+U);
// other builds link to the releases page instead.
KYTY_UI_CASE(lfi_33) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty() || !Store(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	RunWithLibrary(
	    ui,
	    [](KytyApplication* application, KytyMainWindow* window) {
		    GtkWidget* root   = GTK_WIDGET(window);
		    auto&      update = kyty::gtk::MainWindowUpdate(window);
		    GtkWidget* link   = KytyUi::FindByBuildableId(root, "check_updates_link");
		    Check(link != nullptr && gtk_widget_get_visible(link) != FALSE &&
		              gtk_widget_get_visible(KytyUi::FindByBuildableId(root, "releases_link")) ==
		                  FALSE,
		          "an Official build's footer has Check for updates and no releases page");
		    std::vector<std::pair<std::string, std::string>> items;
		    GtkWidget* primary = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			    return GTK_IS_MENU_BUTTON(widget) &&
			           gtk_menu_button_get_primary(GTK_MENU_BUTTON(widget)) != FALSE;
		    });
		    if (primary != nullptr) {
			    MenuItems(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(primary)), items);
		    }
		    Check(std::any_of(items.begin(), items.end(),
		                      [](const auto& item) { return item.second == "app.check-updates"; }),
		          "the primary menu has Check for updates…");
		    Check(update.Stats().checks == 0, "no check at startup (switched off)");
		    Check(KytyUi::Activate(link), "a click on the footer's Check for updates");
		    Check(update.Stats().checks == 1, "the link starts a check");
		    // The loopback server has no release: the check fails, and the user is told.
		    if (GtkWindow* alert = KytyUi::FindAlert("Could not check for updates", 8000);
		        alert != nullptr) {
			    CloseError(alert);
		    }
		    KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "the check to end");
		    gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		    Check(KytyUi::KeyPress(root, GDK_KEY_u, GDK_CONTROL_MASK) && update.Stats().checks == 2,
		          "Ctrl+U starts a check");
		    if (GtkWindow* alert = KytyUi::FindAlert("Could not check for updates", 8000);
		        alert != nullptr) {
			    CloseError(alert);
		    }
		    static_cast<void>(application);
	    },
	    kyty::updater::UpdateUiMode::Official);
	RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root = GTK_WIDGET(window);
		GtkWidget* link = KytyUi::FindByBuildableId(root, "releases_link");
		Check(g_action_group_has_action(G_ACTION_GROUP(application), "check-updates") == FALSE &&
		          gtk_widget_get_visible(KytyUi::FindByBuildableId(root, "check_updates_link")) ==
		              FALSE &&
		          link != nullptr && gtk_widget_get_visible(link) != FALSE,
		      "another build has the releases page link and no update check");
		Check(KytyUi::Activate(link), "a click on Releases page");
		const auto& calls = ui.Services().desktop_launcher.calls;
		KytyUi::WaitFor([&calls]() { return !calls.empty(); }, 3000, "the releases page to open");
		Check(!calls.empty() &&
		          calls.back() == "OpenUri " + std::string(kyty::updater::kReleasesPage),
		      "the link opens the releases page");
	});
}

// LFI-34: Check for updates on startup (Official builds: the global settings' Updates group).
KYTY_UI_CASE(lfi_34) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(
	    ui,
	    [](KytyApplication* application, KytyMainWindow* window) {
		    auto& update = kyty::gtk::MainWindowUpdate(window);
		    Check(update.Stats().checks == 1, "the default checks at startup");
		    KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 8000,
		                    "the startup check to end (quietly)");
		    g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		    auto* settings = KYTY_SETTINGS_WINDOW(
		        WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings"));
		    if (settings == nullptr) {
			    return;
		    }
		    kyty_settings_window_show_page(settings, "updates");
		    GtkWidget* row = RowTitled(GTK_WIDGET(settings), "Check for updates on startup");
		    if (!Check(row != nullptr && ADW_IS_SWITCH_ROW(row) &&
		                   adw_switch_row_get_active(ADW_SWITCH_ROW(row)) != FALSE,
		               "the Updates page has the switch, on")) {
			    return;
		    }
		    adw_switch_row_set_active(ADW_SWITCH_ROW(row), FALSE);
		    GtkWindow* closing = GTK_WINDOW(settings);
		    Check(KytyUi::Activate(KytyUi::FindByBuildableId(GTK_WIDGET(settings), "save_button")),
		          "a click on Save");
		    KytyUi::WaitFor([closing]() { return !KytyUi::IsToplevel(closing); }, 3000,
		                    "Save to close the editor");
	    },
	    kyty::updater::UpdateUiMode::Official);
	Check(!Stored(ui).check_updates_on_startup, "Kyty.ini has check_updates_on_startup=false");
	RunWithLibrary(
	    ui,
	    [](KytyApplication* /*application*/, KytyMainWindow* window) {
		    KytyUi::Pump(300);
		    Check(kyty::gtk::MainWindowUpdate(window).Stats().checks == 0,
		          "after the restart no check runs at startup");
	    },
	    kyty::updater::UpdateUiMode::Official);
}

// LFI-35 (Official builds): the update offer (Install update, Release notes, Cancel), the download
// progress in IEC units (W2) with Cancel, the previous update's result at the next start, and
// "Open release page" after a failed installation. The installed release is set on the context
// before the main window exists, so the case has its own session.
KYTY_UI_CASE(lfi_35) {
	namespace gen = KytyTests::UpdaterGen;
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty() || !Store(ui, [](core::LauncherSettings& settings) {
		    settings.check_updates_on_startup = false;
	    })) {
		return;
	}
	const std::string installed = "2026.03.01-abc1234";
	const std::string latest    = "2026.04.01-def5678";
	const std::string package   = gen::PatternBytes(262144, 7);
	const std::string asset_url =
	    kyty::updater::AssetDownloadUrl(latest, kyty::updater::AssetNameForTag(latest));
	const auto serve = [&](const std::string& digest_of, int stall_after_chunks) {
		KytyTests::LoopbackResponse release;
		release.body = gen::ReleaseDocument(gen::ValidRelease(
		    latest, "2026-04-01T12:00:00Z", gen::DigestOf(digest_of), package.size()));
		ui.Server().Script(kyty::updater::LatestReleaseUrl(), release);
		KytyTests::LoopbackResponse current;
		current.body = gen::ReleaseDocument(
		    gen::ValidRelease(installed, "2026-03-01T12:00:00Z", gen::DigestOf("old"), 3));
		ui.Server().Script(kyty::updater::ReleaseByTagUrl(installed), current);
		KytyTests::LoopbackResponse download;
		download.body               = package;
		download.content_type       = "application/gzip";
		download.chunk_size         = 131072;
		download.chunk_interval_ms  = 20;
		download.stall_after_chunks = stall_after_chunks;
		ui.Server().Script(asset_url, download);
	};
	// The helper the session copies into its workspace, and the result of an earlier update.
	WriteText(ui.AppDirectory() / "kyty_updater", "#!/bin/sh\nexit 0\n");
	Check(static_cast<bool>(kyty::updater::WriteUpdateResult(ui.AppDirectory(),
	                                                         {true, "Updated to 2026.03.01."})),
	      "write the previous update's result");
	serve(package, 1); // 128 KiB, then nothing: the download stays in progress
	OwnSession session(
	    ui, kyty::updater::UpdateUiMode::Official, false,
	    [&installed](kyty::gtk::LauncherContext& context) { context.release_tag = installed; });
	if (!session.Ok()) {
		return;
	}
	KytyMainWindow* window   = session.Window();
	auto&           update   = kyty::gtk::MainWindowUpdate(window);
	const auto&     calls    = ui.Services().desktop_launcher.calls;
	const auto      page_url = "OpenUri " + kyty::gtk::ReleasePageUrl(latest);
	// The previous result, once, when the window is shown.
	if (GtkWindow* result = KytyUi::FindAlert("Update installed"); result != nullptr) {
		Check(KytyUi::AllText(GTK_WIDGET(result)).find("Updated to 2026.03.01.") !=
		          std::string::npos,
		      "the previous result shows its message");
		CloseError(result);
	}
	Check(!Exists(ui.AppDirectory() / ".kyty-update-result"), "the result is read once");
	// The offer: Release notes keeps it (the banner shows it again).
	gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
	// (The banner of the main window has the offer's heading too: the alert is found by its body.)
	GtkWindow* offer = FindDialog("Current: " + installed, GTK_WINDOW(window), 8000);
	if (offer == nullptr) {
		return;
	}
	Check(KytyUi::AllText(GTK_WIDGET(offer)).find("Current: " + installed) != std::string::npos,
	      "the offer names the installed release");
	Check(ButtonLabelled(GTK_WIDGET(offer), {"Cancel"}) != nullptr &&
	          ButtonLabelled(GTK_WIDGET(offer), {"Release notes"}) != nullptr &&
	          ButtonLabelled(GTK_WIDGET(offer), {"Install update"}) != nullptr,
	      "the offer has Cancel, Release notes, and Install update");
	KytyUi::CloseAlert(offer, "Release notes");
	KytyUi::WaitFor([&]() { return !calls.empty() && calls.back() == page_url; }, 3000,
	                "Release notes to open the release's page");
	GtkWidget* banner = KytyUi::FindByBuildableId(GTK_WIDGET(window), "update_banner");
	Check(banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE,
	      "the banner keeps the offer");
	g_signal_emit_by_name(banner, "button-clicked");
	offer = FindDialog("Current: " + installed, GTK_WINDOW(window));
	if (offer == nullptr) {
		return;
	}
	// Install: the progress in IEC units, then Cancel.
	KytyUi::CloseAlert(offer, "Install update");
	KytyUi::WaitFor([&update]() { return update.ProgressWindow() != nullptr; }, 5000,
	                "Install update to open the progress window");
	if (KytyUpdateProgressWindow* progress = update.ProgressWindow(); progress != nullptr) {
		auto* bar =
		    GTK_PROGRESS_BAR(KytyUi::FindByType(GTK_WIDGET(progress), GTK_TYPE_PROGRESS_BAR));
		const std::string expected = "128.0\xc2\xa0KiB of 256.0\xc2\xa0KiB";
		KytyUi::WaitFor(
		    [bar, &expected]() {
			    const char* text = bar != nullptr ? gtk_progress_bar_get_text(bar) : nullptr;
			    return text != nullptr && expected == text;
		    },
		    8000, "the progress to read \"128.0 KiB of 256.0 KiB\"");
		Check(bar != nullptr && gtk_progress_bar_get_fraction(bar) == 0.5, "the bar is half full");
		Check(KytyUi::Capture(GTK_WINDOW(progress), "download"), "the progress window renders");
		const auto messages = update.Stats().messages;
		Check(KytyUi::Activate(KytyUi::FindByBuildableId(GTK_WIDGET(progress), "cancel_button")),
		      "a click on Cancel");
		KytyUi::WaitFor([&update]() { return update.ProgressWindow() == nullptr; }, 5000,
		                "Cancel to close the progress window");
		KytyUi::Pump(200);
		Check(update.Stats().messages == messages, "a cancelled installation shows nothing");
	}
	// A package that does not match its digest: the failure offers the release page.
	serve("another package", 0);
	KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 3000, "the session to be idle");
	gtk_widget_activate_action(GTK_WIDGET(window), "app.check-updates", nullptr);
	if (offer = FindDialog("Current: " + installed, GTK_WINDOW(window), 8000); offer != nullptr) {
		KytyUi::CloseAlert(offer, "Install update");
	}
	if (GtkWindow* failure =
	        FindDialog("The update could not be installed", GTK_WINDOW(window), 10000);
	    failure != nullptr) {
		const auto before = calls.size();
		KytyUi::CloseAlert(failure, "Open release page");
		KytyUi::WaitFor([&]() { return calls.size() > before && calls.back() == page_url; }, 3000,
		                "Open release page to open the release's page");
	}
}

// LFI-36: with no game folder the window shows the empty state with "Add game folder" (Tab,
// Enter), which leads to the library.
KYTY_UI_CASE(lfi_36) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root    = GTK_WIDGET(window);
		GtkWidget* content = KytyUi::FindByBuildableId(root, "content_stack");
		GtkWidget* button  = KytyUi::FindByBuildableId(root, "add_folder_button");
		Check(content != nullptr &&
		          g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(content)), "empty") == 0,
		      "the empty state is shown");
		Check(KytyUi::FindByType(root, ADW_TYPE_STATUS_PAGE) != nullptr &&
		          KytyUi::AllText(root).find("No game folders") != std::string::npos,
		      "the status page says there are no game folders");
		gtk_window_set_focus(GTK_WINDOW(window), nullptr);
		bool reached = false;
		for (int i = 0; i < 30 && !reached; i++) {
			reached = KytyUi::MoveFocus(GTK_WINDOW(window), GTK_DIR_TAB_FORWARD) == button;
		}
		Check(reached, "Tab reaches Add game folder");
		Check(KytyUi::KeyPress(root, GDK_KEY_Return), "Enter on Add game folder");
		auto* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings != nullptr) {
			GtkWidget* settings_root = GTK_WIDGET(settings);
			Check(g_strcmp0(adw_view_stack_get_visible_child_name(
			                    ADW_VIEW_STACK(KytyUi::FindByBuildableId(settings_root, "stack"))),
			                "folders") == 0,
			      "they open on the Folders page");
			ui.Services().file_chooser.folder_answers.push_back({games});
			KytyUi::Activate(ButtonLabelled(settings_root, {"Add folders…"}));
			KytyUi::WaitFor(
			    [&]() {
				    return !Stored(ui).game_folders.empty() ||
				           KytyUi::AllText(settings_root).find(games.string()) != std::string::npos;
			    },
			    3000, "the chosen folder to be listed");
			KytyUi::Activate(KytyUi::FindByBuildableId(settings_root, "save_button"));
		}
		auto& library = ContextOf(application).library;
		KytyUi::WaitFor([&]() { return g_list_model_get_n_items(library.Games()) == 5; }, 8000,
		                "the library to show the folder's 5 games");
		Check(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(content)), "library") == 0,
		      "the library replaces the empty state");
		Quit(application);
	});
	Check(status == 0, "the application ends with status 0");
}

// LFI-37: the Settings_Store is the user file ($XDG_CONFIG_HOME/Kyty/Kyty.ini) unless a Kyty.ini
// exists in the working directory (portable mode, core::DataPaths through the Environment).
KYTY_UI_CASE(lfi_37) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto& user = ui.Context();
	Check(user.paths.settings_file == ui.SettingsFile() &&
	          user.settings.loaded.settings.game_folders.size() == 1,
	      "without a Kyty.ini in the working directory the user file is used");
	// Portable: a Kyty.ini next to the launcher's working directory.
	const fs::path portable = ui.WorkingDirectory() / "Kyty.ini";
	WriteText(portable, "[Launcher]\ngrid_view=true\n");
	ui.Services().environment.AddExistingPath(portable);
	OwnSession session(ui, kyty::updater::UpdateUiMode::Other, false);
	if (!session.Ok()) {
		return;
	}
	auto& context = session.Context();
	Check(context.paths.settings_file == portable, "with one the working directory's file is used");
	Check(context.settings.loaded.settings.grid_view &&
	          context.settings.loaded.settings.game_folders.empty(),
	      "its settings are the portable file's");
	Check(KytyUi::WidgetText(
	          KytyUi::FindByBuildableId(GTK_WIDGET(session.Window()), "settings_label")) ==
	          "Settings file: " + portable.string(),
	      "the footer names the portable file");
	auto next              = context.settings.loaded.settings;
	next.grid_icon_width   = 160;
	const auto user_before = ReadText(ui.SettingsFile());
	Check(static_cast<bool>(kyty::gtk::WriteSettingsStore(context, next)), "save the settings");
	Check(core::OpenSettingsStore(portable).loaded.settings.grid_icon_width == 160,
	      "saving writes the portable file");
	Check(ReadText(ui.SettingsFile()) == user_before, "the user file is untouched");
}

// LFI-38: the windows' icon is the Kyty artwork through the App_ID.
KYTY_UI_CASE(lfi_38) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* /*application*/, KytyMainWindow* window) {
		const char* name = gtk_window_get_default_icon_name();
		Check(name != nullptr && g_strcmp0(name, kyty::gtk::kApplicationId) == 0,
		      "the default window icon name is the App_ID");
		const char* own = gtk_window_get_icon_name(GTK_WINDOW(window));
		Check(own == nullptr || g_strcmp0(own, kyty::gtk::kApplicationId) == 0,
		      "the main window uses it");
		GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
		Check(gtk_icon_theme_has_icon(theme, kyty::gtk::kApplicationId) != FALSE,
		      "the icon theme has the Kyty icon (from the launcher's resources)");
		auto icon = GObjectPtr<GtkIconPaintable>::Adopt(
		    gtk_icon_theme_lookup_icon(theme, kyty::gtk::kApplicationId, nullptr, 48, 1,
		                               GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0)));
		GFile*      file = icon ? gtk_icon_paintable_get_file(icon.get()) : nullptr;
		std::string path;
		if (file != nullptr) {
			const kyty::gtk::GCharPtr uri(g_file_get_uri(file));
			path = std::string(uri.View());
			g_object_unref(file);
		}
		Check(path.find("/io/github/MrG00gle/KytyPS5_GTK/icons/") != std::string::npos,
		      "the icon comes from the launcher's GResource icons (" + path + ")");
	});
}

// LFI-39: the Size column, computed in the background: the regular files below a directory game
// (no symbolic link followed), the size of a .zar file.
KYTY_UI_CASE(lfi_39) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	// A symbolic link to a large file outside the game does not count.
	WriteText(ui.Root() / "large.bin", std::string(100000, 'x'));
	std::error_code ec;
	fs::create_symlink(ui.Root() / "large.bin", games / "Alpha Game" / "link.bin", ec);
	Check(!ec, "a symbolic link inside Alpha Game");
	const auto files_size = [](const fs::path& directory) {
		uint64_t        total = 0;
		std::error_code walk;
		for (auto it = fs::recursive_directory_iterator(directory, walk);
		     it != fs::recursive_directory_iterator(); it.increment(walk)) {
			if (it->is_regular_file(walk) && !it->is_symlink(walk)) {
				total += it->file_size(walk);
			}
		}
		return total;
	};
	const uint64_t alpha_size = files_size(games / "Alpha Game");
	const uint64_t gamma_size = fs::file_size(games / "Gamma Archive.zar", ec);
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&      context     = ContextOf(application);
		KytyGame*  alpha       = GameByTitle(context, kAlpha);
		KytyGame*  gamma       = GameByTitle(context, "Gamma Archive");
		const auto alpha_shown = kyty_game_get_size(alpha);
		const auto gamma_shown = kyty_game_get_size(gamma);
		Check(alpha_shown && *alpha_shown == alpha_size,
		      "Alpha Game's size is its regular files without the link (" +
		          std::to_string(alpha_shown.value_or(0)) + " of " + std::to_string(alpha_size) +
		          ")");
		Check(gamma_shown && *gamma_shown == gamma_size, "Gamma Archive's size is its .zar file");
		Check(kyty::gtk::MainWindowLibrary(window).Stats().details_done >= 1,
		      "the sizes came from the details run");
		const std::string text = KytyUi::AllText(GTK_WIDGET(ListView(window)));
		Check(text.find(kyty::gtk::FormatByteSize(alpha_size)) != std::string::npos &&
		          text.find(kyty::gtk::FormatByteSize(gamma_size)) != std::string::npos,
		      "the Size column shows them in IEC units");
	});
}

// LFI-40: an Emulator that is not there: a banner names its path, and Run shows an error instead
// of quitting.
KYTY_UI_CASE(lfi_40) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path  emulator = ui.AppDirectory() / "kyty_emulator";
	std::error_code ec;
	fs::remove(emulator, ec);
	RunWithLibrary(ui, [&emulator](KytyApplication* /*application*/, KytyMainWindow* window) {
		auto&      run    = kyty::gtk::MainWindowRun(window);
		GtkWidget* banner = KytyUi::FindByBuildableId(GTK_WIDGET(window), "emulator_banner");
		Check(banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE &&
		          std::string(adw_banner_get_title(ADW_BANNER(banner))) ==
		              "Emulator not found at " + emulator.string(),
		      "the banner says where the Emulator is missing");
		FocusGameRow(window, kAlpha);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_r, GDK_CONTROL_MASK),
		      "Ctrl+R is handled");
		if (GtkWindow* alert = KytyUi::FindAlert("The game cannot start"); alert != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(alert)).find(emulator.string()) != std::string::npos,
			      "the error names the Emulator's path");
			CloseError(alert);
		}
		Check(run.Stats().runs_started == 0 && run.Stats().runs_refused == 1, "nothing started");
		Check(g_action_group_get_action_enabled(kyty::gtk::MainWindowGameActions(window), "run") !=
		          FALSE,
		      "Run stays available");
	});
}

// LFI-41 (replaced): no host terminal run mode; the built-in log pane receives the fake
// Emulator's standard output and standard error instead.
KYTY_UI_CASE(lfi_41) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const FakeEmulatorArgs args("--stdout hello-from-the-game --stderr warning-from-the-game "
	                            "--exit 0");
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto& run = kyty::gtk::MainWindowRun(window);
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "run");
		KytyUi::WaitFor([&run]() { return run.Stats().exits == 1; }, 8000,
		                "the game to run and exit");
		KytyUi::WaitFor(
		    [window]() {
			    const auto lines = LogLines(window);
			    return HasLine(lines, "hello-from-the-game") &&
			           HasLine(lines, "warning-from-the-game");
		    },
		    3000, "the log to have the game's standard output and standard error");
		Check(run.Stats().last_arguments.empty() || run.Stats().last_arguments[0] != "-e",
		      "the Emulator runs without a terminal");
		Check(run.Stats().last_error.empty(), "a clean exit shows no error");
	});
}

// LFI-42 (new): the log pane (Ctrl+L) shows the Emulator's output while the game runs.
KYTY_UI_CASE(lfi_42) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const FakeEmulatorArgs args("--stdout line-while-running --wait");
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root = GTK_WIDGET(window);
		GtkWidget* log  = KytyUi::FindByBuildableId(root, "log_area");
		Check(log != nullptr && gtk_widget_get_visible(log) == FALSE, "the log is hidden at first");
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_l, GDK_CONTROL_MASK) &&
		          gtk_widget_get_visible(log) != FALSE,
		      "Ctrl+L shows the log pane");
		Check(KytyUi::FindByAccessibleLabel(log, "Emulator log") != nullptr,
		      "the log list is named Emulator log");
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "run");
		KytyUi::WaitFor(
		    [log]() {
			    return KytyUi::AllText(log).find("line-while-running") != std::string::npos;
		    },
		    5000, "the running game's line in the log pane");
		Check(kyty::gtk::MainWindowGameRunning(window), "the game still runs");
		Check(KytyUi::Capture(GTK_WINDOW(window), "log"), "the window with the log renders");
		ActivateGameAction(window, "stop");
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_l, GDK_CONTROL_MASK) &&
		          gtk_widget_get_visible(log) == FALSE,
		      "Ctrl+L hides it again");
	});
}

// LFI-43 (new): Stop (the header bar button) stops the running game.
KYTY_UI_CASE(lfi_43) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const FakeEmulatorArgs args("--stdout ready --wait");
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      run  = kyty::gtk::MainWindowRun(window);
		GtkWidget* stop = ButtonWithAction(GTK_WIDGET(window), "game.stop");
		Check(stop != nullptr && gtk_widget_get_sensitive(stop) == FALSE,
		      "the header bar's Stop is off while no game runs");
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "run");
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "the game to run");
		KytyUi::WaitFor([stop]() { return gtk_widget_get_sensitive(stop) != FALSE; }, 2000,
		                "Stop to be on");
		Check(KytyUi::Activate(stop), "a click on Stop");
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "Stop to end the game");
		Check(run.Stats().stop_requested_us > 0 && run.Stats().exits == 1 &&
		          run.Stats().last_error.empty(),
		      "the game was stopped, and a stop asked for shows no error");
	});
}

// LFI-44 (new): Save log (the log pane's button and Ctrl+S) writes the log to the chosen file.
KYTY_UI_CASE(lfi_44) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty()) {
		return;
	}
	const fs::path         first  = ui.Root() / "first.log";
	const fs::path         second = ui.Root() / "second.log";
	const FakeEmulatorArgs args("--stdout first-line --stdout second-line --exit 0");
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&      chooser = ui.Services().file_chooser;
		GtkWidget* root    = GTK_WIDGET(window);
		SelectTitle(ContextOf(application), kAlpha);
		ActivateGameAction(window, "run");
		KytyUi::WaitFor(
		    [window]() {
			    const auto lines = LogLines(window);
			    return HasLine(lines, "first-line") && HasLine(lines, "second-line");
		    },
		    8000, "the game's lines in the log");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-log", nullptr);
		GtkWidget* log = KytyUi::FindByBuildableId(root, "log_area");
		chooser.file_answers.push_back(first);
		Check(KytyUi::Activate(ButtonWithAction(log, "app.save-log")),
		      "a click on the log pane's Save log…");
		KytyUi::WaitFor([&]() { return Exists(first); }, 3000, "the log file to be written");
		Check(ReadText(first).find("first-line\nsecond-line\n") != std::string::npos,
		      "the file has the log's lines");
		Check(!chooser.calls.empty() && chooser.calls.back() == "SaveFile Save log | kyty.log",
		      "the chooser suggests kyty.log");
		chooser.file_answers.push_back(second);
		gtk_widget_grab_focus(GTK_WIDGET(ListView(window)));
		Check(KytyUi::KeyPress(root, GDK_KEY_s, GDK_CONTROL_MASK), "Ctrl+S is handled");
		KytyUi::WaitFor([&]() { return Exists(second); }, 3000, "Ctrl+S to write the log");
		Check(ReadText(second) == ReadText(first), "Ctrl+S writes the same log");
	});
}

// LFI-45 (new): Export settings and Import settings of the game menu.
KYTY_UI_CASE(lfi_45) {
	KytyUi::UiContext ui;
	const auto        games = PrepareLibrary(ui);
	const auto        alpha = FixtureEntry(games, kAlpha);
	if (games.empty() || !StoreCustomUserName(ui, alpha, "Alpha Player")) {
		return;
	}
	const fs::path exported = ui.Root() / "PPSA01001.json";
	const fs::path import   = ui.Root() / "beta.json";
	WriteText(import, "{\"PPSA01002\": {\"user_name\": \"Beta Import\"}}");
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&      chooser = ui.Services().file_chooser;
		GtkWidget* root    = GTK_WIDGET(window);
		DisableAutohide(root);
		// Export through the game menu (the Menu key).
		FocusGameRow(window, kAlpha);
		chooser.file_answers.push_back(exported);
		Check(KytyUi::KeyPress(root, GDK_KEY_Menu), "the Menu key is handled");
		GtkPopover* menu = nullptr;
		KytyUi::WaitFor(
		    [&]() {
			    menu = MappedPopover(root, "Export settings…");
			    return menu != nullptr;
		    },
		    2000, "the game menu");
		if (menu != nullptr) {
			GtkWidget* item = MenuItem(GTK_WIDGET(menu), "Export settings…");
			if (Check(item != nullptr, "the game menu has Export settings…")) {
				g_signal_emit_by_name(item, "clicked");
			}
		}
		KytyUi::WaitFor([&]() { return Exists(exported); }, 3000, "the export to be written");
		Check(!chooser.calls.empty() &&
		          chooser.calls.back() == "SaveFile Export game config | PPSA01001.json",
		      "the chooser suggests PPSA01001.json");
		const auto read = core::ReadGameSettingsFile(exported);
		Check(read && read->by_title.contains("PPSA01001") &&
		          ReadText(exported).find("Alpha Player") != std::string::npos,
		      "the file has Alpha Game's own settings");
		if (menu != nullptr && gtk_widget_get_mapped(GTK_WIDGET(menu)) != FALSE) {
			gtk_popover_popdown(menu);
		}
		// Import into Beta Quest.
		SelectTitle(ContextOf(application), kBeta);
		chooser.file_answers.push_back(import);
		ActivateGameAction(window, "import-settings");
		KytyUi::WaitFor(
		    [&]() { return StoredUserName(ui, FixtureEntry(games, kBeta)) == "Beta Import"; }, 3000,
		    "the import to give Beta Quest the file's user name");
		Check(chooser.calls.back().starts_with("OpenFile Import game config"),
		      "the chooser asks for a JSON file");
	});
	Check(core::HasCustomSettings(Stored(ui), FixtureEntry(games, kBeta)),
	      "Beta Quest has its own settings now");
}

// LFI-46 (replaced): no theme selector; the colors follow the desktop's color-scheme preference
// (the fake portal), and a stored Qt theme changes nothing.
KYTY_UI_CASE(lfi_46) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty() ||
	    !Store(ui, [](core::LauncherSettings& settings) { settings.theme = "2"; })) {
		return;
	}
	RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* /*window*/) {
		AdwStyleManager* style = adw_style_manager_get_default();
		Check(adw_style_manager_get_color_scheme(style) == ADW_COLOR_SCHEME_DEFAULT,
		      "the launcher follows the desktop (the stored Qt theme is not used)");
		Check(adw_style_manager_get_dark(style) == FALSE, "no preference is light");
		if (KytyUi::SetPortalColorScheme(1)) {
			KytyUi::WaitFor([style]() { return adw_style_manager_get_dark(style) != FALSE; }, 1000,
			                "the desktop's dark preference to make the launcher dark");
		}
		if (KytyUi::SetPortalColorScheme(0)) {
			KytyUi::WaitFor([style]() { return adw_style_manager_get_dark(style) == FALSE; }, 1000,
			                "no preference to make it light again");
		}
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(settings));
			Check(text.find("Theme") == std::string::npos &&
			          text.find("Dark") == std::string::npos &&
			          RowTitled(GTK_WIDGET(settings), "Theme") == nullptr,
			      "no page of the editor offers a theme");
			CloseWithEscape(settings, "the global settings");
		}
	});
	Check(Stored(ui).theme == std::optional<std::string>("2"), "the stored value is kept as it is");
}

// LFI-47: the main window's and the settings editor's size come back after a restart (the
// position is the compositor's).
KYTY_UI_CASE(lfi_47) {
	KytyUi::UiContext ui;
	if (PrepareLibrary(ui).empty() || !Store(ui, [](core::LauncherSettings& settings) {
		    settings.window          = {1000, 650, false};
		    settings.settings_window = {880, 660, false};
	    })) {
		return;
	}
	const auto default_size = [](GtkWindow* window) {
		int width  = 0;
		int height = 0;
		gtk_window_get_default_size(window, &width, &height);
		return std::pair<int, int> {width, height};
	};
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		Check(default_size(GTK_WINDOW(window)) == std::pair<int, int> {1000, 650},
		      "the main window opens at the stored 1000x650");
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		if (settings != nullptr) {
			Check(default_size(settings) == std::pair<int, int> {880, 660},
			      "the editor opens at the stored 880x660");
			gtk_window_set_default_size(settings, 940, 700);
			KytyUi::WaitFor([&]() { return default_size(settings).first == 940; }, 2000,
			                "the editor's new size");
			CloseWithEscape(settings, "the global settings");
		}
		gtk_window_set_default_size(GTK_WINDOW(window), 1111, 701);
		KytyUi::WaitFor([&]() { return default_size(GTK_WINDOW(window)).first == 1111; }, 2000,
		                "the main window's new size");
	});
	const auto stored = Stored(ui);
	Check(stored.window.width == 1111 && stored.window.height == 701 && !stored.window.maximized,
	      "quitting stores the main window's 1111x701 (" + std::to_string(stored.window.width) +
	          "x" + std::to_string(stored.window.height) + ")");
	Check(stored.settings_window.width == 940 && stored.settings_window.height == 700,
	      "and the editor's 940x700");
	RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		Check(default_size(GTK_WINDOW(window)) == std::pair<int, int> {1111, 701},
		      "after the restart the main window is 1111x701");
		g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
		if (GtkWindow* settings = WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
		    settings != nullptr) {
			Check(default_size(settings) == std::pair<int, int> {940, 700},
			      "and the editor 940x700");
			CloseWithEscape(settings, "the global settings");
		}
	});
}
