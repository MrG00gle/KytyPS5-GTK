#pragma once

// Keyboard focus around rebuilt lists (phase 6 FEAT-010 (5), FEAT-012 (11); phase 7 plan D29).

#include <gtk/gtk.h>

namespace kyty::gtk {

// Moves window's keyboard focus to target (or nowhere when target is nullptr or cannot take it)
// when the focus is container or inside it, before container's children are removed. Otherwise GTK
// moves the focus away from the removed widget only after the next frame, and when that move lands
// on a selectable label (a status line next to the list), GTK 4.22's gtk_label_grab_focus reads the
// removed widget after the window dropped its last reference (Gtk-CRITICAL gtk_widget_is_ancestor:
// assertion 'GTK_IS_WIDGET (widget)' failed).
void MoveFocusOutOf(GtkWindow* window, GtkWidget* container, GtkWidget* target);

} // namespace kyty::gtk
