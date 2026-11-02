// UI tests of the launcher's start (task 22.2; Requirements 6.8, 6.13, 7.3; phase 7 plan D13, D14,
// D20), each on Wayland and on X11 (the CTest of the backend):
// - startup_within_5s: the real KytyApplication (D13) with no Game_Folder maps its main window
//   within 5 s of the process start (field 22 of /proc/self/stat against CLOCK_BOOTTIME, D14),
//   through the session's own display protocol, with the empty state and no warning or critical
//   (the harness's log writer fails the case on any).
// - startup_empty_state: the empty state is an AdwStatusPage whose "Add game folder" button is
//   reached with Tab and activates app.add-game-folder with Enter (the global settings on their
//   Folders page); the grid toggle and the icon width slider are hidden there, also after Ctrl+G
//   (W4-03, D20); a folder chosen through the scripted chooser and saved shows the library, and
//   the grid toggle with it.
// - startup_with_games: the fixture games are listed sorted by title, ascending, with the Game
//   column's header showing it (Requirement 7.3, LFI-06).
// - startup_window_icon: the default window icon name is the App_ID, the icon theme resolves it
//   from the launcher's GResource bundle, whose two icon files are the Main_Branch artwork byte for
//   byte (Requirement 6.13), and the main window and every registry window use the default icon.

#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/settingsWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <gtk/gtk.h>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using kyty::gtk::GObjectPtr;
using KytyUi::Check;

// The sha256 of the Main_Branch artwork (c5016d94: src/launcher/icons/kyty.svg and kyty-small.svg,
// read once from _Build/main-ref when this test was written; Requirement 6.13 asks for it
// unchanged).
constexpr const char* kKytySvgSha256 =
    "76a381bd4ccf3801d1c850c3bf9903e6d11a480eb6eb5938cdab3d8e949fe857";
constexpr const char* kKytySmallSvgSha256 =
    "e52251dceb20f231fd2f5d3fb94d3e32885b6fae9e25383a43e3f92ec6162138";

double BootSeconds() {
	timespec now {};
	clock_gettime(CLOCK_BOOTTIME, &now);
	return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

// When this process started, in seconds since boot (proc_pid_stat(5): field 22, starttime, in
// clock ticks); a negative value after a Fail.
double ProcessStartSeconds() {
	std::ifstream     in("/proc/self/stat");
	const std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const auto        comm_end = stat.rfind(')');
	if (comm_end == std::string::npos) {
		KytyUi::Fail("cannot parse /proc/self/stat");
		return -1;
	}
	// After the command name come the fields from 3 (state) on; starttime is field 22.
	std::istringstream fields(stat.substr(comm_end + 1));
	std::string        field;
	for (int number = 3; number <= 22 && fields >> field; number++) {
	}
	const double ticks = std::strtod(field.c_str(), nullptr);
	return ticks / static_cast<double>(sysconf(_SC_CLK_TCK));
}

struct FirstMap {
	double seconds = -1; // CLOCK_BOOTTIME of the main window's first map
};

gboolean OnMapEmission(GSignalInvocationHint* /*hint*/, guint count, const GValue* values,
                       gpointer data) {
	auto* first = static_cast<FirstMap*>(data);
	if (count > 0 && first->seconds < 0 && KYTY_IS_MAIN_WINDOW(g_value_get_object(&values[0]))) {
		first->seconds = BootSeconds();
	}
	return TRUE;
}

// The display is the backend the CTest asked for (GDK_BACKEND from tests/ui/with-display.sh).
void CheckDisplayProtocol() {
	const char*       backend = g_getenv("GDK_BACKEND");
	const std::string type    = G_OBJECT_TYPE_NAME(gdk_display_get_default());
	if (g_strcmp0(backend, "wayland") == 0) {
		Check(type == "GdkWaylandDisplay", "a Wayland session uses Wayland (" + type + ")");
	} else if (g_strcmp0(backend, "x11") == 0) {
		Check(type == "GdkX11Display", "an X11 session uses X11 (" + type + ")");
	} else {
		KytyUi::Fail("GDK_BACKEND is neither wayland nor x11");
	}
}

std::string VisibleChild(GtkWidget* root, const char* stack_id) {
	GtkWidget*  stack = KytyUi::FindByBuildableId(root, stack_id);
	const char* name  = stack != nullptr && GTK_IS_STACK(stack)
	                        ? gtk_stack_get_visible_child_name(GTK_STACK(stack))
	                        : nullptr;
	return name != nullptr ? name : "";
}

GtkWidget* FindButton(GtkWidget* root, std::string_view label) {
	return KytyUi::FindFirst(root, [label](GtkWidget* widget) {
		return GTK_IS_BUTTON(widget) && KytyUi::WidgetText(widget) == label;
	});
}

bool Visible(GtkWidget* root, const char* id) {
	GtkWidget* widget = KytyUi::FindByBuildableId(root, id);
	return widget != nullptr && gtk_widget_get_visible(widget) != FALSE;
}

std::string Sha256(GBytes* bytes) {
	const kyty::gtk::GCharPtr digest(g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes));
	return std::string(digest.View());
}

// The settings window that is mapped, or nullptr.
GtkWindow* MappedSettingsWindow() {
	GListModel* toplevels = gtk_window_get_toplevels();
	for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
		auto window = GObjectPtr<GtkWindow>::Adopt(
		    static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
		if (KYTY_IS_SETTINGS_WINDOW(window.get()) &&
		    gtk_widget_get_mapped(GTK_WIDGET(window.get())) != FALSE) {
			return window.get();
		}
	}
	return nullptr;
}

} // namespace

KYTY_UI_CASE(startup_within_5s) {
	const double start = ProcessStartSeconds();
	FirstMap     first;
	// The signal exists once the class does.
	gpointer     widget_class = g_type_class_ref(GTK_TYPE_WIDGET);
	const guint  map_signal   = g_signal_lookup("map", GTK_TYPE_WIDGET);
	const gulong hook = g_signal_add_emission_hook(map_signal, 0, &OnMapEmission, &first, nullptr);
	KytyUi::UiContext ui;
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		const double seconds = first.seconds - start;
		std::printf("%s: process start to the main window's first map: %.3f s (limit 5 s, %s)\n",
		            KytyUi::CaseName().c_str(), seconds, g_getenv("GDK_BACKEND"));
		Check(first.seconds > 0 && start > 0, "the first map and the process start are known");
		Check(seconds <= 5.0, "the main window is shown within 5 s of the process start (" +
		                          std::to_string(seconds) + " s)");
		CheckDisplayProtocol();
		Check(VisibleChild(GTK_WIDGET(window), "content_stack") == "empty",
		      "without a Game_Folder the main window shows the empty state");
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	g_signal_remove_emission_hook(map_signal, hook);
	g_type_class_unref(widget_class);
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(startup_empty_state) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* root = GTK_WIDGET(window);
		auto*      page = KytyUi::FindByType(root, ADW_TYPE_STATUS_PAGE);
		Check(page != nullptr && gtk_widget_get_mapped(page) != FALSE &&
		          VisibleChild(root, "content_stack") == "empty",
		      "the empty state (an AdwStatusPage) is shown");
		Check(page != nullptr && KytyUi::AllText(page).find("No game folders") != std::string::npos,
		      "the empty state says that there are no game folders");
		// W4-03 (D20): no grid toggle and no slider on the empty state, also after Ctrl+G.
		Check(!Visible(root, "grid_button") && !Visible(root, "icon_width_scale"),
		      "the grid toggle and the icon width slider are hidden on the empty state");
		Check(KytyUi::Capture(GTK_WINDOW(window), "empty-state"), "the empty state renders");
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK),
		      "Ctrl+G (app.toggle-grid) is enabled on the empty state");
		Check(!Visible(root, "grid_button") && !Visible(root, "icon_width_scale"),
		      "after Ctrl+G the grid toggle and the slider stay hidden");
		KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK);

		// "Add game folder" by keyboard: Tab reaches it, Enter activates app.add-game-folder.
		GtkWidget* add = FindButton(root, "Add game folder");
		if (!Check(add != nullptr && GTK_IS_ACTIONABLE(add) &&
		               g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(add)),
		                         "app.add-game-folder") == 0,
		           "the empty state has the \"Add game folder\" button for app.add-game-folder")) {
			g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
			return;
		}
		for (int i = 0; i < 40 && gtk_window_get_focus(GTK_WINDOW(window)) != add; i++) {
			KytyUi::MoveFocus(GTK_WINDOW(window), GTK_DIR_TAB_FORWARD);
		}
		Check(gtk_window_get_focus(GTK_WINDOW(window)) == add,
		      "Tab reaches the \"Add game folder\" button");
		Check(KytyUi::KeyPress(root, GDK_KEY_Return), "Enter activates the button");
		GtkWindow* settings = nullptr;
		KytyUi::WaitFor(
		    [&settings]() {
			    settings = MappedSettingsWindow();
			    return settings != nullptr;
		    },
		    5000, "the global settings window");
		if (settings != nullptr) {
			GtkWidget* stack = KytyUi::FindByType(GTK_WIDGET(settings), ADW_TYPE_VIEW_STACK);
			Check(stack != nullptr &&
			          g_strcmp0(adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(stack)),
			                    "folders") == 0,
			      "app.add-game-folder opens the settings on their Folders page");
			// A folder through the scripted chooser, then Save.
			ui.Services().file_chooser.folder_answers.push_back({games});
			Check(KytyUi::Activate(FindButton(GTK_WIDGET(settings), "Add folders…")),
			      "\"Add folders…\" asks the file chooser");
			KytyUi::WaitFor(
			    [&]() {
				    return KytyUi::AllText(GTK_WIDGET(settings)).find(games.string()) !=
				           std::string::npos;
			    },
			    3000, "the chosen folder in the settings window's folder list");
			Check(!ui.Services().file_chooser.calls.empty() &&
			          ui.Services().file_chooser.calls.back() == "SelectFolders Add game folders",
			      "the chooser was asked for game folders");
			Check(KytyUi::Activate(FindButton(GTK_WIDGET(settings), "Save")), "Save the settings");
			KytyUi::WaitFor([&]() { return !KytyUi::IsToplevel(settings); }, 3000,
			                "the settings window to close after Save");
		}
		auto* context = kyty::gtk::ApplicationContext(application);
		KytyUi::WaitFor(
		    [&]() {
			    return VisibleChild(root, "content_stack") == "library" && context != nullptr &&
			           g_list_model_get_n_items(context->library.Games()) == 5;
		    },
		    8000, "the library with the 5 fixture games after the folder was saved");
		Check(Visible(root, "grid_button") && !Visible(root, "icon_width_scale"),
		      "with the library the grid toggle is back (the slider only in the grid view)");
		// With the library, Ctrl+G shows the grid view and the slider, and Ctrl+G again the list
		// view without it (D20 keeps the slider to the grid view).
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK), "Ctrl+G is handled");
		Check(VisibleChild(root, "views_stack") == "grid" && Visible(root, "grid_button") &&
		          Visible(root, "icon_width_scale"),
		      "in the grid view of the library the grid toggle and the slider are shown");
		Check(KytyUi::KeyPress(root, GDK_KEY_g, GDK_CONTROL_MASK), "Ctrl+G again is handled");
		Check(VisibleChild(root, "views_stack") == "list" && Visible(root, "grid_button") &&
		          !Visible(root, "icon_width_scale"),
		      "back in the list view the slider is hidden again");
		Check(KytyUi::Capture(GTK_WINDOW(window), "library"), "the library renders");
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(startup_with_games) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto& library = kyty::gtk::MainWindowLibrary(window);
		KytyUi::WaitFor([&library]() { return library.Stats().scans_applied >= 1; }, 8000,
		                "the first scan to be applied");
		auto* context = kyty::gtk::ApplicationContext(application);
		if (!Check(context != nullptr, "the application has its context")) {
			return;
		}
		Check(context->library.SortColumn() == kyty::core::Column::Title &&
		          !context->library.SortDescending(),
		      "the library is sorted by title, ascending");
		const std::vector<std::string> expected = {
		    "Alpha Game", "Beta Quest", "Broken Metadata", "Gamma Archive",
		    // "Ünïcöde" in UTF-8, as uiFixtures.cpp writes it.
		    "\xc3\x9c"
		    "n\xc3\xaf"
		    "c\xc3\xb6"
		    "de \"Odd\" Game"};
		std::vector<std::string> shown;
		GListModel*              sorted = context->library.Sorted();
		for (guint i = 0; i < g_list_model_get_n_items(sorted); i++) {
			auto game = GObjectPtr<KytyGame>::Adopt(
			    static_cast<KytyGame*>(g_list_model_get_item(sorted, i)));
			shown.push_back(kyty_game_get_entry(game.get()).title_name);
		}
		std::string order;
		for (const auto& title: shown) {
			order += (order.empty() ? "" : " | ") + title;
		}
		std::printf("%s: list order: %s\n", KytyUi::CaseName().c_str(), order.c_str());
		Check(shown == expected, "the games are listed by title: " + order);
		auto* view = GTK_COLUMN_VIEW(KytyUi::FindByType(GTK_WIDGET(window), GTK_TYPE_COLUMN_VIEW));
		Check(view != nullptr && gtk_widget_get_mapped(GTK_WIDGET(view)) != FALSE &&
		          gtk_column_view_get_model(view) ==
		              GTK_SELECTION_MODEL(context->library.Selection()),
		      "the list view shows the library");
		if (view != nullptr) {
			auto*       sorter  = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(view));
			auto*       primary = gtk_column_view_sorter_get_primary_sort_column(sorter);
			const char* id = primary != nullptr ? gtk_column_view_column_get_id(primary) : nullptr;
			Check(g_strcmp0(id, "game") == 0 &&
			          gtk_column_view_sorter_get_primary_sort_order(sorter) == GTK_SORT_ASCENDING,
			      "the Game column's header shows the ascending sort");
		}
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(startup_window_icon) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const std::string app_id = kyty::gtk::kApplicationId;
	const std::string base   = std::string(kyty::gtk::kResourceBasePath) + "/icons/";
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		Check(g_strcmp0(gtk_window_get_default_icon_name(), app_id.c_str()) == 0,
		      "the default window icon name is the App_ID");
		GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
		Check(gtk_icon_theme_has_icon(theme, app_id.c_str()) != FALSE,
		      "the icon theme has the App_ID icon");
		for (const int size: {16, 24, 32, 48, 64, 128}) {
			auto icon = GObjectPtr<GtkIconPaintable>::Adopt(gtk_icon_theme_lookup_icon(
			    theme, app_id.c_str(), nullptr, size, 1, GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0)));
			auto file =
			    GObjectPtr<GFile>::Adopt(icon ? gtk_icon_paintable_get_file(icon.get()) : nullptr);
			const kyty::gtk::GCharPtr uri(file ? g_file_get_uri(file.get()) : nullptr);
			const std::string         text(uri.View());
			Check(text.rfind("resource://" + base, 0) == 0,
			      "the " + std::to_string(size) + " px icon comes from the GResource bundle (" +
			          text + ")");
		}
		struct Resource {
			std::string path;
			const char* sha256;
		};
		std::vector<Resource> resources = {
		    {base + "scalable/apps/" + app_id + ".svg", kKytySvgSha256}};
		for (const char* size: {"16x16", "24x24", "32x32", "48x48", "64x64"}) {
			resources.push_back({base + size + "/apps/" + app_id + ".svg", kKytySmallSvgSha256});
		}
		for (const auto& resource: resources) {
			kyty::gtk::GErrorPtr error;
			GBytes* bytes = g_resources_lookup_data(resource.path.c_str(),
			                                        G_RESOURCE_LOOKUP_FLAGS_NONE, error.Out());
			Check(bytes != nullptr && Sha256(bytes) == resource.sha256,
			      resource.path + " is the Main_Branch artwork byte for byte");
			if (bytes != nullptr) {
				g_bytes_unref(bytes);
			}
		}
		Check(gtk_window_get_icon_name(GTK_WINDOW(window)) == nullptr,
		      "the main window uses the default icon");

		// Every registry window (the main window is the application's own, above).
		auto* context = kyty::gtk::ApplicationContext(application);
		auto& library = kyty::gtk::MainWindowLibrary(window);
		KytyUi::WaitFor([&library]() { return library.Stats().scans_applied >= 1; }, 8000,
		                "the first scan to be applied");
		kyty::core::GameEntry entry;
		if (context != nullptr && g_list_model_get_n_items(context->library.Sorted()) > 0) {
			auto game = GObjectPtr<KytyGame>::Adopt(
			    static_cast<KytyGame*>(g_list_model_get_item(context->library.Sorted(), 0)));
			entry = kyty_game_get_entry(game.get());
		}
		for (const auto& type: kyty::gtk::DialogTypes()) {
			if (context == nullptr || type.id == "main-window") {
				continue;
			}
			const guint           before = KytyUi::ToplevelCount();
			kyty::gtk::DialogArgs args;
			args.entry                     = &entry;
			kyty::gtk::DialogHandle handle = type.open(*context, GTK_WINDOW(window), args);
			const std::string       id(type.id);
			if (Check(handle.window != nullptr, id + " opens a window") &&
			    KytyUi::WaitFor(
			        [&handle]() {
				        return gtk_widget_get_mapped(GTK_WIDGET(handle.window)) != FALSE;
			        },
			        5000, id + " to be mapped")) {
				const char* name = gtk_window_get_icon_name(handle.window);
				Check(name == nullptr || app_id == name,
				      id + " uses the application icon (" +
				          std::string(name != nullptr ? name : "default") + ")");
			}
			type.close(handle);
			KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
			                id + " to close");
		}
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}
