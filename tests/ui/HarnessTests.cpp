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
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <span>
#include <string>

namespace {

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
