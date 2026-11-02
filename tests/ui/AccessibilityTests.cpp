// UI tests of the accessibility tree (task 22.2; Requirement 6.10; phase 7 plan D9), each on
// Wayland and on X11, under GTK's test accessibility backend (GTK_A11Y=test): one a11y_<window>
// case per top-level window and dialog of the launcher, and one for the alerts of every helper.
// Each walks the window's widget tree in every state the case shows (pages, tabs, views, menus) and
// checks that every interactive control there (KytyUi::InteractiveControls: buttons, toggle
// buttons, check and radio buttons, switches, combo boxes and drop-downs, text and search entries,
// sliders, spin buttons, links, tabs, menu items, and list, row, or grid items) has a non-empty
// accessible name: its LABEL property, the text of its LABELLED_BY widgets, or its own visible text
// (D9); the internal button of a GtkMenuButton or GtkDropDown has the composite's name. A failure
// names the widget's role and its path in the widget tree. The windows that show a game's content
// (trophy window, trophy overview, add-ons window, cheats window) are walked with the content of
// KytyUi::UiContext::WriteGameContent loaded (an earned, a not earned, and a hidden trophy with its
// Reveal button, an add-on, two local cheats, the remote file list), and each walk must contain
// the controls of that content (KytyUi::GameContentControls).

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace {

using kyty::gtk::GObjectPtr;
using KytyUi::Check;

// Every interactive control below root has a name; where is the state shown ("settings-window,
// page folders"). Returns the controls.
std::vector<GtkWidget*> CheckNames(GtkWidget* root, const std::string& where) {
	// The widgets that were just shown get their size and their accessible state in the next
	// frame.
	KytyUi::WaitForPaintedFrame(root);
	KytyUi::Pump(50);
	const auto controls = KytyUi::InteractiveControls(root);
	int        unnamed  = 0;
	for (GtkWidget* control: controls) {
		if (KytyUi::ControlName(control).empty()) {
			unnamed++;
			KytyUi::Fail(where + ": a " + KytyUi::RoleName(control) +
			             " without an accessible name: " + KytyUi::WidgetPath(control));
		}
	}
	std::printf("%s: %s: %zu interactive controls, %d without a name\n", KytyUi::CaseName().c_str(),
	            where.c_str(), controls.size(), unnamed);
	Check(!controls.empty(), where + " has interactive controls");
	Check(KytyUi::AtContextType(root) == "GtkTestATContext",
	      where + " uses GTK's test accessibility backend");
	return controls;
}

// Every page of every AdwViewStack in window in turn, then the first page again. The content
// controls of each page are among the controls of its walk, and every page they name is walked.
void CheckEveryPage(GtkWindow* window, const std::string& what,
                    const std::vector<KytyUi::ContentControl>& content = {}) {
	std::vector<std::string> walked; // the pages checked

	const auto check_page = [&](const std::string& page, const std::string& where) {
		const auto controls = CheckNames(GTK_WIDGET(window), where);
		walked.push_back(page);
		for (const auto& kind: content) {
			if (kind.page == page) {
				const bool found = std::any_of(controls.begin(), controls.end(), kind.match);
				std::printf("%s: %s: %s %s\n", KytyUi::CaseName().c_str(), where.c_str(),
				            kind.what.c_str(), found ? "checked" : "missing");
				Check(found, where + " shows " + kind.what);
			}
		}
	};
	const auto check_walked = [&]() {
		for (const auto& kind: content) {
			Check(std::find(walked.begin(), walked.end(), kind.page) != walked.end(),
			      what + ": the page \"" + kind.page + "\" of " + kind.what + " is walked");
		}
	};
	const auto stacks = KytyUi::FindAll(GTK_WIDGET(window), [](GtkWidget* widget) {
		return ADW_IS_VIEW_STACK(widget) && gtk_widget_get_mapped(widget) != FALSE;
	});
	if (stacks.empty()) {
		check_page("", what);
		check_walked();
		return;
	}
	for (GtkWidget* widget: stacks) {
		auto*       stack = ADW_VIEW_STACK(widget);
		const auto  first = std::string(adw_view_stack_get_visible_child_name(stack));
		auto        pages = GObjectPtr<GtkSelectionModel>::Adopt(adw_view_stack_get_pages(stack));
		const guint count = g_list_model_get_n_items(G_LIST_MODEL(pages.get()));
		for (guint i = 0; i < count; i++) {
			auto page = GObjectPtr<AdwViewStackPage>::Adopt(static_cast<AdwViewStackPage*>(
			    g_list_model_get_item(G_LIST_MODEL(pages.get()), i)));
			if (adw_view_stack_page_get_visible(page.get()) == FALSE) {
				continue;
			}
			const char*       name = adw_view_stack_page_get_name(page.get());
			const std::string page_name(name != nullptr ? name : "?");
			adw_view_stack_set_visible_child_name(stack, name);
			check_page(page_name, what + ", page " + page_name);
		}
		adw_view_stack_set_visible_child_name(stack, first.c_str());
	}
	check_walked();
}

// The fixture games in the context's library (the windows over the library read it) and the
// entry of Alpha Game (PPSA01001, a directory game); with_content, also the content of Alpha Game
// (UiContext::WriteGameContent).
kyty::core::GameEntry FillLibrary(KytyUi::UiContext& ui, bool with_content) {
	const auto games = ui.WriteFixtureGames();
	Check(!games.empty(), "the fixture games are written");
	if (with_content) {
		ui.WriteGameContent(games);
	}
	std::vector<kyty::core::GameEntry> entries;
	kyty::core::GameEntry              alpha;
	alpha.path       = games / "Alpha Game";
	alpha.title_name = "Alpha Game";
	alpha.title_id   = "PPSA01001";
	entries.push_back(alpha);
	kyty::core::GameEntry beta;
	beta.path       = games / "Beta" / "Beta Quest";
	beta.title_name = "Beta Quest";
	beta.title_id   = "PPSA01002";
	entries.push_back(beta);
	ui.Context().library.Replace(entries);
	return alpha;
}

// Opens the registry's id (for the entry, when given), checks every page, and closes it. A window
// with content controls (KytyUi::GameContentControls) gets Alpha Game's content and is checked
// once all of it is loaded.
void CheckRegistryWindow(const char* id, bool with_entry) {
	KytyUi::UiContext     ui;
	GtkWindow*            parent  = ui.ParentWindow();
	const auto            content = KytyUi::GameContentControls(id);
	kyty::core::GameEntry entry   = FillLibrary(ui, !content.empty());
	const auto*           type    = kyty::gtk::FindDialogType(id);
	if (!Check(type != nullptr, std::string(id) + " is registered")) {
		return;
	}
	kyty::gtk::DialogArgs args;
	if (with_entry) {
		args.entry = &entry;
	}
	const guint             before = KytyUi::ToplevelCount();
	kyty::gtk::DialogHandle handle = type->open(ui.Context(), parent, args);
	if (Check(handle.window != nullptr, std::string(id) + " opens a window") &&
	    KytyUi::WaitFor(
	        [&handle]() { return gtk_widget_get_mapped(GTK_WIDGET(handle.window)) != FALSE; }, 5000,
	        std::string(id) + " to be mapped")) {
		// Content that loads in the background (trophies, add-ons, cheat files) first.
		GtkWidget* root = GTK_WIDGET(handle.window);
		KytyUi::WaitFor(
		    [&content, root]() {
			    return std::all_of(content.begin(), content.end(), [root](const auto& kind) {
				    return KytyUi::FindFirst(root, kind.match) != nullptr;
			    });
		    },
		    5000, std::string(id) + " to show the content of Alpha Game");
		KytyUi::Pump(500);
		CheckEveryPage(handle.window, id, content);
	}
	type->close(handle);
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
	                std::string(id) + " to close");
}

} // namespace

KYTY_UI_CASE(a11y_main_window) {
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
		GtkWidget* root = GTK_WIDGET(window);
		CheckNames(root, "main window, list view");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-log", nullptr);
		CheckNames(root, "main window, list view with the log");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-grid", nullptr);
		CheckNames(root, "main window, grid view with the log");
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-grid", nullptr);
		g_action_group_activate_action(G_ACTION_GROUP(application), "toggle-log", nullptr);
		// The header bar's menus.
		for (GtkWidget* button: KytyUi::FindAll(root, [](GtkWidget* widget) {
			     return GTK_IS_MENU_BUTTON(widget) && gtk_widget_get_mapped(widget) != FALSE;
		     })) {
			const std::string name    = KytyUi::AccessibleName(button);
			GtkPopover*       popover = KytyUi::OpenPopover(GTK_MENU_BUTTON(button));
			if (popover != nullptr) {
				CheckNames(GTK_WIDGET(popover), "main window, the menu of \"" + name + "\"");
				gtk_popover_popdown(popover);
				KytyUi::WaitFor(
				    [popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) == FALSE; },
				    2000, "the menu to close");
			}
		}
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(a11y_main_window_empty) {
	KytyUi::UiContext ui;
	ui.ServeCompatibility();
	const int status = ui.RunApplication([](KytyApplication* application, KytyMainWindow* window) {
		CheckNames(GTK_WIDGET(window), "main window, empty state");
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}

KYTY_UI_CASE(a11y_settings_window) {
	CheckRegistryWindow("settings-window", false);
}

KYTY_UI_CASE(a11y_game_settings_window) {
	CheckRegistryWindow("game-settings-window", true);
}

KYTY_UI_CASE(a11y_game_picker_dialog) {
	CheckRegistryWindow("game-picker-dialog", false);
}

KYTY_UI_CASE(a11y_input_mapping_window) {
	CheckRegistryWindow("input-mapping-window", false);
}

KYTY_UI_CASE(a11y_game_input_mapping_window) {
	CheckRegistryWindow("input-mapping-window", true);
}

KYTY_UI_CASE(a11y_binding_capture_dialog) {
	CheckRegistryWindow("binding-capture-dialog", false);
}

KYTY_UI_CASE(a11y_trophy_window) {
	CheckRegistryWindow("trophy-window", true);
}

KYTY_UI_CASE(a11y_trophy_details_dialog) {
	CheckRegistryWindow("trophy-details-dialog", false);
}

KYTY_UI_CASE(a11y_trophy_overview_window) {
	CheckRegistryWindow("trophy-overview-window", false);
}

KYTY_UI_CASE(a11y_additional_content_window) {
	CheckRegistryWindow("additional-content-window", true);
}

KYTY_UI_CASE(a11y_compression_dialog) {
	CheckRegistryWindow("compression-dialog", true);
}

KYTY_UI_CASE(a11y_cheats_window) {
	CheckRegistryWindow("cheats-window", true);
}

KYTY_UI_CASE(a11y_update_progress_window) {
	CheckRegistryWindow("update-progress-window", false);
}

// One alert of each helper (alerts.h Confirm, ShowError, Choose) and every other alert of the
// registry.
KYTY_UI_CASE(a11y_alerts) {
	for (const char* id: {"confirmation-alert", "error-alert", "update-offer-alert",
	                      "update-result-alert", "clear-config-alert", "remove-save-data-alert",
	                      "save-data-failures-alert", "import-settings-alert"}) {
		CheckRegistryWindow(id, true);
	}
}
