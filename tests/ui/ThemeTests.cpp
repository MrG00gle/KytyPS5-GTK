// UI tests of the color scheme (task 22.2; Requirements 6.9, 6.16; phase 7 plan D8, D15), each on
// Wayland and on X11, with the fake portal's org.freedesktop.appearance color-scheme:
// - theme_initial_light: no preference (0) when the launcher starts: the main window and a dialog
//   opened later are light.
// - theme_initial_dark: prefer dark (1) before the start (KytyUi::InitialColorScheme, set before
//   gtk_init and adw_init): both are dark.
// - theme_prefer_light: prefer light (2) before the start: both are light.
// - theme_live_switch: with the main window, the settings window, and an alert open,
//   TestPortal.SetColorScheme(1) and then (0) over D-Bus switch every toplevel within 1 s of the
//   call, measured as the first painted frame whose rendering has the new scheme's background
//   luminance; one warm-up round, then 5 measured rounds, the median per toplevel and direction
//   asserted and min, median, and max printed (D15). With --capture-dir it writes each toplevel
//   before (light) and after (dark) the switch.
// A window's scheme is read from its rendering: the mean luminance of its opaque pixels is below
// 0.5 in the dark scheme and above it in the light one.

#include "launcher/gtk/dialogRegistry.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/settingsWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <algorithm>
#include <cstdio>
#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace {

using kyty::gtk::GObjectPtr;
using KytyUi::Check;

const KytyUi::InitialColorScheme kInitialDark("theme_initial_dark", 1);
const KytyUi::InitialColorScheme kPreferLight("theme_prefer_light", 2);

// The mean relative luminance (0 black to 1 white) of the texture's opaque pixels, or -1.
double MeanLuminance(GdkTexture* texture) {
	const int           width  = gdk_texture_get_width(texture);
	const int           height = gdk_texture_get_height(texture);
	const gsize         stride = static_cast<gsize>(width) * 4;
	std::vector<guint8> pixels(stride * static_cast<gsize>(height));
	// CAIRO_FORMAT_ARGB32, premultiplied: B, G, R, A in memory on little-endian hosts.
	gdk_texture_download(texture, pixels.data(), stride);
	double sum   = 0;
	gsize  count = 0;
	for (gsize at = 0; at + 3 < pixels.size(); at += 4) {
		if (pixels[at + 3] < 250) {
			continue;
		}
		sum += (0.0722 * pixels[at] + 0.7152 * pixels[at + 1] + 0.2126 * pixels[at + 2]) / 255.0;
		count++;
	}
	return count > 0 ? sum / static_cast<double>(count) : -1;
}

// The window as its renderer draws it now (after a layout), without waiting for a frame.
GObjectPtr<GdkTexture> Snapshot(GtkWindow* window) {
	auto paintable  = GObjectPtr<GdkPaintable>::Adopt(gtk_widget_paintable_new(GTK_WIDGET(window)));
	const int width = gdk_paintable_get_intrinsic_width(paintable.get());
	const int height = gdk_paintable_get_intrinsic_height(paintable.get());
	if (width <= 0 || height <= 0) {
		return {};
	}
	GtkSnapshot* snapshot = gtk_snapshot_new();
	gdk_paintable_snapshot(paintable.get(), snapshot, width, height);
	GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
	if (node == nullptr) {
		return {};
	}
	graphene_rect_t bounds;
	gsk_render_node_get_bounds(node, &bounds);
	auto texture = GObjectPtr<GdkTexture>::Adopt(
	    gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, &bounds));
	gsk_render_node_unref(node);
	return texture;
}

struct Sample {
	double luminance  = -1;
	gint64 painted_us = 0; // g_get_monotonic_time() of the frame's after-paint
};

// The next painted frame of window and its luminance.
Sample NextFrame(GtkWindow* window) {
	Sample         sample;
	GdkFrameClock* clock = gtk_widget_get_frame_clock(GTK_WIDGET(window));
	if (clock == nullptr) {
		KytyUi::Fail("a toplevel without a frame clock");
		return sample;
	}
	const auto scope = kyty::gtk::SignalScope::Connect(
	    clock, "after-paint", G_CALLBACK(+[](GdkFrameClock* /*clock*/, gpointer data) {
		    auto* painted = static_cast<gint64*>(data);
		    if (*painted == 0) {
			    *painted = g_get_monotonic_time();
		    }
	    }),
	    &sample.painted_us);
	gtk_widget_queue_draw(GTK_WIDGET(window));
	if (!KytyUi::WaitFor([&sample]() { return sample.painted_us != 0; }, 2000,
	                     "a painted frame of a toplevel")) {
		return sample;
	}
	if (const auto texture = Snapshot(window); texture) {
		sample.luminance = MeanLuminance(texture.get());
	}
	return sample;
}

bool IsDark(GtkWindow* window, const std::string& what) {
	const auto   texture   = KytyUi::Render(window, what);
	const double luminance = texture ? MeanLuminance(texture.get()) : -1;
	std::printf("%s: %s: mean luminance %.3f\n", KytyUi::CaseName().c_str(), what.c_str(),
	            luminance);
	Check(luminance >= 0, what + " has opaque pixels");
	return luminance >= 0 && luminance < 0.5;
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

GtkWindow* OpenSettings(KytyApplication* application) {
	g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
	GtkWindow* settings = nullptr;
	KytyUi::WaitFor(
	    [&settings]() {
		    settings = MappedSettingsWindow();
		    return settings != nullptr;
	    },
	    5000, "the global settings window");
	if (settings != nullptr) {
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(settings));
	}
	return settings;
}

void CloseSettings(GtkWindow* settings) {
	GtkWidget* cancel = KytyUi::FindFirst(GTK_WIDGET(settings), [](GtkWidget* widget) {
		return GTK_IS_BUTTON(widget) && KytyUi::WidgetText(widget) == "Cancel";
	});
	Check(KytyUi::Activate(cancel), "the settings window's Cancel");
	KytyUi::WaitFor([settings]() { return !KytyUi::IsToplevel(settings); }, 3000,
	                "the settings window to close");
}

// The initial cases: the main window and a dialog opened later have the expected scheme.
void CheckInitialScheme(bool dark) {
	KytyUi::UiContext ui;
	ui.ServeCompatibility();
	const int status =
	    ui.RunApplication([dark](KytyApplication* application, KytyMainWindow* window) {
		    AdwStyleManager* style = adw_style_manager_get_default();
		    Check(adw_style_manager_get_color_scheme(style) == ADW_COLOR_SCHEME_DEFAULT,
		          "the launcher follows the desktop (ADW_COLOR_SCHEME_DEFAULT)");
		    Check((adw_style_manager_get_dark(style) != FALSE) == dark,
		          dark ? "libadwaita is dark" : "libadwaita is light");
		    Check(IsDark(GTK_WINDOW(window), "main window") == dark,
		          dark ? "the main window is dark" : "the main window is light");
		    if (GtkWindow* settings = OpenSettings(application); settings != nullptr) {
			    Check(IsDark(settings, "settings window") == dark,
			          dark ? "a window opened later is dark" : "a window opened later is light");
			    CloseSettings(settings);
		    }
		    g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	    });
	Check(status == 0, "app.quit ends the application with status 0");
}

struct Toplevel {
	std::string         name;
	GtkWindow*          window = nullptr;
	std::vector<double> to_dark_ms;
	std::vector<double> to_light_ms;
};

// One switch: the portal's scheme becomes value; every toplevel's first painted frame with the new
// scheme, in ms after the call (recorded when record).
bool Switch(std::vector<Toplevel>& toplevels, guint32 value, bool record) {
	const bool   dark  = value == 1;
	const gint64 start = g_get_monotonic_time();
	if (!KytyUi::SetPortalColorScheme(value)) {
		return false;
	}
	std::vector<bool> done(toplevels.size(), false);
	const gint64      deadline = start + 3 * G_USEC_PER_SEC;
	bool              all      = false;
	while (!all && g_get_monotonic_time() < deadline) {
		all = true;
		for (std::size_t i = 0; i < toplevels.size(); i++) {
			if (done[i]) {
				continue;
			}
			const Sample sample = NextFrame(toplevels[i].window);
			if (sample.luminance >= 0 && (sample.luminance < 0.5) == dark) {
				done[i]          = true;
				const double ms  = static_cast<double>(sample.painted_us - start) / 1000.0;
				auto&        out = dark ? toplevels[i].to_dark_ms : toplevels[i].to_light_ms;
				if (record) {
					out.push_back(ms);
				}
			} else {
				all = false;
			}
		}
	}
	for (std::size_t i = 0; i < toplevels.size(); i++) {
		Check(done[i], toplevels[i].name + " switches to the " + (dark ? "dark" : "light") +
		                   " scheme within 3 s");
	}
	return all;
}

double Median(std::vector<double> values) {
	std::sort(values.begin(), values.end());
	return values.empty() ? -1 : values[values.size() / 2];
}

void Report(const std::string& name, const char* direction, std::vector<double> values) {
	std::sort(values.begin(), values.end());
	const double median = Median(values);
	std::printf("%s: %s to %s: min %.1f ms, median %.1f ms, max %.1f ms, N %zu\n",
	            KytyUi::CaseName().c_str(), name.c_str(), direction,
	            values.empty() ? -1 : values.front(), median, values.empty() ? -1 : values.back(),
	            values.size());
	Check(values.size() == 5 && median >= 0 && median <= 1000.0,
	      name + " switches to " + direction + " within 1 s (median " + std::to_string(median) +
	          " ms of " + std::to_string(values.size()) + " switches)");
}

void WriteCapture(const Toplevel& toplevel, const char* suffix) {
	if (KytyUi::CaptureDirectory().empty()) {
		return;
	}
	const auto texture = KytyUi::Render(toplevel.window, toplevel.name);
	const auto path    = KytyUi::CaptureDirectory() /
	                     (KytyUi::CaseName() + "-" + toplevel.name + "-" + suffix + ".png");
	Check(texture && gdk_texture_save_to_png(texture.get(), path.c_str()) != FALSE,
	      "write " + path.string());
	std::printf("%s: captured %s\n", KytyUi::CaseName().c_str(), path.c_str());
}

} // namespace

KYTY_UI_CASE(theme_initial_light) {
	CheckInitialScheme(false);
}

KYTY_UI_CASE(theme_initial_dark) {
	CheckInitialScheme(true);
}

KYTY_UI_CASE(theme_prefer_light) {
	CheckInitialScheme(false);
}

KYTY_UI_CASE(theme_live_switch) {
	KytyUi::UiContext ui;
	ui.ServeCompatibility();
	const int status = ui.RunApplication([](KytyApplication* application, KytyMainWindow* window) {
		auto*                   context  = kyty::gtk::ApplicationContext(application);
		GtkWindow*              settings = OpenSettings(application);
		const auto*             type     = kyty::gtk::FindDialogType("confirmation-alert");
		kyty::gtk::DialogHandle handle;
		GtkWindow*              alert = nullptr;
		if (context != nullptr && type != nullptr) {
			handle = type->open(*context, GTK_WINDOW(window), {});
			alert  = KytyUi::FindAlert("Do you want to continue?");
		}
		if (!Check(settings != nullptr && alert != nullptr,
		           "the settings window and an alert are open")) {
			g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
			return;
		}
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(alert));
		std::vector<Toplevel> toplevels = {{"main-window", GTK_WINDOW(window), {}, {}},
		                                   {"settings-window", settings, {}, {}},
		                                   {"alert", alert, {}, {}}};
		for (const auto& toplevel: toplevels) {
			Check(!IsDark(toplevel.window, toplevel.name), toplevel.name + " starts light");
			WriteCapture(toplevel, "before-light");
		}
		// One warm-up round, then 5 measured rounds.
		bool ok = Switch(toplevels, 1, false) && Switch(toplevels, 0, false);
		for (int round = 0; ok && round < 5; round++) {
			ok = Switch(toplevels, 1, true) && Switch(toplevels, 0, true);
		}
		if (ok && KytyUi::SetPortalColorScheme(1) &&
		    KytyUi::WaitFor(
		        []() {
			        return adw_style_manager_get_dark(adw_style_manager_get_default()) != FALSE;
		        },
		        2000, "the dark scheme for the captures")) {
			for (const auto& toplevel: toplevels) {
				Check(IsDark(toplevel.window, toplevel.name), toplevel.name + " ends dark");
				WriteCapture(toplevel, "after-dark");
			}
		}
		KytyUi::SetPortalColorScheme(0);
		for (const auto& toplevel: toplevels) {
			Report(toplevel.name, "dark", toplevel.to_dark_ms);
			Report(toplevel.name, "light", toplevel.to_light_ms);
		}
		KytyUi::CloseAlert(alert, "Cancel");
		type->close(handle);
		CloseSettings(settings);
		g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
	});
	Check(status == 0, "app.quit ends the application with status 0");
}
