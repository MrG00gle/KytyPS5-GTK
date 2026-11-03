#pragma once

// GtkAlertDialog helpers (task 19.6; plan D18). Every confirmation of the launcher (clear a
// configuration, remove save data, replace cheats, delete the original folder, import settings,
// overwrite Kyty.ini, quit while a game runs) goes through Confirm; errors through ShowError, and
// errors with a list that may be long through ShowErrorList. Texts are translated by the caller;
// dynamic parts (paths, reasons) go verbatim into the body.

#include <functional>
#include <gtk/gtk.h>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::gtk {

// A modal alert with heading, body, and two buttons, transient for parent (may be nullptr). The
// cancel button is the default button and the one Escape and closing the alert choose. With
// destructive, the accept button has the destructive-action style. callback runs once from the
// main loop with true for the accept button and false otherwise, unless cancellable is cancelled
// first (the alert closes then, and callback never runs). With a cancellable, destroying parent
// counts as cancelling: the alert closes and callback never runs, also when nobody cancels
// cancellable (the alert has its own cancellable, chained to it). When cancellable is already
// cancelled, nothing is created, callback never runs, and the result is nullptr. Returns the
// alert's window (the new toplevel), or nullptr when it could not be found.
GtkWindow* Confirm(GtkWindow* parent, std::string_view heading, std::string_view body,
                   std::string_view accept_label, std::string_view cancel_label, bool destructive,
                   std::function<void(bool accepted)> callback,
                   GCancellable*                      cancellable = nullptr);

// A modal alert with heading, body, and one button per label, left to right (the update offer,
// task 21.6). Escape and closing the alert choose the button at cancel_index; default_index is the
// default button. callback runs once from the main loop with the index of the chosen button,
// unless cancellable is cancelled first; with a cancellable, destroying parent counts as
// cancelling (as for Confirm). When cancellable is already cancelled, nothing is created, callback
// never runs, and the result is nullptr. Returns the alert's window, or nullptr when it could not
// be found.
GtkWindow* Choose(GtkWindow* parent, std::string_view heading, std::string_view body,
                  const std::vector<std::string>& labels, int cancel_index, int default_index,
                  std::function<void(int index)> callback, GCancellable* cancellable = nullptr);

// A modal alert with heading, body, and GTK's own Close button. Cancelling cancellable closes it;
// with a cancellable, destroying parent closes it too (as for Confirm). When cancellable is
// already cancelled, nothing is created and the result is nullptr. Returns the alert's window, or
// nullptr when it could not be found.
GtkWindow* ShowError(GtkWindow* parent, std::string_view heading, std::string_view body,
                     GCancellable* cancellable = nullptr);

// An error with a list of any length (the save data that could not be removed; phase 7 plan D22,
// W3): a modal AdwWindow transient for parent (may be nullptr; destroyed with it), titled heading,
// with heading, the wrapped body, and items one per row (wrapping, selectable text) in a
// scrolled boxed list that grows to at most 320 pixels and then scrolls, and a Close button, the
// default widget; Escape closes it too. Cancelling cancellable closes it. When cancellable is
// already cancelled, nothing is created and the result is nullptr. Returns the window (presented).
GtkWindow* ShowErrorList(GtkWindow* parent, std::string_view heading, std::string_view body,
                         const std::vector<std::string>& items,
                         GCancellable*                   cancellable = nullptr);

} // namespace kyty::gtk
