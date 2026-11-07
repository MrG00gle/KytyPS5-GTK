// Smoke cases of the UI test harness (task 22.1; phase 7 plan section 6 U2.5, U2.7): each piece of
// the harness works on both backends, so that the cases of tasks 22.2-22.5 can rely on it.
// - harness_portal_scheme: libadwaita reads the fake portal's color scheme (no preference: light)
//   and follows TestPortal.SetColorScheme live.
// - harness_registry_confirmation_alert: the registry's confirmation-alert opens, is found by its
//   text, has buttons with accessible names under GTK_A11Y=test (plan D9), and closes with its
//   Cancel button; the toplevel count is back where it was.
// - harness_loopback_http: requests through LoopbackHttpClient reach the in-process SoupServer
//   and are recorded on both sides (plan D12); unscripted paths answer 404; chunked, delayed, and
//   stalled responses behave as scripted.
// - harness_application_starts: the real KytyApplication with the test hooks (plan D13) maps its
//   main window, scans the fixture games, downloads the compatibility database from the loopback
//   server, and app.quit ends it with status 0.
// - harness_capture: a launcher window (the registry's settings-window) renders; with
//   --capture-dir it is written as a light and a dark PNG.
// - harness_keypress: KeyPress dispatches to a local shortcut of the focus widget, an alternative
//   trigger of an ancestor, a global shortcut elsewhere in the window, the window's Tab binding,
//   and a key controller; MoveFocus moves the focus both ways (plan D10).
// - harness_locale (phase 8 plan D17, p7-u2-1): no LC_* or LANGUAGE variable reaches the session,
//   LANG is C.UTF-8, and every locale category is C.UTF-8 (ui_harness_selftest runs it with the
//   caller's LC_MESSAGES, LC_NUMERIC, and LC_TIME set).
// - harness_accessible_property (phase 8 plan D17, p7-u2-2): AccessibleProperty reads boolean,
//   integer, number, string, and token properties by their own type, set by hand and by GTK (a
//   scale's range); a property the widget lacks gives nullopt. ui_harness_selftest checks that a
//   property outside these types fails the case.
// The loopback case also covers p7-u2-3: a scripted redirect is mapped to the server too, and one
// to a URL that is neither https nor the server's ends the request.

#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "support/gtk/loopbackHttp.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <chrono>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <span>
#include <string>

namespace {

using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using KytyUi::Check;
using namespace std::chrono_literals;

constexpr const char* kCompatibilityUrl = "https://kytyps5.github.io/data/compatibility.json";

struct Fetched {
	kyty::core::HttpResult result;
	std::string            body;
	int                    chunks = 0;
	int                    done   = 0;
};

Fetched Fetch(kyty::core::HttpClient& client, const kyty::core::HttpRequest& request) {
	Fetched fetched;
	client.Get(
	    request,
	    [&fetched](std::span<const std::byte> bytes) {
		    fetched.chunks++;
		    fetched.body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		    return true;
	    },
	    [&fetched](const kyty::core::HttpResult& result) {
		    fetched.done++;
		    fetched.result = result;
	    });
	KytyUi::WaitFor([&fetched]() { return fetched.done > 0; }, 10000,
	                "the request to " + request.url);
	KytyUi::Pump(30);
	Check(fetched.done == 1, "DoneFn runs once for " + request.url);
	return fetched;
}

gboolean Count(GtkWidget* /*widget*/, GVariant* /*arguments*/, gpointer data) {
	++*static_cast<int*>(data);
	return TRUE;
}

GtkShortcut* CountingShortcut(const char* trigger, int* counter) {
	return gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger),
	                        gtk_callback_action_new(&Count, counter, nullptr));
}

} // namespace

KYTY_UI_CASE(harness_portal_scheme) {
	AdwStyleManager* style = adw_style_manager_get_default();
	Check(adw_style_manager_get_system_supports_color_schemes(style) != FALSE,
	      "libadwaita reads color-scheme from the fake portal");
	Check(adw_style_manager_get_color_scheme(style) == ADW_COLOR_SCHEME_DEFAULT,
	      "the style manager follows the desktop");
	Check(adw_style_manager_get_dark(style) == FALSE, "no preference (0) is light");
	if (KytyUi::SetPortalColorScheme(1)) {
		KytyUi::WaitFor([style]() { return adw_style_manager_get_dark(style) != FALSE; }, 1000,
		                "prefer dark (1) from the portal to make the style dark");
	}
	if (KytyUi::SetPortalColorScheme(2)) {
		KytyUi::WaitFor([style]() { return adw_style_manager_get_dark(style) == FALSE; }, 1000,
		                "prefer light (2) from the portal to make the style light again");
	}
}

KYTY_UI_CASE(harness_registry_confirmation_alert) {
	KytyUi::UiContext ui;
	GtkWindow*        parent = ui.ParentWindow();
	const guint       before = KytyUi::ToplevelCount();
	const auto*       type   = kyty::gtk::FindDialogType("confirmation-alert");
	if (!Check(type != nullptr, "confirmation-alert is registered")) {
		return;
	}
	kyty::gtk::DialogHandle handle = type->open(ui.Context(), parent, {});
	GtkWindow*              alert  = KytyUi::FindAlert("Do you want to continue?");
	if (alert != nullptr) {
		Check(KytyUi::AllText(GTK_WIDGET(alert)).find("The action cannot be undone.") !=
		          std::string::npos,
		      "the alert shows its body");
		Check(KytyUi::ToplevelCount() == before + 1, "the alert is one new toplevel");
		// Plan D9: GTK's test accessibility backend, and accessible names through its test API.
		const auto buttons = KytyUi::FindAll(
		    GTK_WIDGET(alert), [](GtkWidget* widget) { return GTK_IS_BUTTON(widget); });
		Check(KytyUi::FindByLabel(GTK_WIDGET(alert), "Cancel") != nullptr &&
		          KytyUi::FindByLabel(GTK_WIDGET(alert), "Continue") != nullptr,
		      "the alert has its Cancel and Continue buttons");
		for (GtkWidget* button: buttons) {
			const std::string name = KytyUi::AccessibleName(button);
			std::printf(
			    "%s: button \"%s\": context %s, role button %d\n", KytyUi::CaseName().c_str(),
			    name.c_str(), KytyUi::AtContextType(button).c_str(),
			    gtk_test_accessible_has_role(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_ROLE_BUTTON));
			Check(KytyUi::AtContextType(button) == "GtkTestATContext",
			      "GTK_A11Y=test gives the button GTK's test accessibility context");
			Check(gtk_test_accessible_has_role(GTK_ACCESSIBLE(button),
			                                   GTK_ACCESSIBLE_ROLE_BUTTON) != FALSE,
			      "the button has the button role");
			Check(!name.empty(), "the button has an accessible name");
		}
		Check(KytyUi::Capture(alert, "alert"), "the alert renders");
		KytyUi::CloseAlert(alert, "Cancel");
	}
	type->close(handle);
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 2000,
	                "the toplevel count to be back after Cancel");
}

KYTY_UI_CASE(harness_loopback_http) {
	KytyTests::LoopbackServer     server;
	KytyTests::LoopbackHttpClient client(server);
	KytyTests::LoopbackResponse   compatibility;
	compatibility.body = KytyUi::kFixtureCompatibility;
	server.Script(kCompatibilityUrl, compatibility);

	kyty::core::HttpRequest request;
	request.url     = kCompatibilityUrl;
	request.headers = {{"X-Kyty-Test", "smoke"}};
	const auto ok   = Fetch(client, request);
	Check(ok.result.failure == kyty::core::HttpFailure::None && ok.result.status == 200 &&
	          ok.body == KytyUi::kFixtureCompatibility,
	      "a scripted https URL answers through the loopback server");
	Check(client.Requests().size() == 1 && client.Requests()[0].url == kCompatibilityUrl &&
	          client.Requests()[0].path == server.Rewrite(kCompatibilityUrl) &&
	          client.Requests()[0].headers == request.headers,
	      "the client records the URL as asked, the rewritten URL, and the headers");
	Check(server.Rewrite(kCompatibilityUrl) == "http://127.0.0.1:" + std::to_string(server.Port()) +
	                                               "/kytyps5.github.io/data/compatibility.json",
	      "https://<host>/<path> becomes http://127.0.0.1:<port>/<host>/<path>");
	bool header_seen = false;
	if (Check(server.Requests().size() == 1, "the server saw one request")) {
		const auto& seen = server.Requests()[0];
		Check(seen.path == "/kytyps5.github.io/data/compatibility.json" && seen.method == "GET" &&
		          seen.url == kCompatibilityUrl,
		      "the server records the path, the method, and the https URL");
		for (const auto& [name, value]: seen.headers) {
			header_seen = header_seen || (name == "X-Kyty-Test" && value == "smoke");
		}
	}
	Check(header_seen, "the request header reaches the server");
	Check(server.Hits(kCompatibilityUrl) == 1, "Hits counts the scripted URL");

	request.url           = "https://example.invalid/not-scripted";
	request.headers       = {};
	const auto unscripted = Fetch(client, request);
	Check(unscripted.result.failure == kyty::core::HttpFailure::Status &&
	          unscripted.result.status == 404,
	      "an unscripted URL answers 404");

	// p7-u2-3: SoupHttpClient follows redirects itself, and the loopback mapping covers them too.
	KytyTests::LoopbackResponse moved;
	moved.status  = 302;
	moved.headers = {{"Location", kCompatibilityUrl}};
	server.Script("https://example.invalid/moved", moved);
	request.url           = "https://example.invalid/moved";
	const auto redirected = Fetch(client, request);
	Check(redirected.result.failure == kyty::core::HttpFailure::None &&
	          redirected.body == KytyUi::kFixtureCompatibility &&
	          server.Hits(kCompatibilityUrl) == 2,
	      "a scripted redirect to an https URL is served by the loopback server");
	KytyTests::LoopbackResponse away = moved;
	const std::string           other_port =
	    "http://127.0.0.1:" + std::to_string(server.Port() == 1 ? 2 : 1) + "/elsewhere";
	away.headers = {{"Location", other_port}};
	server.Script("https://example.invalid/away", away);
	request.url        = "https://example.invalid/away";
	const auto stopped = Fetch(client, request);
	Check(stopped.result.failure == kyty::core::HttpFailure::Network &&
	          stopped.result.status == 302 &&
	          stopped.result.message == "No URL mapping for " + other_port,
	      "a redirect that is neither https nor the server's ends with Network naming it (" +
	          stopped.result.message + ")");

	KytyTests::LoopbackResponse chunked;
	chunked.body.assign(200000, 'k');
	chunked.delay_ms          = 50;
	chunked.chunk_size        = 65536;
	chunked.chunk_interval_ms = 20;
	server.Script("https://example.invalid/chunked", chunked);
	request.url       = "https://example.invalid/chunked";
	const auto pieces = Fetch(client, request);
	Check(pieces.result.failure == kyty::core::HttpFailure::None && pieces.body == chunked.body &&
	          pieces.chunks >= 2,
	      "a delayed chunked body arrives complete in several chunks");

	KytyTests::LoopbackResponse stalled = chunked;
	stalled.stall_after_chunks          = 1;
	server.Script("https://example.invalid/stalled", stalled);
	request.url           = "https://example.invalid/stalled";
	request.stall_timeout = 300ms;
	const auto stall      = Fetch(client, request);
	Check(stall.result.failure == kyty::core::HttpFailure::Stalled && stall.body.size() == 65536,
	      "a body that stops after one chunk gives Stalled");
	Check(client.ActiveCount() == 0, "no request is left");
}

KYTY_UI_CASE(harness_application_starts) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return;
	}
	ui.ServeCompatibility();
	const int status = ui.RunApplication([&ui](KytyApplication* application,
	                                           KytyMainWindow*  window) {
		auto* context = kyty::gtk::ApplicationContext(application);
		Check(context != nullptr && context->emulator.found &&
		          context->emulator.executable == ui.AppDirectory() / "kyty_emulator",
		      "the application uses the case's launcher directory and finds the fake Emulator");
		Check(context != nullptr && context->paths.settings_file == ui.SettingsFile() &&
		          context->paths.data_dir == ui.AppDirectory(),
		      "the Settings_Store and the Data_Directory are in the case's tree");
		auto& library = kyty::gtk::MainWindowLibrary(window);
		KytyUi::WaitFor([&library]() { return library.Stats().scans_applied >= 1; }, 8000,
		                "the first scan to be applied");
		const guint count =
		    context != nullptr ? g_list_model_get_n_items(context->library.Games()) : 0;
		Check(count == 5, "the library shows the 5 fixture games (" + std::to_string(count) + ")");
		KytyUi::WaitFor([&ui]() { return ui.Server().Hits(kCompatibilityUrl) == 1; }, 5000,
		                "the compatibility database download from the loopback server");
		Check(ui.Http().Requests().size() >= 1 && ui.Http().Requests()[0].url == kCompatibilityUrl,
		      "the application's HTTP goes through the test client");
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0,
	      "app.quit ends the application with status 0 (" + std::to_string(status) + ")");
}

KYTY_UI_CASE(harness_capture) {
	KytyUi::UiContext ui;
	GtkWindow*        parent = ui.ParentWindow();
	const guint       before = KytyUi::ToplevelCount();
	const auto*       type   = kyty::gtk::FindDialogType("settings-window");
	if (!Check(type != nullptr, "settings-window is registered")) {
		return;
	}
	kyty::gtk::DialogHandle handle = type->open(ui.Context(), parent, {});
	if (Check(handle.window != nullptr, "settings-window opens a window") &&
	    KytyUi::WaitFor(
	        [&handle]() { return gtk_widget_get_mapped(GTK_WIDGET(handle.window)) != FALSE; }, 5000,
	        "the settings window to be mapped")) {
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(handle.window));
		const auto texture = KytyUi::Render(handle.window, "settings-window");
		Check(texture && gdk_texture_get_width(texture.get()) > 0 &&
		          gdk_texture_get_height(texture.get()) > 0,
		      "the settings window renders into a texture");
		Check(KytyUi::Capture(handle.window, "settings-window"),
		      "Capture() of the settings window");
		if (!KytyUi::CaptureDirectory().empty()) {
			for (const char* scheme: {"light", "dark"}) {
				const auto path = KytyUi::CaptureDirectory() /
				                  (KytyUi::CaseName() + "-settings-window-" + scheme + ".png");
				GObjectPtr<GdkTexture> written;
				kyty::gtk::GErrorPtr   error;
				written = GObjectPtr<GdkTexture>::Adopt(
				    gdk_texture_new_from_filename(path.c_str(), error.Out()));
				Check(written && gdk_texture_get_width(written.get()) ==
				                     gdk_texture_get_width(texture.get()),
				      "Capture() wrote " + path.string() + " at the window's width");
			}
		}
	}
	type->close(handle);
	KytyUi::WaitFor([before]() { return KytyUi::ToplevelCount() == before; }, 2000,
	                "the settings window to close");
}

KYTY_UI_CASE(harness_keypress) {
	struct Counters {
		int local       = 0;
		int alternative = 0;
		int global      = 0;
		int other_key   = 0;
	} counters;
	auto*      window = GTK_WINDOW(adw_window_new());
	GtkWidget* box    = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget* first  = gtk_button_new_with_label("First");
	GtkWidget* second = gtk_button_new_with_label("Second");
	gtk_box_append(GTK_BOX(box), first);
	gtk_box_append(GTK_BOX(box), second);
	adw_window_set_content(ADW_WINDOW(window), box);
	gtk_window_set_default_size(window, 400, 200);

	// A local shortcut of the focus widget, an alternative trigger on an ancestor (the box), a
	// global shortcut on a widget that is not an ancestor, and a key controller on the box.
	GtkEventController* local = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(local),
	                                     CountingShortcut("<Control>k", &counters.local));
	gtk_widget_add_controller(first, local);
	GtkEventController* alternative = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(
	    GTK_SHORTCUT_CONTROLLER(alternative),
	    CountingShortcut("Delete|KP_Delete", &counters.alternative));
	gtk_widget_add_controller(box, alternative);
	GtkEventController* global = gtk_shortcut_controller_new();
	gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(global), GTK_SHORTCUT_SCOPE_GLOBAL);
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(global),
	                                     CountingShortcut("<Control>g", &counters.global));
	gtk_widget_add_controller(second, global);
	GtkEventController* keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed",
	                 G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
	                                gpointer data) -> gboolean {
		                 if (keyval == GDK_KEY_x) {
			                 ++*static_cast<int*>(data);
		                 }
		                 return FALSE;
	                 }),
	                 &counters.other_key);
	gtk_widget_add_controller(box, keys);

	gtk_window_present(window);
	if (!KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                     5000, "the test window to be mapped")) {
		gtk_window_destroy(window);
		return;
	}
	gtk_widget_grab_focus(first);
	Check(gtk_window_get_focus(window) == first, "the first button has the focus");

	Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_k, GDK_CONTROL_MASK) && counters.local == 1,
	      "Ctrl+K runs the focus widget's local shortcut");
	Check(!KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_k,
	                        GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
	          counters.local == 1,
	      "Ctrl+Shift+K does not match Ctrl+K");
	Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_KP_Delete) && counters.alternative == 1,
	      "KP_Delete runs an ancestor's Delete|KP_Delete shortcut");
	Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_g, GDK_CONTROL_MASK) && counters.global == 1,
	      "Ctrl+G runs a global shortcut of a widget that is not an ancestor");
	Check(!KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_x) && counters.other_key == 1,
	      "an unbound key reaches the key controller, which does not handle it");
	Check(counters.local == 1 && counters.alternative == 1 && counters.global == 1,
	      "every key ran only its own handler");

	Check(KytyUi::MoveFocus(window, GTK_DIR_TAB_FORWARD) == second,
	      "move-focus forward moves to the second button");
	Check(KytyUi::MoveFocus(window, GTK_DIR_TAB_BACKWARD) == first,
	      "move-focus backward moves back to the first one");
	Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_Tab) &&
	          gtk_window_get_focus(window) == second,
	      "Tab runs the window's own binding and moves the focus forward");
	gtk_window_destroy(window);
}

KYTY_UI_CASE(harness_locale) {
	std::string leaked;
	gchar**     environment = g_get_environ();
	for (gchar** entry = environment; *entry != nullptr; entry++) {
		if (g_str_has_prefix(*entry, "LC_") || g_str_has_prefix(*entry, "LANGUAGE=")) {
			leaked += (leaked.empty() ? "" : ", ") + std::string(*entry);
		}
	}
	g_strfreev(environment);
	std::printf("%s: LANG %s, leaked variables: %s\n", KytyUi::CaseName().c_str(),
	            g_getenv("LANG") != nullptr ? g_getenv("LANG") : "(unset)",
	            leaked.empty() ? "none" : leaked.c_str());
	Check(leaked.empty(), "no LC_* or LANGUAGE variable reaches the session (" + leaked + ")");
	Check(g_strcmp0(g_getenv("LANG"), "C.UTF-8") == 0, "LANG is C.UTF-8");
	// setlocale(LC_ALL, "") ran before gtk_init (uiHarness.cpp): one name for every category.
	const std::string all = std::setlocale(LC_ALL, nullptr);
	std::printf("%s: setlocale(LC_ALL) %s\n", KytyUi::CaseName().c_str(), all.c_str());
	Check(all == "C.UTF-8", "every locale category is C.UTF-8 (" + all + ")");
	struct Category {
		int         id;
		const char* name;
	};
	for (const Category category:
	     {Category {LC_MESSAGES, "LC_MESSAGES"}, Category {LC_NUMERIC, "LC_NUMERIC"},
	      Category {LC_TIME, "LC_TIME"}}) {
		const std::string locale = std::setlocale(category.id, nullptr);
		Check(locale == "C.UTF-8", std::string(category.name) + " is C.UTF-8 (" + locale + ")");
	}
	// What the categories change: the decimal separator and the month names.
	char number[16];
	std::snprintf(number, sizeof number, "%.1f", 2.5);
	GDateTime*     date = g_date_time_new_utc(2026, 1, 15, 12, 0, 0);
	const GCharPtr month(g_date_time_format(date, "%B"));
	g_date_time_unref(date);
	std::printf("%s: 2.5 prints as %s, month 1 is %s\n", KytyUi::CaseName().c_str(), number,
	            month.get() != nullptr ? month.get() : "(none)");
	Check(std::string(number) == "2.5",
	      "the decimal separator is a point (" + std::string(number) + ")");
	Check(month.View() == "January", "month names are English (" + std::string(month.View()) + ")");
}

KYTY_UI_CASE(harness_accessible_property) {
	auto*      window = GTK_WINDOW(adw_window_new());
	GtkWidget* box    = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget* typed  = gtk_label_new("Typed");
	GtkWidget* scale  = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 10, 1);
	GtkWidget* plain  = gtk_button_new_with_label("Plain");
	gtk_range_set_value(GTK_RANGE(scale), 4);
	gtk_box_append(GTK_BOX(box), typed);
	gtk_box_append(GTK_BOX(box), scale);
	gtk_box_append(GTK_BOX(box), plain);
	adw_window_set_content(ADW_WINDOW(window), box);
	// Values equal to the first value AccessibleProperty compares with (FALSE, 0, 0.0, the first
	// token) and values that differ from it, of each type.
	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(typed), GTK_ACCESSIBLE_PROPERTY_MODAL, TRUE,
	    GTK_ACCESSIBLE_PROPERTY_READ_ONLY, FALSE, GTK_ACCESSIBLE_PROPERTY_LEVEL, 3,
	    GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, 2.5, GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, 0.0,
	    GTK_ACCESSIBLE_PROPERTY_SORT, GTK_ACCESSIBLE_SORT_DESCENDING,
	    GTK_ACCESSIBLE_PROPERTY_AUTOCOMPLETE, GTK_ACCESSIBLE_AUTOCOMPLETE_NONE,
	    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "A typed label", -1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(box), GTK_ACCESSIBLE_PROPERTY_LEVEL, 0, -1);
	gtk_window_present(window);
	if (!KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                     5000, "the test window to be mapped")) {
		gtk_window_destroy(window);
		return;
	}
	struct Expected {
		GtkWidget*            widget;
		GtkAccessibleProperty property;
		const char*           what;
		const char*           value;
	};
	const Expected expected[] = {
	    {typed, GTK_ACCESSIBLE_PROPERTY_MODAL, "MODAL (boolean, true)", "true"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_READ_ONLY, "READ_ONLY (boolean, false)", "false"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_LEVEL, "LEVEL (integer, 3)", "3"},
	    {box, GTK_ACCESSIBLE_PROPERTY_LEVEL, "the box's LEVEL (integer, 0)", "0"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, "VALUE_NOW (number, 2.5)", "2.5"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, "VALUE_MIN (number, 0)", "0"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_SORT, "SORT (token, descending)", "descending"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_AUTOCOMPLETE, "AUTOCOMPLETE (token, none)", "none"},
	    {typed, GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "DESCRIPTION (string)", "A typed label"},
	    {scale, GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, "the scale's VALUE_MAX (number)", "10"},
	    {scale, GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, "the scale's VALUE_NOW (number)", "4"},
	    {scale, GTK_ACCESSIBLE_PROPERTY_ORIENTATION, "the scale's ORIENTATION (token)", "vertical"},
	};
	for (const auto& item: expected) {
		const auto value = KytyUi::AccessibleProperty(item.widget, item.property);
		std::printf("%s: %s: %s\n", KytyUi::CaseName().c_str(), item.what,
		            value ? value->c_str() : "(none)");
		Check(value && *value == item.value, std::string(item.what) + " reads \"" + item.value +
		                                         "\" (" + value.value_or("none") + ")");
	}
	Check(!KytyUi::AccessibleProperty(plain, GTK_ACCESSIBLE_PROPERTY_LEVEL).has_value(),
	      "a property the widget does not have gives nullopt");
	Check(KytyUi::AccessibleName(plain) == "Plain", "AccessibleName still reads the label");
	gtk_window_destroy(window);
}
