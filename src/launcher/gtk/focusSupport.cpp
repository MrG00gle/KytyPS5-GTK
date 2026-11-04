#include "launcher/gtk/focusSupport.h"

namespace kyty::gtk {

void MoveFocusOutOf(GtkWindow* window, GtkWidget* container, GtkWidget* target) {
	GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window));
	if (focus == nullptr ||
	    (focus != container && gtk_widget_is_ancestor(focus, container) == FALSE)) {
		return;
	}
	if (target == nullptr || gtk_widget_grab_focus(target) == FALSE) {
		gtk_root_set_focus(GTK_ROOT(window), nullptr);
	}
}

} // namespace kyty::gtk
