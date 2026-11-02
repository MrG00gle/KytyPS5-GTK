// Example tests of the alert helpers (src/launcher/gtk/alerts.h; phase 7 plan D4): with a
// cancellable that is already cancelled, Confirm, Choose, and ShowError create nothing, return
// nullptr, and never call back, so no alert outlives the window or dialog that is going away. They
// return before GTK creates anything, so the test needs no display and never calls gtk_init. (The
// same case on a display is a UI test.)

#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gobjectPtr.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace {

using kyty::gtk::GObjectPtr;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "AlertsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

guint ToplevelCount() {
	return g_list_model_get_n_items(gtk_window_get_toplevels());
}

void TestAlreadyCancelled() {
	auto cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	g_cancellable_cancel(cancellable.get());
	Check(ToplevelCount() == 0, "no toplevel before the alerts");

	int        confirmed = 0;
	GtkWindow* confirm   = kyty::gtk::Confirm(
	    nullptr, "Remove the save data?", "The action cannot be undone.", "Remove", "Cancel", true,
	    [&confirmed](bool /*accepted*/) { confirmed++; }, cancellable.get());
	Check(confirm == nullptr, "Confirm returns nullptr for a cancelled cancellable");

	int                            chosen = 0;
	const std::vector<std::string> labels = {"Cancel", "Release notes", "Install update"};
	GtkWindow*                     choose = kyty::gtk::Choose(
	    nullptr, "Version 2.0 is available", "Install it now?", labels, 0, 2,
	    [&chosen](int /*index*/) { chosen++; }, cancellable.get());
	Check(choose == nullptr, "Choose returns nullptr for a cancelled cancellable");

	GtkWindow* error = kyty::gtk::ShowError(nullptr, "The settings could not be saved",
	                                        "No space left on device.", cancellable.get());
	Check(error == nullptr, "ShowError returns nullptr for a cancelled cancellable");

	Check(ToplevelCount() == 0, "no alert window was created");
	Check(g_signal_has_handler_pending(cancellable.get(),
	                                   g_signal_lookup("cancelled", G_TYPE_CANCELLABLE), 0,
	                                   FALSE) == FALSE,
	      "nothing is connected to the cancellable");

	// Whatever a created alert would have scheduled would run here.
	for (int i = 0; i < 20; i++) {
		g_main_context_iteration(nullptr, FALSE);
	}
	Check(confirmed == 0, "Confirm's callback never runs");
	Check(chosen == 0, "Choose's callback never runs");
	Check(ToplevelCount() == 0, "still no alert window after the main loop ran");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("AlertsTests");
	// A GLib critical or warning fails the test.
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestAlreadyCancelled();
	std::printf("AlertsTests: all tests passed\n");
	return 0;
}
