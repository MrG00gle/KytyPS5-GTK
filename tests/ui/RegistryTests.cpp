// UI tests of the dialog registry (task 22.3; Requirement 17.10; phase 7 plan D17, D22, D33; HC
// P7-HC-02, P7-HC-13), each on Wayland and on X11:
// - registry_<id> (one case per registry id, "-" written "_"): the registry's open function makes a
//   window transient for a mapped parent (the main window is a toplevel of its own) over a context
//   with the fixture games in the library (an entry-bound id gets Alpha Game; the content windows
//   get Alpha Game's content, UiContext::WriteGameContent); the window is mapped, draws something
//   (with --capture-dir: <case>-<id>-{light,dark}.png), and has an accessible title or, for an
//   alert, its heading; it closes through the registry's close function (an alert first through
//   its own button, plan D17), and the toplevel count is back where it was.
// - registry_complete: every id of DialogTypes() has its registry_<id> case in this harness, and
//   every registry_<name> case except the three below names a registered id.
// - registry_alert_already_cancelled (P7-HC-02 on a display): with a cancellable that is already
//   cancelled, Confirm, Choose, ShowError, ShowErrorList, and the launcher's alerts built on them
//   open nothing next to a mapped parent and never call back.
// - registry_save_data_failures_scrolls (W3, plan D22): 200 save data failures fit the 1280x800
//   screen (the window is at most 600 pixels high) in a list that scrolls; the window closes with
//   its Close button, with Escape, when its cancellable is cancelled, and with its parent.

#include "launcher/core/gameMetadata.h"
#include "launcher/core/saveData.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gameActions.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/updateUi.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;
using KytyUi::Check;

// What a registry case knows about its id: whether it needs a game, and for an alert its heading
// and the button that closes it (save-data-failures-alert, an AdwWindow, is closed by the
// registry's close function, which uses its Close button).
struct RegistryEntry {
	const char* id;
	bool        with_entry;
	const char* heading      = nullptr; // alerts only
	const char* close_button = nullptr; // alerts closed with a button first
};

const RegistryEntry kEntries[] = {
    {"confirmation-alert", false, "Do you want to continue?", "Cancel"},
    {"error-alert", false, "The action failed", "Close"},
    {"settings-window", false},
    {"game-settings-window", true},
    {"game-picker-dialog", false},
    {"main-window", false},
    {"input-mapping-window", false},
    {"binding-capture-dialog", false},
    {"trophy-window", true},
    {"trophy-details-dialog", false},
    {"trophy-overview-window", false},
    {"additional-content-window", true},
    {"compression-dialog", true},
    {"cheats-window", true},
    {"update-offer-alert", false, "Version 2026.04.01-def5678 is available", "Cancel"},
    {"update-progress-window", false},
    {"update-result-alert", false, "Update installed", "Close"},
    {"clear-config-alert", true, "Clear the game config?", "Cancel"},
    {"remove-save-data-alert", true, "Remove the save data of Alpha Game?", "Cancel"},
    {"save-data-failures-alert", true, "Some save data of Alpha Game could not be removed"},
    {"import-settings-alert", true, "Import the config of another game?", "Cancel"},
};

// The cases of this file that are not one id's.
const char* const kOtherCases[] = {"registry_complete", "registry_alert_already_cancelled",
                                   "registry_save_data_failures_scrolls"};

std::string CaseFor(std::string_view id) {
	std::string name = "registry_" + std::string(id);
	std::replace(name.begin(), name.end(), '-', '_');
	return name;
}

const RegistryEntry* FindEntry(std::string_view id) {
	for (const auto& entry: kEntries) {
		if (id == entry.id) {
			return &entry;
		}
	}
	return nullptr;
}

core::GameEntry AlphaEntry(const fs::path& games) {
	core::GameEntry alpha;
	alpha.path       = games / "Alpha Game";
	alpha.title_name = "Alpha Game";
	alpha.title_id   = "PPSA01001";
	return alpha;
}

// The button of window whose label is one of labels ("Close" and "_Close": GTK's alert button and
// the launcher's mnemonic one), or nullptr.
GtkWidget* Button(GtkWindow* window, std::initializer_list<std::string_view> labels) {
	return KytyUi::FindFirst(GTK_WIDGET(window), [labels](GtkWidget* widget) {
		const std::string text = KytyUi::WidgetText(widget);
		return GTK_IS_BUTTON(widget) &&
		       std::find(labels.begin(), labels.end(), text) != labels.end();
	});
}

// Clicks the button (by its label) and waits until window is gone.
bool CloseWith(GtkWindow* window, std::string_view label) {
	GtkWidget* button =
	    label == "Close" ? Button(window, {"Close", "_Close"}) : Button(window, {label});
	if (!Check(button != nullptr, "the window has a button \"" + std::string(label) +
	                                  "\"; its text: " + KytyUi::AllText(GTK_WIDGET(window)))) {
		return false;
	}
	g_signal_emit_by_name(button, "clicked");
	return KytyUi::WaitFor([window]() { return !KytyUi::IsToplevel(window); }, 3000,
	                       "the window to close after \"" + std::string(label) + "\"");
}

// The registry case of id (see the comment at the top).
void CheckRegistryType(const char* id) {
	const RegistryEntry* entry = FindEntry(id);
	const auto*          type  = kyty::gtk::FindDialogType(id);
	if (!Check(entry != nullptr && type != nullptr, std::string(id) + " is registered")) {
		return;
	}
	KytyUi::UiContext ui;
	GtkWindow*        parent = ui.ParentWindow();
	const auto        games  = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return;
	}
	const auto content = KytyUi::GameContentControls(id);
	if (!content.empty()) {
		ui.WriteGameContent(games);
	}
	const bool main_window = std::string_view(id) == "main-window";
	if (main_window) {
		ui.StoreGameFolders({games});
	}
	ui.ServeCompatibility();
	core::GameEntry              alpha = AlphaEntry(games);
	std::vector<core::GameEntry> library {alpha};
	core::GameEntry              beta;
	beta.path       = games / "Beta" / "Beta Quest";
	beta.title_name = "Beta Quest";
	beta.title_id   = "PPSA01002";
	library.push_back(beta);
	auto& context = ui.Context();
	if (!main_window) {
		context.library.Replace(library);
	}

	kyty::gtk::DialogArgs args;
	if (entry->with_entry) {
		args.entry = &alpha;
	}
	const guint             before = KytyUi::ToplevelCount();
	kyty::gtk::DialogHandle handle = type->open(context, parent, args);
	GtkWindow*              window = handle.window;
	if (Check(window != nullptr, std::string(id) + " opens a window") &&
	    KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                    5000, std::string(id) + " to be mapped")) {
		Check(KytyUi::ToplevelCount() == before + 1, std::string(id) + " is one new toplevel");
		Check(main_window ? gtk_window_get_transient_for(window) == nullptr
		                  : gtk_window_get_transient_for(window) == parent,
		      std::string(id) +
		          (main_window ? " is a toplevel of its own" : " is transient for the parent"));
		if (main_window) {
			auto& controller = kyty::gtk::MainWindowLibrary(KYTY_MAIN_WINDOW(window));
			KytyUi::WaitFor([&controller]() { return controller.Stats().details_done >= 1; }, 8000,
			                "the main window's games and their details");
		}
		// Content that loads in the background (trophies, add-ons, cheat files) first.
		GtkWidget* root = GTK_WIDGET(window);
		KytyUi::WaitFor(
		    [&content, root]() {
			    return std::all_of(content.begin(), content.end(), [root](const auto& kind) {
				    return KytyUi::FindFirst(root, kind.match) != nullptr;
			    });
		    },
		    5000, std::string(id) + " to show the content of Alpha Game");
		KytyUi::Pump(300);

		// An accessible title (the window title, which GTK gives the window as its accessible
		// label), or the alert's heading.
		const char*       title_text = gtk_window_get_title(window);
		const std::string title      = title_text != nullptr ? title_text : "";
		const std::string label =
		    KytyUi::AccessibleProperty(GTK_WIDGET(window), GTK_ACCESSIBLE_PROPERTY_LABEL)
		        .value_or("");
		std::printf("%s: %s: %s, %dx%d, title \"%s\", accessible label \"%s\"\n",
		            KytyUi::CaseName().c_str(), id, G_OBJECT_TYPE_NAME(window),
		            gtk_widget_get_width(root), gtk_widget_get_height(root), title.c_str(),
		            label.c_str());
		if (entry->heading != nullptr) {
			Check(KytyUi::FindByLabel(root, entry->heading) != nullptr,
			      std::string(id) + " shows its heading \"" + entry->heading + "\"");
		} else {
			Check(!title.empty() && !label.empty(),
			      std::string(id) + " has a title and an accessible label");
		}
		Check(KytyUi::Capture(window, id), std::string(id) + " draws something");
		if (entry->close_button != nullptr) {
			CloseWith(window, entry->close_button);
		}
	}
	type->close(handle);
	Check(handle.window == nullptr && handle.cancellable == nullptr,
	      "the close function empties the handle");
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
	                std::string(id) + " to close (the toplevel count back to " +
	                    std::to_string(before) + ")");
}

std::vector<core::RemovalFailure> ManyFailures(const fs::path& directory, int count) {
	std::vector<core::RemovalFailure> failures;
	for (int i = 1; i <= count; i++) {
		failures.push_back({directory / "1" / ("savedata" + std::to_string(i) + ".bin"),
		                    std::make_error_code(std::errc::permission_denied)});
	}
	return failures;
}

} // namespace

KYTY_UI_CASE(registry_confirmation_alert) {
	CheckRegistryType("confirmation-alert");
}

KYTY_UI_CASE(registry_error_alert) {
	CheckRegistryType("error-alert");
}

KYTY_UI_CASE(registry_settings_window) {
	CheckRegistryType("settings-window");
}

KYTY_UI_CASE(registry_game_settings_window) {
	CheckRegistryType("game-settings-window");
}

KYTY_UI_CASE(registry_game_picker_dialog) {
	CheckRegistryType("game-picker-dialog");
}

KYTY_UI_CASE(registry_main_window) {
	CheckRegistryType("main-window");
}

KYTY_UI_CASE(registry_input_mapping_window) {
	CheckRegistryType("input-mapping-window");
}

KYTY_UI_CASE(registry_binding_capture_dialog) {
	CheckRegistryType("binding-capture-dialog");
}

KYTY_UI_CASE(registry_trophy_window) {
	CheckRegistryType("trophy-window");
}

KYTY_UI_CASE(registry_trophy_details_dialog) {
	CheckRegistryType("trophy-details-dialog");
}

KYTY_UI_CASE(registry_trophy_overview_window) {
	CheckRegistryType("trophy-overview-window");
}

KYTY_UI_CASE(registry_additional_content_window) {
	CheckRegistryType("additional-content-window");
}

KYTY_UI_CASE(registry_compression_dialog) {
	CheckRegistryType("compression-dialog");
}

KYTY_UI_CASE(registry_cheats_window) {
	CheckRegistryType("cheats-window");
}

KYTY_UI_CASE(registry_update_offer_alert) {
	CheckRegistryType("update-offer-alert");
}

KYTY_UI_CASE(registry_update_progress_window) {
	CheckRegistryType("update-progress-window");
}

KYTY_UI_CASE(registry_update_result_alert) {
	CheckRegistryType("update-result-alert");
}

KYTY_UI_CASE(registry_clear_config_alert) {
	CheckRegistryType("clear-config-alert");
}

KYTY_UI_CASE(registry_remove_save_data_alert) {
	CheckRegistryType("remove-save-data-alert");
}

KYTY_UI_CASE(registry_save_data_failures_alert) {
	CheckRegistryType("save-data-failures-alert");
}

KYTY_UI_CASE(registry_import_settings_alert) {
	CheckRegistryType("import-settings-alert");
}

KYTY_UI_CASE(registry_complete) {
	const auto names = KytyUi::CaseNames();
	const auto has   = [&names](const std::string& name) {
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	std::set<std::string> cases_of_ids;
	for (const auto& type: kyty::gtk::DialogTypes()) {
		const std::string name = CaseFor(type.id);
		cases_of_ids.insert(name);
		Check(has(name), std::string(type.id) + " has its case " + name);
		Check(FindEntry(type.id) != nullptr, std::string(type.id) + " is in this file's table");
	}
	std::printf("%s: %zu registry ids, %zu cases\n", KytyUi::CaseName().c_str(),
	            kyty::gtk::DialogTypes().size(), names.size());
	Check(kyty::gtk::DialogTypes().size() == 21, "the registry has its 21 ids");
	Check(std::size(kEntries) == kyty::gtk::DialogTypes().size(),
	      "this file's table has one row per registry id");
	for (const auto& name: names) {
		if (!name.starts_with("registry_") ||
		    std::find(std::begin(kOtherCases), std::end(kOtherCases), name) !=
		        std::end(kOtherCases)) {
			continue;
		}
		Check(cases_of_ids.count(name) == 1, "the case " + name + " names a registry id");
	}
	for (const char* other: kOtherCases) {
		Check(has(other), std::string("the case ") + other + " exists");
	}
}

KYTY_UI_CASE(registry_alert_already_cancelled) {
	KytyUi::UiContext ui;
	GtkWindow*        parent      = ui.ParentWindow();
	const auto        games       = ui.WriteFixtureGames();
	const auto        alpha       = AlphaEntry(games);
	auto              cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	g_cancellable_cancel(cancellable.get());
	const guint before = KytyUi::ToplevelCount();
	int         calls  = 0;

	Check(kyty::gtk::Confirm(
	          parent, "Remove the save data?", "The action cannot be undone.", "Remove", "Cancel",
	          true, [&calls](bool /*accepted*/) { calls++; }, cancellable.get()) == nullptr,
	      "Confirm opens nothing");
	Check(kyty::gtk::Choose(
	          parent, "Version 2.0 is available", "Install it now?",
	          {"Cancel", "Release notes", "Install update"}, 0, 2,
	          [&calls](int /*index*/) { calls++; }, cancellable.get()) == nullptr,
	      "Choose opens nothing");
	Check(kyty::gtk::ShowError(parent, "The settings could not be saved",
	                           "No space left on device.", cancellable.get()) == nullptr,
	      "ShowError opens nothing");
	Check(kyty::gtk::ShowErrorList(parent, "Some save data could not be removed",
	                               "Could not remove:", {"a: Permission denied"},
	                               cancellable.get()) == nullptr,
	      "ShowErrorList opens nothing");
	// The launcher's alerts on top of them.
	Check(kyty::gtk::ConfirmClearGameConfig(
	          parent, alpha, [&calls](bool /*accepted*/) { calls++; }, cancellable.get()) ==
	          nullptr,
	      "the Clear game config confirmation opens nothing");
	Check(kyty::gtk::ShowSaveDataFailures(parent, alpha, ManyFailures(games, 3),
	                                      cancellable.get()) == nullptr,
	      "the save data failure list opens nothing");
	Check(kyty::gtk::ShowUpdateOffer(
	          parent, "2026.04.01-def5678", "2026.03.01-abc1234",
	          [&calls](kyty::gtk::UpdateOfferResponse /*response*/) { calls++; },
	          cancellable.get()) == nullptr,
	      "the update offer opens nothing");
	Check(KytyUi::ToplevelCount() == before, "no window was created");
	// Whatever a created alert would have scheduled would run here.
	KytyUi::Pump(300);
	Check(KytyUi::ToplevelCount() == before, "still no new window after the main loop ran");
	Check(calls == 0, "no callback ran");
	Check(gtk_widget_get_mapped(GTK_WIDGET(parent)) != FALSE, "the parent is still mapped");
}

KYTY_UI_CASE(registry_save_data_failures_scrolls) {
	KytyUi::UiContext ui;
	GtkWindow*        parent  = ui.ParentWindow();
	const auto        games   = ui.WriteFixtureGames();
	const auto        alpha   = AlphaEntry(games);
	const fs::path    saves   = ui.AppDirectory() / "_SaveData" / "PPSA01001";
	const auto        many    = ManyFailures(saves, 200);
	const guint       before  = KytyUi::ToplevelCount();
	const auto        open_at = [&](GtkWindow* owner, GCancellable* cancellable) -> GtkWindow* {
		GtkWindow* window = kyty::gtk::ShowSaveDataFailures(owner, alpha, many, cancellable);
		if (!Check(window != nullptr, "the failure list opens") ||
		    !KytyUi::WaitFor(
		        [window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; }, 5000,
		        "the failure list to be mapped")) {
			return nullptr;
		}
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		return window;
	};

	// The size and the scrolling.
	GtkWindow* window = open_at(parent, nullptr);
	if (window == nullptr) {
		return;
	}
	GdkDisplay* display  = gdk_display_get_default();
	auto        monitors = g_list_model_get_n_items(gdk_display_get_monitors(display));
	auto        monitor  = GObjectPtr<GdkMonitor>::Adopt(
	    static_cast<GdkMonitor*>(g_list_model_get_item(gdk_display_get_monitors(display), 0)));
	GdkRectangle geometry {};
	if (monitor) {
		gdk_monitor_get_geometry(monitor.get(), &geometry);
	}
	// The window as drawn (on X11 without a window manager GTK draws a 5 pixel frame of its own
	// around the window widget's allocation, which the widget's width does not count).
	const auto texture = KytyUi::Render(window, "the failure list");
	const int  width   = texture ? gdk_texture_get_width(texture.get()) : 0;
	const int  height  = texture ? gdk_texture_get_height(texture.get()) : 0;
	std::printf("%s: %u monitor(s), %dx%d; the failure list window is drawn %dx%d (allocation "
	            "%dx%d)\n",
	            KytyUi::CaseName().c_str(), monitors, geometry.width, geometry.height, width,
	            height, gtk_widget_get_width(GTK_WIDGET(window)),
	            gtk_widget_get_height(GTK_WIDGET(window)));
	Check(geometry.width == 1280 && geometry.height == 800, "the screen is 1280x800");
	Check(height > 0 && height <= 600, "200 failures fit the screen: the window is " +
	                                       std::to_string(height) + " <= 600 pixels high");
	Check(width >= 480, "the window is drawn at least 480 pixels wide");
	Check(gtk_window_get_transient_for(window) == parent && gtk_window_get_modal(window) != FALSE &&
	          gtk_window_get_destroy_with_parent(window) != FALSE,
	      "the window is modal, transient for its parent, and destroyed with it");
	const auto rows = KytyUi::FindAll(
	    GTK_WIDGET(window), [](GtkWidget* widget) { return GTK_IS_LIST_BOX_ROW(widget); });
	Check(rows.size() == 200, "one row per failure (" + std::to_string(rows.size()) + ")");
	const std::string text = KytyUi::AllText(GTK_WIDGET(window));
	Check(text.find((saves / "1" / "savedata1.bin").string() + ": ") != std::string::npos &&
	          text.find((saves / "1" / "savedata200.bin").string() + ": ") != std::string::npos,
	      "the first and the last failure are listed with their reasons");
	GtkWidget* scrolled = KytyUi::FindByType(GTK_WIDGET(window), GTK_TYPE_SCROLLED_WINDOW);
	if (Check(scrolled != nullptr, "the list is in a scrolled window")) {
		GtkAdjustment* adjustment =
		    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
		const double upper = gtk_adjustment_get_upper(adjustment);
		const double page  = gtk_adjustment_get_page_size(adjustment);
		std::printf("%s: vadjustment upper %.0f, page size %.0f\n", KytyUi::CaseName().c_str(),
		            upper, page);
		Check(upper > page, "the list is longer than its view: it scrolls");
		Check(page <= 320, "the list's view is at most 320 pixels high");
		GtkPolicyType horizontal = GTK_POLICY_AUTOMATIC;
		gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(scrolled), &horizontal, nullptr);
		Check(horizontal == GTK_POLICY_NEVER, "the lines wrap instead of scrolling sideways");
		gtk_adjustment_set_value(adjustment, upper - page);
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		Check(gtk_adjustment_get_value(adjustment) > 0, "the list scrolls to its end");
		gtk_adjustment_set_value(adjustment, 0);
	}
	Check(gtk_window_get_default_widget(window) == Button(window, {"_Close"}),
	      "Close is the default button");
	Check(KytyUi::Capture(window, "save-data-failures"), "the failure list draws something");
	CloseWith(window, "Close");

	// Escape.
	window = open_at(parent, nullptr);
	if (window != nullptr) {
		Check(KytyUi::KeyPressOn(GTK_WIDGET(window), GDK_KEY_Escape), "Escape is handled");
		KytyUi::WaitFor([window]() { return !KytyUi::IsToplevel(window); }, 3000,
		                "Escape to close the failure list");
	}
	// The cancellable.
	auto cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	window           = open_at(parent, cancellable.get());
	if (window != nullptr) {
		g_cancellable_cancel(cancellable.get());
		KytyUi::WaitFor([window]() { return !KytyUi::IsToplevel(window); }, 3000,
		                "cancelling to close the failure list");
	}
	// The parent.
	auto* owner = GTK_WINDOW(adw_window_new());
	gtk_window_set_default_size(owner, 600, 400);
	gtk_window_present(owner);
	if (KytyUi::WaitFor([owner]() { return gtk_widget_get_mapped(GTK_WIDGET(owner)) != FALSE; },
	                    5000, "the second parent to be mapped") &&
	    KytyUi::WaitForPaintedFrame(GTK_WIDGET(owner))) {
		auto other = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
		window     = open_at(owner, other.get());
		if (window != nullptr) {
			gtk_window_destroy(owner); // the parent, not the list
			KytyUi::WaitFor([window]() { return !KytyUi::IsToplevel(window); }, 3000,
			                "the parent's destruction to close the failure list");
			g_cancellable_cancel(other.get());
		}
	}
	if (KytyUi::IsToplevel(owner)) {
		gtk_window_destroy(owner);
	}
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 3000,
	                "the toplevel count to be back");
}
