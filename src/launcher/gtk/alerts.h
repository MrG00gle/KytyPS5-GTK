#pragma once

// GtkAlertDialog helpers (task 19.6; plan D18). Every confirmation of the launcher (clear a
// configuration, remove save data, replace cheats, delete the original folder, import settings,
// overwrite Kyty.ini, quit while a game runs) goes through Confirm; errors through ShowError.
// Texts are translated by the caller; dynamic parts (paths, reasons) go verbatim into the body.

#include <functional>
#include <gtk/gtk.h>
#include <string_view>

namespace kyty::gtk {

// A modal alert with heading, body, and two buttons, transient for parent (may be nullptr). The
// cancel button is the default button and the one Escape and closing the alert choose. With
// destructive, the accept button has the destructive-action style. callback runs once from the
// main loop with true for the accept button and false otherwise, unless cancellable is cancelled
// first (the alert closes then, and callback never runs). With a cancellable, destroying parent
// counts as cancelling: the alert closes and callback never runs, also when nobody cancels
// cancellable (the alert has its own cancellable, chained to it). Returns the alert's window (the
// new toplevel), or nullptr when it could not be found.
GtkWindow* Confirm(GtkWindow* parent, std::string_view heading, std::string_view body,
                   std::string_view accept_label, std::string_view cancel_label, bool destructive,
                   std::function<void(bool accepted)> callback,
                   GCancellable*                      cancellable = nullptr);

// A modal alert with heading, body, and GTK's own Close button. Cancelling cancellable closes it;
// with a cancellable, destroying parent closes it too (as for Confirm). Returns the alert's
// window, or nullptr when it could not be found.
GtkWindow* ShowError(GtkWindow* parent, std::string_view heading, std::string_view body,
                     GCancellable* cancellable = nullptr);

} // namespace kyty::gtk
