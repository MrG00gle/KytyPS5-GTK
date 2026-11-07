// UI tests of keyboard use (task 22.2; Requirement 6.11; LFI-06, LFI-24; phase 7 plan D10, D18,
// D19, D30), each on Wayland and on X11. weston's headless session has a fake seat with no input
// devices, so nothing can type there and the harness dispatches the keys the way GTK does
// (KytyUi::KeyPress, KytyUi::MoveFocus; plan D10, G4): the cases test the launcher's bindings, its
// focus chain, and its handlers, not the compositor's key delivery.
// - keyboard_tab_cycle_<window> (main window, settings window with every page, input mapping
//   window with both tabs, cheats window with both tabs, an alert): Tab (the window's move-focus)
//   goes through a cycle of focus stops that reaches every focusable interactive control (one stop
//   for a list or grid view, whose items take the arrow keys) and comes back to the start;
//   Shift+Tab goes through the same stops in reverse. The cheats window shows Alpha Game's content
//   (UiContext::WriteGameContent: two local cheats, the remote file list), and Tab reaches its
//   content controls (KytyUi::GameContentControls).
// - keyboard_focus_visible: after Tab the focused control has GTK_STATE_FLAG_FOCUS_VISIBLE and the
//   control's area of the settled rendering differs from the unfocused one (the focus indicator).
// - keyboard_accelerators: every accelerator of the D30 table resolves to its action
//   (gtk_application_get_actions_for_accel) and, pressed where its action is enabled, has its
//   effect (in an Official build, where Ctrl+U exists).
// - keyboard_view_keys: in the game list, Enter runs the game, Delete asks to clear its config,
//   Menu and Shift+F10 open the game menu; Escape in the search entry clears it; Ctrl+U does
//   nothing in an Other build.
// - keyboard_sort (W4-02, D18): F10 opens the primary menu, the arrow keys and Enter choose each
//   column of "Sort by" and then "Descending"; the list sorts like the column headers, the header
//   shows the sort, and a header click updates the menu's state. A column chosen while
//   "Descending" is checked sorts descending: the menu keeps the direction (a header click on
//   another column sorts ascending, Requirement 7.12).
// - keyboard_binding_delete (W4-01, D19): Delete unmaps a binding whether its row or its button
//   has the focus; Enter on the button still opens the capture dialog.

#include "launcher/core/gameLibrary.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/bindingCaptureDialog.h"
#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/runController.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/updateUi.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiWindows.h"

#include <adwaita.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;
using KytyUi::Check;
using KytyUi::MappedToplevel;

void Quit(KytyApplication* application) {
	g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
}

std::string Describe(GtkWidget* widget) {
	if (widget == nullptr) {
		return "(no focus)";
	}
	return KytyUi::RoleName(widget) + " \"" + KytyUi::ControlName(widget) + "\" (" +
	       G_OBJECT_TYPE_NAME(widget) + ")";
}

// The widget that is one Tab stop for its items, which take the arrow keys: a list, grid, or column
// view (GTK_LIST_TAB_ITEM), or a view switcher (a tab list: Tab reaches the shown page's tab).
GtkWidget* EnclosingGroup(GtkWidget* widget) {
	for (GtkWidget* at = widget; at != nullptr; at = gtk_widget_get_parent(at)) {
		if (GTK_IS_LIST_VIEW(at) || GTK_IS_GRID_VIEW(at) || GTK_IS_COLUMN_VIEW(at) ||
		    ADW_IS_VIEW_SWITCHER(at)) {
			return at;
		}
	}
	return nullptr;
}

// No ancestor of widget, and not widget, has can-focus off.
bool FocusReachable(GtkWidget* widget) {
	for (GtkWidget* at = widget; at != nullptr; at = gtk_widget_get_parent(at)) {
		if (gtk_widget_get_can_focus(at) == FALSE) {
			return false;
		}
	}
	return true;
}

std::string Stops(const std::vector<GtkWidget*>& stops) {
	std::string text;
	for (GtkWidget* stop: stops) {
		text += "\n    " + Describe(stop);
	}
	return text;
}

bool HasFocusableDescendant(GtkWidget* widget) {
	return KytyUi::FindFirst(widget, [](GtkWidget* candidate) {
		       return gtk_widget_get_focusable(candidate) != FALSE &&
		              gtk_widget_get_mapped(candidate) != FALSE;
	       }) != nullptr;
}

// control is a Tab stop, inside one (a composite: an entry's text, a menu button's button), or an
// item of a list view with a stop.
bool Reached(const std::vector<GtkWidget*>& stops, GtkWidget* control) {
	GtkWidget* list = EnclosingGroup(control);
	return std::any_of(stops.begin(), stops.end(), [&](GtkWidget* stop) {
		return stop == control || gtk_widget_is_ancestor(stop, control) != FALSE ||
		       (list != nullptr && gtk_widget_is_ancestor(stop, list) != FALSE);
	});
}

// Tab from no focus until the focus comes back to the first stop, then Shift+Tab as far back.
// Returns the stops (empty when Tab does not cycle).
std::vector<GtkWidget*> CheckTabCycle(GtkWindow* window, const std::string& where) {
	gtk_window_set_focus(window, nullptr);
	std::vector<GtkWidget*> stops;
	GtkWidget*              first = KytyUi::MoveFocus(window, GTK_DIR_TAB_FORWARD);
	if (!Check(first != nullptr, where + ": Tab focuses a control")) {
		return {};
	}
	stops.push_back(first);
	bool closed = false;
	for (int i = 0; i < 400; i++) {
		GtkWidget* next = KytyUi::MoveFocus(window, GTK_DIR_TAB_FORWARD);
		if (next == first) {
			closed = true;
			break;
		}
		if (!Check(next != nullptr, where + ": Tab keeps a focus")) {
			return {};
		}
		if (std::find(stops.begin(), stops.end(), next) != stops.end()) {
			KytyUi::Fail(where + ": Tab comes back to " + Describe(next) +
			             " without reaching the first stop again; the stops:" + Stops(stops));
			return {};
		}
		stops.push_back(next);
	}
	if (!Check(closed, where + ": Tab cycles back to the first stop")) {
		return {};
	}
	std::printf("%s: %s: %zu Tab stops:%s\n", KytyUi::CaseName().c_str(), where.c_str(),
	            stops.size(), Stops(stops).c_str());

	// Shift+Tab from the first stop: the last, ..., the second, and the first stop again.
	std::vector<GtkWidget*> back;
	for (std::size_t i = 0; i < stops.size(); i++) {
		back.push_back(KytyUi::MoveFocus(window, GTK_DIR_TAB_BACKWARD));
	}
	std::vector<GtkWidget*> reversed(stops.rbegin(), stops.rend() - 1);
	reversed.push_back(stops.front());
	for (std::size_t i = 0; i < back.size(); i++) {
		if (back[i] != reversed[i]) {
			KytyUi::Fail(where + ": Shift+Tab step " + std::to_string(i + 1) + " focuses " +
			             Describe(back[i]) + ", not " + Describe(reversed[i]));
			break;
		}
	}

	// Every focusable interactive control is a stop, inside one, or an item of a list view with a
	// stop.
	for (GtkWidget* control: KytyUi::InteractiveControls(GTK_WIDGET(window))) {
		if (gtk_widget_is_sensitive(control) == FALSE || !FocusReachable(control) ||
		    (gtk_widget_get_focusable(control) == FALSE && !HasFocusableDescendant(control))) {
			continue;
		}
		Check(Reached(stops, control), where + ": Tab never reaches the " + Describe(control) +
		                                   " at " + KytyUi::WidgetPath(control));
	}
	return stops;
}

// The content controls of page (KytyUi::GameContentControls) are on the page, and Tab reaches
// them.
void CheckContentReached(GtkWindow* window, const std::vector<GtkWidget*>& stops,
                         const std::vector<KytyUi::ContentControl>& content,
                         const std::string& page, const std::string& where) {
	const auto controls = KytyUi::InteractiveControls(GTK_WIDGET(window));
	for (const auto& kind: content) {
		if (kind.page != page) {
			continue;
		}
		const bool reached = std::any_of(controls.begin(), controls.end(), [&](GtkWidget* control) {
			return kind.match(control) && Reached(stops, control);
		});
		std::printf("%s: %s: %s %s\n", KytyUi::CaseName().c_str(), where.c_str(), kind.what.c_str(),
		            reached ? "reached" : "missing");
		Check(reached, where + ": Tab reaches " + kind.what);
	}
}

// Every visible page of the window's view stack in turn, with the content controls of each page.
void CheckTabCyclePerPage(GtkWindow* window, const std::string& what,
                          const std::vector<KytyUi::ContentControl>& content = {}) {
	std::vector<std::string> walked; // the pages cycled

	const auto check_walked = [&]() {
		for (const auto& kind: content) {
			Check(std::find(walked.begin(), walked.end(), kind.page) != walked.end(),
			      what + ": the page \"" + kind.page + "\" of " + kind.what + " is cycled");
		}
	};
	GtkWidget* widget = KytyUi::FindFirst(GTK_WIDGET(window), [](GtkWidget* candidate) {
		return ADW_IS_VIEW_STACK(candidate) && gtk_widget_get_mapped(candidate) != FALSE;
	});
	if (widget == nullptr) {
		CheckContentReached(window, CheckTabCycle(window, what), content, "", what);
		walked.emplace_back();
		check_walked();
		return;
	}
	auto*       stack = ADW_VIEW_STACK(widget);
	auto        pages = GObjectPtr<GtkSelectionModel>::Adopt(adw_view_stack_get_pages(stack));
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(pages.get()));
	for (guint i = 0; i < count; i++) {
		auto page = GObjectPtr<AdwViewStackPage>::Adopt(
		    static_cast<AdwViewStackPage*>(g_list_model_get_item(G_LIST_MODEL(pages.get()), i)));
		if (adw_view_stack_page_get_visible(page.get()) == FALSE) {
			continue;
		}
		const char*       name = adw_view_stack_page_get_name(page.get());
		const std::string page_name(name != nullptr ? name : "?");
		adw_view_stack_set_visible_child_name(stack, name);
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		KytyUi::Pump(50);
		const std::string where = what + ", page " + page_name;
		CheckContentReached(window, CheckTabCycle(window, where), content, page_name, where);
		walked.push_back(page_name);
	}
	check_walked();
}

// Opens the registry's id (for Alpha Game, with_entry) and checks the Tab cycle of every page. A
// window with content controls (KytyUi::GameContentControls) gets Alpha Game's content first.
void CheckRegistryTabCycle(const char* id, bool with_entry) {
	KytyUi::UiContext ui;
	GtkWindow*        parent  = ui.ParentWindow();
	const auto        games   = ui.WriteFixtureGames();
	const auto        content = KytyUi::GameContentControls(id);
	if (!content.empty()) {
		ui.WriteGameContent(games);
	}
	core::GameEntry entry;
	entry.path       = games / "Alpha Game";
	entry.title_name = "Alpha Game";
	entry.title_id   = "PPSA01001";
	const auto* type = kyty::gtk::FindDialogType(id);
	if (!Check(type != nullptr, std::string(id) + " is registered")) {
		return;
	}
	kyty::gtk::DialogArgs args;
	if (with_entry) {
		args.entry = &entry;
	}
	const guint             before = KytyUi::ToplevelCount();
	kyty::gtk::DialogHandle handle = type->open(ui.Context(), parent, args);
	GtkWindow*              window = handle.window;
	if (Check(window != nullptr, std::string(id) + " opens a window") &&
	    KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                    5000, std::string(id) + " to be mapped")) {
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		KytyUi::WaitFor(
		    [&content, window]() {
			    return std::all_of(content.begin(), content.end(), [window](const auto& kind) {
				    return KytyUi::FindFirst(GTK_WIDGET(window), kind.match) != nullptr;
			    });
		    },
		    5000, std::string(id) + " to show the content of Alpha Game");
		KytyUi::Pump(500);
		CheckTabCyclePerPage(window, id, content);
	}
	type->close(handle);
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
	                std::string(id) + " to close");
}

// The main window's list view and its column with the view id.
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

// The titles in the order core::SortRows gives for the library's games (scan order input).
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

std::string StateString(GActionGroup* group, const char* action) {
	GVariant* state = g_action_group_get_action_state(group, action);
	if (state == nullptr) {
		return "(none)";
	}
	std::string text = g_variant_is_of_type(state, G_VARIANT_TYPE_STRING) != FALSE
	                       ? g_variant_get_string(state, nullptr)
	                       : (g_variant_get_boolean(state) != FALSE ? "true" : "false");
	g_variant_unref(state);
	return text;
}

// The deepest focus child below widget (a popover's focused item), or widget.
GtkWidget* FocusIn(GtkWidget* widget) {
	GtkWidget* at = widget;
	while (GtkWidget* child = gtk_widget_get_focus_child(at)) {
		at = child;
	}
	return at;
}

// The text of a popover menu item: the label inside the focused GtkModelButton.
std::string ItemText(GtkWidget* item) {
	for (GtkWidget* at = item; at != nullptr; at = gtk_widget_get_parent(at)) {
		if (g_strcmp0(G_OBJECT_TYPE_NAME(at), "GtkModelButton") == 0) {
			const std::string text = KytyUi::AllText(at);
			return text.substr(0, text.find('\n'));
		}
	}
	return "";
}

// The keyboard focus is on a shown item of the popover's current page: with a seat, the
// compositor gives an opened popup the keyboard and GTK focuses its first item (D10 emulates it).
void EnsureItemFocus(GtkPopover* popover) {
	GtkWidget* focus = FocusIn(GTK_WIDGET(popover));
	if (focus == GTK_WIDGET(popover) || gtk_widget_get_mapped(focus) == FALSE ||
	    ItemText(focus).empty()) {
		gtk_widget_child_focus(GTK_WIDGET(popover), GTK_DIR_TAB_FORWARD);
	}
}

// Down (then Up) until the popover's focused item reads label; false after a Fail.
bool FocusItem(GtkPopover* popover, std::string_view label) {
	EnsureItemFocus(popover);
	for (const guint key: {GDK_KEY_Down, GDK_KEY_Up}) {
		for (int i = 0; i < 20; i++) {
			if (ItemText(FocusIn(GTK_WIDGET(popover))) == label) {
				return true;
			}
			KytyUi::KeyPress(FocusIn(GTK_WIDGET(popover)), key);
		}
	}
	KytyUi::Fail("the arrow keys never reach the menu item \"" + std::string(label) +
	             "\" (the focus is on \"" + ItemText(FocusIn(GTK_WIDGET(popover))) + "\")");
	return false;
}

// F10, then the arrow keys and Enter to "Sort by", then to label: the menu closes.
bool ChooseSortItem(KytyMainWindow* window, GtkPopover* popover, std::string_view label) {
	if (!KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F10) ||
	    !KytyUi::WaitFor(
	        [popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE; }, 2000,
	        "F10 to open the primary menu")) {
		return false;
	}
	if (!FocusItem(popover, "Sort by") ||
	    !Check(KytyUi::KeyPress(FocusIn(GTK_WIDGET(popover)), GDK_KEY_Return),
	           "Enter opens \"Sort by\"")) {
		return false;
	}
	KytyUi::Pump(50);
	if (!FocusItem(popover, label) ||
	    !Check(KytyUi::KeyPress(FocusIn(GTK_WIDGET(popover)), GDK_KEY_Return),
	           "Enter chooses \"" + std::string(label) + "\"")) {
		return false;
	}
	return KytyUi::WaitFor(
	    [popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) == FALSE; }, 2000,
	    "the primary menu to close after \"" + std::string(label) + "\"");
}

// The popover as its renderer draws it, in the light and the dark scheme (D33), with
// --capture-dir.
void CapturePopover(GtkPopover* popover, const std::string& name) {
	if (KytyUi::CaptureDirectory().empty()) {
		return;
	}
	AdwStyleManager*     style    = adw_style_manager_get_default();
	const AdwColorScheme previous = adw_style_manager_get_color_scheme(style);
	for (const bool dark: {false, true}) {
		adw_style_manager_set_color_scheme(style, dark ? ADW_COLOR_SCHEME_FORCE_DARK
		                                               : ADW_COLOR_SCHEME_FORCE_LIGHT);
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(popover));
		KytyUi::Pump(300);
		auto paintable =
		    GObjectPtr<GdkPaintable>::Adopt(gtk_widget_paintable_new(GTK_WIDGET(popover)));
		const int    width    = gdk_paintable_get_intrinsic_width(paintable.get());
		const int    height   = gdk_paintable_get_intrinsic_height(paintable.get());
		GtkSnapshot* snapshot = gtk_snapshot_new();
		gdk_paintable_snapshot(paintable.get(), snapshot, width, height);
		GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
		const auto path = KytyUi::CaptureDirectory() /
		                  (KytyUi::CaseName() + "-" + name + (dark ? "-dark.png" : "-light.png"));
		bool       written = false;
		if (node != nullptr) {
			graphene_rect_t bounds;
			gsk_render_node_get_bounds(node, &bounds);
			auto texture = GObjectPtr<GdkTexture>::Adopt(gsk_renderer_render_texture(
			    gtk_native_get_renderer(GTK_NATIVE(popover)), node, &bounds));
			gsk_render_node_unref(node);
			written = texture && gdk_texture_save_to_png(texture.get(), path.c_str()) != FALSE;
		}
		Check(written, "write " + path.string());
		std::printf("%s: captured %s (%dx%d)\n", KytyUi::CaseName().c_str(), path.c_str(), width,
		            height);
	}
	adw_style_manager_set_color_scheme(style, previous);
}

bool HasAction(gchar** actions, const char* action) {
	return actions != nullptr && g_strv_contains(actions, action) != FALSE;
}

GtkWidget* AlertButton(GtkWindow* alert, std::initializer_list<std::string_view> labels) {
	return KytyUi::FindFirst(GTK_WIDGET(alert), [labels](GtkWidget* widget) {
		const std::string text = KytyUi::WidgetText(widget);
		return GTK_IS_BUTTON(widget) &&
		       std::find(labels.begin(), labels.end(), text) != labels.end();
	});
}

// Tab to the list view, the arrow keys to the row of title, and Space to select it unless it is
// selected; the focused row, or nullptr after a Fail.
GtkWidget* FocusGameRow(KytyMainWindow* window, GtkColumnView* view, std::string_view title) {
	GtkWindow* gtk_window = GTK_WINDOW(window);
	const auto in_view    = [gtk_window, view]() -> GtkWidget* {
		GtkWidget* focus = gtk_window_get_focus(gtk_window);
		return focus != nullptr && gtk_widget_is_ancestor(focus, GTK_WIDGET(view)) != FALSE
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
				// Space toggles the selection of the focused row.
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

// The fixture games with Alpha Game's own settings in Kyty.ini; Alpha Game's path.
fs::path WriteGamesWithCustomSettings(KytyUi::UiContext& ui) {
	const auto games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return {};
	}
	auto store        = core::OpenSettingsStore(ui.SettingsFile());
	auto next         = store.loaded.settings;
	next.game_folders = {games.string()};
	core::GameEntry alpha;
	alpha.path       = games / "Alpha Game";
	alpha.title_name = "Alpha Game";
	alpha.title_id   = "PPSA01001";
	Check(static_cast<bool>(
	          core::SaveCustomSettings(next, alpha, core::EnableCustomSettings(next, alpha))),
	      "Alpha Game gets custom settings");
	Check(static_cast<bool>(core::SaveSettingsStore(store, next)), "write Kyty.ini");
	return alpha.path;
}

} // namespace

KYTY_UI_CASE(keyboard_tab_cycle_main_window) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([](KytyApplication* application, KytyMainWindow* window) {
		auto& library = kyty::gtk::MainWindowLibrary(window);
		KytyUi::WaitFor([&library]() { return library.Stats().details_done >= 1; }, 8000,
		                "the games' details");
		CheckTabCycle(GTK_WINDOW(window), "main window");
		Quit(application);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(keyboard_tab_cycle_settings_window) {
	CheckRegistryTabCycle("settings-window", false);
}

KYTY_UI_CASE(keyboard_tab_cycle_input_mapping_window) {
	CheckRegistryTabCycle("input-mapping-window", false);
}

KYTY_UI_CASE(keyboard_tab_cycle_cheats_window) {
	CheckRegistryTabCycle("cheats-window", true);
}

KYTY_UI_CASE(keyboard_tab_cycle_alert) {
	CheckRegistryTabCycle("confirmation-alert", false);
}

KYTY_UI_CASE(keyboard_focus_visible) {
	KytyUi::UiContext ui;
	ui.ServeCompatibility();
	const int status = ui.RunApplication([](KytyApplication* application, KytyMainWindow* window) {
		GtkWindow* gtk_window = GTK_WINDOW(window);
		gtk_window_set_focus(gtk_window, nullptr);
		gtk_window_set_focus_visible(gtk_window, FALSE);
		// Settled renderings (p7-pr-4): the focus ring fades in through a CSS transition, which the
		// first painted frame after Tab may still show at its start, the more so under CPU load.
		const auto before = KytyUi::RenderSettled(gtk_window, "before Tab");
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Tab), "Tab is handled");
		GtkWidget* focus = gtk_window_get_focus(gtk_window);
		std::printf("%s: Tab focused %s\n", KytyUi::CaseName().c_str(), Describe(focus).c_str());
		Check(focus != nullptr, "Tab focuses a control");
		// KytyUi::KeyPress sets it itself (its emulation of gtk_main_do_event, D10): a check of
		// the harness; the pixel comparison below is what checks the indicator.
		Check(gtk_window_get_focus_visible(gtk_window) != FALSE,
		      "the harness's key press sets the window's focus-visible (as gtk_main_do_event)");
		Check(focus != nullptr &&
		          (gtk_widget_get_state_flags(focus) & GTK_STATE_FLAG_FOCUS_VISIBLE) != 0,
		      "the focused control has GTK_STATE_FLAG_FOCUS_VISIBLE");
		const auto      after = KytyUi::RenderSettled(gtk_window, "after Tab");
		graphene_rect_t bounds;
		if (focus != nullptr && before && after &&
		    gtk_widget_compute_bounds(focus, GTK_WIDGET(window), &bounds) != FALSE) {
			const int           width  = gdk_texture_get_width(after.get());
			const int           height = gdk_texture_get_height(after.get());
			const gsize         stride = static_cast<gsize>(width) * 4;
			std::vector<guint8> a(stride * static_cast<gsize>(height));
			std::vector<guint8> b(stride * static_cast<gsize>(height));
			Check(gdk_texture_get_width(before.get()) == width &&
			          gdk_texture_get_height(before.get()) == height,
			      "both renderings have the same size");
			gdk_texture_download(before.get(), a.data(), stride);
			gdk_texture_download(after.get(), b.data(), stride);
			// The focus ring is drawn around the control: its bounds and 4 px around them.
			const int x0 = std::max(0, static_cast<int>(bounds.origin.x) - 4);
			const int y0 = std::max(0, static_cast<int>(bounds.origin.y) - 4);
			const int x1 =
			    std::min(width, static_cast<int>(bounds.origin.x + bounds.size.width) + 4);
			const int y1 =
			    std::min(height, static_cast<int>(bounds.origin.y + bounds.size.height) + 4);
			int changed = 0;
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					const gsize at = static_cast<gsize>(y) * stride + static_cast<gsize>(x) * 4;
					changed += std::equal(a.begin() + static_cast<std::ptrdiff_t>(at),
					                      a.begin() + static_cast<std::ptrdiff_t>(at + 4),
					                      b.begin() + static_cast<std::ptrdiff_t>(at))
					               ? 0
					               : 1;
				}
			}
			std::printf("%s: %d pixels changed in the focused control's area (%d,%d)-(%d,%d)\n",
			            KytyUi::CaseName().c_str(), changed, x0, y0, x1, y1);
			Check(changed > 0, "the focused control looks different (the focus indicator)");
		}
		Check(KytyUi::Capture(gtk_window, "focus"), "the focused main window renders");
		Quit(application);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(keyboard_accelerators) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	g_setenv("KYTY_UI_FAKE_ARGS", "--stdout ready --wait", TRUE);
	const fs::path log_file = ui.Root() / "saved.log";
	const fs::path alpha    = games / "Alpha Game";
	const int      status   = ui.RunApplication(
	    [&](KytyApplication* application, KytyMainWindow* window) {
		    GtkApplication* app     = GTK_APPLICATION(application);
		    GtkWidget*      root    = GTK_WIDGET(window);
		    auto*           context = kyty::gtk::ApplicationContext(application);
		    auto&           library = kyty::gtk::MainWindowLibrary(window);
		    auto&           run     = kyty::gtk::MainWindowRun(window);
		    auto&           update  = kyty::gtk::MainWindowUpdate(window);
		    KytyUi::WaitFor([&library]() { return library.Stats().scans_applied >= 1; }, 8000,
		                    "the first scan to be applied");
		    // The D30 table.
		    const std::pair<const char*, const char*> table[] = {
		        {"F5", "app.refresh"},
		        {"<Control>f", "app.search"},
		        {"<Control>g", "app.toggle-grid"},
		        {"<Control>comma", "app.global-settings"},
		        {"<Control>l", "app.toggle-log"},
		        {"<Control>s", "app.save-log"},
		        {"<Control>u", "app.check-updates"},
		        {"<Control>q", "app.quit"},
		        {"<Control>r", "game.run"},
		        {"<Control>period", "game.stop"},
		        {"<Control>o", "game.open-folder"},
		        {"<Control>e", "game.edit-config"},
		    };
		    for (const auto& [accelerator, action]: table) {
			    gchar** actions = gtk_application_get_actions_for_accel(app, accelerator);
			    Check(HasAction(actions, action),
			          std::string(accelerator) + " is the accelerator of " + action);
			    g_strfreev(actions);
		    }
		    Check(
		        g_action_group_has_action(G_ACTION_GROUP(application), "check-updates") != FALSE &&
		            g_action_group_has_action(G_ACTION_GROUP(application), "open-releases-page") ==
		                FALSE,
		        "an Official build has app.check-updates and no releases page");

		    // F5 rescans.
		    const auto scans = library.Stats().scans_started;
		    Check(KytyUi::KeyPress(root, GDK_KEY_F5), "F5 is handled");
		    KytyUi::WaitFor([&]() { return library.Stats().scans_applied > scans; }, 8000,
		                    "F5 to rescan the game folders");
		    // Ctrl+F focuses the search entry.
		    GtkWidget* search = KytyUi::FindByBuildableId(root, "search_entry");
		    Check(KytyUi::KeyPress(root, GDK_KEY_f, GDK_CONTROL_MASK), "Ctrl+F is handled");
		    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window));
		    Check(search != nullptr && focus != nullptr &&
		              (focus == search || gtk_widget_is_ancestor(focus, search) != FALSE),
		          "Ctrl+F focuses the search entry");
		    // The game keys below go to the game list (the search entry's text has its own Ctrl+.).
		    GtkColumnView* view = ListView(window);
		    if (view != nullptr) {
			    gtk_widget_grab_focus(GTK_WIDGET(view));
		    }
		    // Ctrl+G switches the view and back.
		    GtkWidget* views = KytyUi::FindByBuildableId(root, "views_stack");
		    Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK) && views != nullptr &&
		              g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(views)), "grid") == 0,
		          "Ctrl+G shows the grid view");
		    Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK) && views != nullptr &&
		              g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(views)), "list") == 0,
		          "Ctrl+G again shows the list view");
		    if (view != nullptr) {
			    gtk_widget_grab_focus(GTK_WIDGET(view));
		    }
		    // Ctrl+comma opens the global settings; Escape closes them.
		    Check(KytyUi::KeyPress(root, GDK_KEY_comma, GDK_CONTROL_MASK), "Ctrl+comma is handled");
		    GtkWindow* settings = nullptr;
		    KytyUi::WaitFor(
		        [&settings]() {
			        settings = MappedToplevel(KYTY_TYPE_SETTINGS_WINDOW);
			        return settings != nullptr;
		        },
		        5000, "Ctrl+comma to open the global settings");
		    if (settings != nullptr) {
			    KytyUi::WaitForPaintedFrame(GTK_WIDGET(settings));
			    KytyUi::KeyPressOn(GTK_WIDGET(settings), GDK_KEY_Escape);
			    KytyUi::WaitFor([settings]() { return !KytyUi::IsToplevel(settings); }, 3000,
			                    "Escape to close the settings");
		    }
		    // Ctrl+L shows the log and hides it again.
		    GtkWidget* log = KytyUi::FindByBuildableId(root, "log_area");
		    Check(KytyUi::KeyPress(root, GDK_KEY_l, GDK_CONTROL_MASK) && log != nullptr &&
		              gtk_widget_get_visible(log) != FALSE,
		          "Ctrl+L shows the log");
		    Check(KytyUi::KeyPress(root, GDK_KEY_l, GDK_CONTROL_MASK) && log != nullptr &&
		              gtk_widget_get_visible(log) == FALSE,
		          "Ctrl+L hides the log");
		    // Ctrl+S saves the log to the chosen file.
		    auto& chooser = ui.Services().file_chooser;
		    chooser.file_answers.push_back(log_file);
		    Check(KytyUi::KeyPress(root, GDK_KEY_s, GDK_CONTROL_MASK), "Ctrl+S is handled");
		    KytyUi::WaitFor(
		        [&]() {
			        std::error_code ec;
			        return fs::exists(log_file, ec);
		        },
		        3000, "Ctrl+S to write the log file");
		    Check(!chooser.calls.empty() && chooser.calls.back() == "SaveFile Save log | kyty.log",
		          "Ctrl+S asks where to save the log");
		    // Ctrl+U checks for updates (the loopback server answers 404: "Could not check").
		    KytyUi::WaitFor([&update]() { return update.CanCheck(); }, 8000,
		                    "the check at startup to end");
		    const auto checks = update.Stats().checks;
		    Check(KytyUi::KeyPress(root, GDK_KEY_u, GDK_CONTROL_MASK), "Ctrl+U is handled");
		    Check(update.Stats().checks == checks + 1, "Ctrl+U starts an update check");
		    if (GtkWindow* alert = KytyUi::FindAlert("Could not check for updates", 8000);
		        alert != nullptr) {
			    Check(KytyUi::Activate(AlertButton(alert, {"Close", "_Close"})), "close the alert");
			    KytyUi::WaitFor([alert]() { return !KytyUi::IsToplevel(alert); }, 3000,
			                    "the update alert to close");
		    }
		    // Ctrl+R runs the selected game, Ctrl+Period stops it.
		    Check(view != nullptr && FocusGameRow(window, view, "Alpha Game") != nullptr &&
		              context != nullptr && context->library.Selected() != nullptr &&
		              kyty_game_get_entry(context->library.Selected()).path == alpha,
		          "Alpha Game is selected");
		    Check(KytyUi::KeyPress(root, GDK_KEY_r, GDK_CONTROL_MASK), "Ctrl+R is handled");
		    KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                    "Ctrl+R to run the game");
		    Check(run.Stats().runs_started == 1, "Ctrl+R started one run");
		    Check(KytyUi::KeyPress(root, GDK_KEY_period, GDK_CONTROL_MASK),
		          "Ctrl+Period is handled");
		    KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                    "Ctrl+Period to stop the game");
		    Check(run.Stats().stop_requested_us > 0 && run.Stats().exits == 1,
		          "Ctrl+Period stopped the game");
		    // Ctrl+O opens the game's folder.
		    Check(KytyUi::KeyPress(root, GDK_KEY_o, GDK_CONTROL_MASK), "Ctrl+O is handled");
		    const auto& opened = ui.Services().desktop_launcher.calls;
		    KytyUi::WaitFor([&opened]() { return !opened.empty(); }, 3000,
		                    "Ctrl+O to open the folder");
		    Check(!opened.empty() && opened.back() == "OpenFolder " + alpha.string(),
		          "Ctrl+O opens Alpha Game's folder");
		    // Ctrl+E edits the game's config; Escape closes the editor.
		    Check(KytyUi::KeyPress(root, GDK_KEY_e, GDK_CONTROL_MASK), "Ctrl+E is handled");
		    settings = nullptr;
		    KytyUi::WaitFor(
		        [&settings]() {
			        settings = MappedToplevel(KYTY_TYPE_SETTINGS_WINDOW);
			        return settings != nullptr;
		        },
		        5000, "Ctrl+E to open the game's settings");
		    if (settings != nullptr) {
			    KytyUi::WaitForPaintedFrame(GTK_WIDGET(settings));
			    Check(KytyUi::AllText(GTK_WIDGET(settings)).find("Alpha Game") != std::string::npos,
			          "the editor is Alpha Game's");
			    KytyUi::KeyPressOn(GTK_WIDGET(settings), GDK_KEY_Escape);
			    KytyUi::WaitFor([settings]() { return !KytyUi::IsToplevel(settings); }, 3000,
			                    "Escape to close the game's settings");
		    }
		    // Ctrl+Q quits.
		    Check(KytyUi::KeyPress(root, GDK_KEY_q, GDK_CONTROL_MASK), "Ctrl+Q is handled");
	    },
	    kyty::updater::UpdateUiMode::Official);
	g_unsetenv("KYTY_UI_FAKE_ARGS");
	Check(status == 0, "Ctrl+Q ends the application with status 0");
}

KYTY_UI_CASE(keyboard_view_keys) {
	KytyUi::UiContext ui;
	const fs::path    alpha = WriteGamesWithCustomSettings(ui);
	if (alpha.empty()) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root    = GTK_WIDGET(window);
		auto*      context = kyty::gtk::ApplicationContext(application);
		auto&      library = kyty::gtk::MainWindowLibrary(window);
		auto&      run     = kyty::gtk::MainWindowRun(window);
		KytyUi::WaitFor([&library]() { return library.Stats().scans_applied >= 1; }, 8000,
		                "the first scan to be applied");
		GtkColumnView* view = ListView(window);
		if (!Check(context != nullptr && view != nullptr, "the main window has its list view")) {
			Quit(application);
			return;
		}
		// Tab and the arrow keys to Alpha Game's row, which the selection follows.
		const auto focus_alpha = [&]() {
			GtkWidget* row      = FocusGameRow(window, view, "Alpha Game");
			KytyGame*  selected = context->library.Selected();
			return Check(row != nullptr && selected != nullptr &&
			                 kyty_game_get_entry(selected).path == alpha,
			             "Alpha Game's row has the focus and is selected");
		};
		focus_alpha();

		// Enter runs the game (the fake Emulator exits at once).
		Check(KytyUi::KeyPress(root, GDK_KEY_Return), "Enter is handled");
		KytyUi::WaitFor([&run]() { return run.Stats().exits >= 1; }, 8000,
		                "Enter to run the game and the game to exit");
		Check(run.Stats().runs_started == 1, "Enter started one run");
		focus_alpha();

		// Delete asks to clear the game's config.
		Check(KytyUi::KeyPress(root, GDK_KEY_Delete), "Delete is handled");
		if (GtkWindow* alert = KytyUi::FindAlert("Clear the game config?"); alert != nullptr) {
			KytyUi::CloseAlert(alert, "Cancel");
		}
		focus_alpha();

		// Menu and Shift+F10 open the game menu.
		std::vector<GtkPopover*> popovers;
		for (GtkWidget* widget: KytyUi::FindAll(
		         root, [](GtkWidget* candidate) { return GTK_IS_POPOVER(candidate); })) {
			gtk_popover_set_autohide(GTK_POPOVER(widget), FALSE);
			popovers.push_back(GTK_POPOVER(widget));
		}
		const auto open_game_menu = [&]() -> GtkPopover* {
			for (GtkPopover* popover: popovers) {
				if (gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE &&
				    KytyUi::AllText(GTK_WIDGET(popover)).find("Run") != std::string::npos) {
					return popover;
				}
			}
			return nullptr;
		};
		for (const auto& [keyval, modifiers, name]:
		     {std::tuple<guint, GdkModifierType, const char*> {GDK_KEY_Menu, GdkModifierType(0),
		                                                       "Menu"},
		      std::tuple<guint, GdkModifierType, const char*> {GDK_KEY_F10, GDK_SHIFT_MASK,
		                                                       "Shift+F10"}}) {
			Check(KytyUi::KeyPress(root, keyval, modifiers), std::string(name) + " is handled");
			GtkPopover* menu = nullptr;
			KytyUi::WaitFor(
			    [&]() {
				    menu = open_game_menu();
				    return menu != nullptr;
			    },
			    2000, std::string(name) + " to open the game menu");
			if (menu != nullptr) {
				gtk_popover_popdown(menu);
				KytyUi::WaitFor(
				    [menu]() { return gtk_widget_get_mapped(GTK_WIDGET(menu)) == FALSE; }, 2000,
				    "the game menu to close");
			}
			focus_alpha();
		}

		// Escape in the search entry clears the search.
		GtkWidget* search = KytyUi::FindByBuildableId(root, "search_entry");
		gtk_editable_set_text(GTK_EDITABLE(search), "Alpha");
		KytyUi::WaitFor([context]() { return context->library.SearchText() == "Alpha"; }, 2000,
		                "the search text to reach the library");
		Check(KytyUi::KeyPress(root, GDK_KEY_f, GDK_CONTROL_MASK), "Ctrl+F focuses the search");
		Check(KytyUi::KeyPressOn(search, GDK_KEY_Escape), "Escape is handled in the search entry");
		KytyUi::WaitFor([context]() { return context->library.SearchText().empty(); }, 2000,
		                "Escape to clear the search");
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(search))).empty(),
		      "Escape clears the search entry");

		// Ctrl+U exists only in Official builds.
		Check(g_action_group_has_action(G_ACTION_GROUP(application), "check-updates") == FALSE,
		      "an Other build has no app.check-updates");
		focus_alpha();
		const guint before = KytyUi::ToplevelCount();
		Check(!KytyUi::KeyPress(root, GDK_KEY_u, GDK_CONTROL_MASK) &&
		          KytyUi::ToplevelCount() == before,
		      "Ctrl+U does nothing in an Other build");
		Quit(application);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(keyboard_sort) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([](KytyApplication* application, KytyMainWindow* window) {
		auto* context = kyty::gtk::ApplicationContext(application);
		auto& library = kyty::gtk::MainWindowLibrary(window);
		KytyUi::WaitFor([&library]() { return library.Stats().details_done >= 1; }, 8000,
		                "the games' details (sizes)");
		GtkColumnView* view    = ListView(window);
		GtkWidget*     primary = KytyUi::FindFirst(GTK_WIDGET(window), [](GtkWidget* widget) {
			return GTK_IS_MENU_BUTTON(widget) &&
			       gtk_menu_button_get_primary(GTK_MENU_BUTTON(widget)) != FALSE;
		});
		GtkPopover*    popover =
		    primary != nullptr ? gtk_menu_button_get_popover(GTK_MENU_BUTTON(primary)) : nullptr;
		if (!Check(context != nullptr && view != nullptr && popover != nullptr,
		           "the main window has its list view and the primary menu")) {
			Quit(application);
			return;
		}
		gtk_popover_set_autohide(popover, FALSE);
		auto&         model   = context->library;
		GActionGroup* actions = G_ACTION_GROUP(application);
		struct Choice {
			const char*  label;
			const char*  state;
			const char*  column_id;
			core::Column column;
		};
		const Choice choices[] = {
		    {"Serial", "serial", "serial", core::Column::TitleId},
		    {"Game Version", "version", "game-version", core::Column::GameVersion},
		    {"Firmware Version", "firmware", "firmware-version", core::Column::FirmwareVersion},
		    {"Size", "size", "size", core::Column::Size},
		    {"Path", "path", "path", core::Column::Path},
		    {"Status", "status", "status", core::Column::Status},
		    {"Comments", "comment", "comments", core::Column::Comment},
		    {"Game", "title", "game", core::Column::Title},
		};
		const auto check_sort = [&](const Choice& choice, bool descending, const std::string& how) {
			Check(model.SortColumn() == choice.column && model.SortDescending() == descending,
			      how + " sorts by " + choice.label + (descending ? ", descending" : ""));
			Check(SortedTitles(model) == ExpectedTitles(model, choice.column, descending),
			      how + ": the list is in the order of the column (core::SortRows)");
			auto*       sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(view));
			auto*       shown  = gtk_column_view_sorter_get_primary_sort_column(sorter);
			const char* id     = shown != nullptr ? gtk_column_view_column_get_id(shown) : nullptr;
			const auto  order  = gtk_column_view_sorter_get_primary_sort_order(sorter);
			Check(g_strcmp0(id, choice.column_id) == 0 &&
			          order == (descending ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING),
			      how + ": the header of " + choice.label + " shows the sort");
			Check(StateString(actions, "sort-column") == choice.state &&
			          StateString(actions, "sort-descending") == (descending ? "true" : "false"),
			      how + ": the menu shows " + choice.label +
			          (descending ? ", descending" : ", ascending"));
		};
		for (const auto& choice: choices) {
			if (ChooseSortItem(window, popover, choice.label)) {
				check_sort(choice, false, std::string("the menu's ") + choice.label);
			}
		}
		// Descending, then the menu with the sub menu open for the captures.
		if (ChooseSortItem(window, popover, "Descending")) {
			check_sort(choices[7], true, "the menu's Descending");
		}
		if (KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F10) &&
		    KytyUi::WaitFor(
		        [popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE; }, 2000,
		        "the primary menu")) {
			if (FocusItem(popover, "Sort by") &&
			    KytyUi::KeyPress(FocusIn(GTK_WIDGET(popover)), GDK_KEY_Return)) {
				KytyUi::Pump(300);
				CapturePopover(popover, "sort-menu");
			}
			gtk_popover_popdown(popover);
			KytyUi::WaitFor(
			    [popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) == FALSE; }, 2000,
			    "the primary menu to close");
		}
		// A column chosen while Descending is checked keeps the direction (D18: the menu's
		// Descending is a toggle of its own; a header click on another column sorts ascending).
		if (ChooseSortItem(window, popover, "Size")) {
			check_sort(choices[3], true, "the menu's Size with Descending checked");
		}
		// A header click (the column view's sort by a column) changes the menu's state too.
		gtk_column_view_sort_by_column(view, ColumnById(view, "serial"), GTK_SORT_ASCENDING);
		check_sort(choices[0], false, "a click on the Serial header");
		gtk_column_view_sort_by_column(view, ColumnById(view, "serial"), GTK_SORT_DESCENDING);
		check_sort(choices[0], true, "a second click on the Serial header");
		Quit(application);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(keyboard_binding_delete) {
	KytyUi::UiContext ui;
	GtkWindow*        parent = ui.ParentWindow();
	const auto*       type   = kyty::gtk::FindDialogType("input-mapping-window");
	if (!Check(type != nullptr, "input-mapping-window is registered")) {
		return;
	}
	const guint             before = KytyUi::ToplevelCount();
	kyty::gtk::DialogHandle handle = type->open(ui.Context(), parent, {});
	GtkWindow*              window = handle.window;
	if (!Check(window != nullptr, "the input mapping window opens") ||
	    !KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                     5000, "the input mapping window to be mapped")) {
		type->close(handle);
		return;
	}
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
	GtkWidget* stack = KytyUi::FindByType(GTK_WIDGET(window), ADW_TYPE_VIEW_STACK);
	for (const char* tab: {"keyboard", "gamepad"}) {
		if (stack != nullptr) {
			adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(stack), tab);
		}
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		// The mapped binding buttons that have a binding.
		auto buttons = KytyUi::FindAll(GTK_WIDGET(window), [](GtkWidget* widget) {
			return GTK_IS_BUTTON(widget) && gtk_widget_has_css_class(widget, "binding") != FALSE &&
			       gtk_widget_get_mapped(widget) != FALSE && KytyUi::WidgetText(widget) != "None";
		});
		if (!Check(buttons.size() >= 3, std::string(tab) + ": three bound controls")) {
			continue;
		}
		// Delete with the row's focus, KP_Delete with the button's focus.
		GtkWidget* row = gtk_widget_get_ancestor(buttons[0], ADW_TYPE_ACTION_ROW);
		Check(row != nullptr && gtk_widget_grab_focus(row) != FALSE &&
		          gtk_window_get_focus(window) == row,
		      std::string(tab) + ": the first binding's row has the focus");
		const std::string first = KytyUi::WidgetText(buttons[0]);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Delete), "Delete on the row is handled");
		Check(KytyUi::WidgetText(buttons[0]) == "None",
		      std::string(tab) + ": Delete on the row unmaps \"" + first + "\"");
		Check(gtk_widget_grab_focus(buttons[1]) != FALSE &&
		          gtk_window_get_focus(window) == buttons[1],
		      std::string(tab) + ": the second binding's button has the focus");
		const std::string second = KytyUi::WidgetText(buttons[1]);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_KP_Delete),
		      "KP_Delete on the button is handled");
		Check(KytyUi::WidgetText(buttons[1]) == "None",
		      std::string(tab) + ": KP_Delete on the button unmaps \"" + second + "\"");
		Check(KytyUi::WidgetText(buttons[2]) != "None", "the third binding stays");
		// Enter on a button still opens the capture dialog.
		gtk_widget_grab_focus(buttons[2]);
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Return),
		      "Enter on a binding is handled");
		GtkWindow* dialog = nullptr;
		KytyUi::WaitFor(
		    [&dialog]() {
			    dialog = MappedToplevel(KYTY_TYPE_BINDING_CAPTURE_DIALOG);
			    return dialog != nullptr;
		    },
		    3000, "Enter to open the binding capture dialog");
		if (dialog != nullptr) {
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(dialog));
			KytyUi::KeyPress(GTK_WIDGET(dialog), GDK_KEY_Escape);
			KytyUi::WaitFor([dialog]() { return !KytyUi::IsToplevel(dialog); }, 3000,
			                "Escape to close the capture dialog");
		}
		Check(KytyUi::WidgetText(buttons[2]) != "None", "the cancelled capture changes nothing");
	}
	type->close(handle);
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
	                "the input mapping window to close");
}
