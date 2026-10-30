#include "launcher/gtk/alerts.h"

#include "launcher/gtk/gobjectPtr.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {
namespace {

// Button indexes of a confirmation: Cancel first (left), the action second.
constexpr int kCancelButton = 0;
constexpr int kAcceptButton = 1;

std::vector<GtkWindow*> Toplevels() {
	GListModel*             list = gtk_window_get_toplevels();
	std::vector<GtkWindow*> windows;
	const guint             count = g_list_model_get_n_items(list);
	for (guint i = 0; i < count; i++) {
		auto window =
		    GObjectPtr<GtkWindow>::Adopt(static_cast<GtkWindow*>(g_list_model_get_item(list, i)));
		windows.push_back(window.get());
	}
	return windows;
}

// The toplevel that gtk_alert_dialog_choose() created: the one that was not there before.
GtkWindow* NewToplevel(const std::vector<GtkWindow*>& before) {
	for (GtkWindow* window: Toplevels()) {
		if (std::find(before.begin(), before.end(), window) == before.end()) {
			return window;
		}
	}
	return nullptr;
}

// The last button below root (in widget tree order) whose label is label, or nullptr.
// GtkAlertDialog has no API for a button's style, so the accept button is found by its label. The
// alert adds its buttons in the order of gtk_alert_dialog_set_buttons() (Cancel, then the action),
// so the last match is the accept button even when both buttons have the same label.
GtkWidget* FindLastButton(GtkWidget* root, std::string_view label) {
	GtkWidget* found = nullptr;
	if (GTK_IS_BUTTON(root)) {
		const char* text = gtk_button_get_label(GTK_BUTTON(root));
		if (text != nullptr && label == text) {
			found = root;
		}
	}
	for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
	     child            = gtk_widget_get_next_sibling(child)) {
		if (GtkWidget* match = FindLastButton(child, label); match != nullptr) {
			found = match;
		}
	}
	return found;
}

GObjectPtr<GtkAlertDialog> NewAlert(std::string_view heading, std::string_view body) {
	auto alert =
	    GObjectPtr<GtkAlertDialog>::Adopt(gtk_alert_dialog_new("%s", std::string(heading).c_str()));
	gtk_alert_dialog_set_detail(alert.get(), std::string(body).c_str());
	gtk_alert_dialog_set_modal(alert.get(), TRUE);
	return alert;
}

// True for the error of an alert closed through its GCancellable.
bool WasCancelled(const GErrorPtr& error) {
	return error && (g_error_matches(error.get(), GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
	                 g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED));
}

struct ConfirmCall {
	std::function<void(bool accepted)> callback;
};

void OnConfirmChosen(GObject* source, GAsyncResult* result, gpointer data) {
	std::unique_ptr<ConfirmCall> call(static_cast<ConfirmCall*>(data));
	GErrorPtr                    error;
	const int                    button =
	    gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, error.Out());
	if (WasCancelled(error)) {
		return;
	}
	if (call->callback) {
		call->callback(button == kAcceptButton);
	}
}

void OnErrorClosed(GObject* source, GAsyncResult* result, gpointer /*data*/) {
	GErrorPtr error;
	static_cast<void>(
	    gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, error.Out()));
}

} // namespace

GtkWindow* Confirm(GtkWindow* parent, std::string_view heading, std::string_view body,
                   std::string_view accept_label, std::string_view cancel_label, bool destructive,
                   std::function<void(bool accepted)> callback, GCancellable* cancellable) {
	auto              alert       = NewAlert(heading, body);
	const std::string cancel_text = std::string(cancel_label);
	const std::string accept_text = std::string(accept_label);
	const char*       buttons[]   = {cancel_text.c_str(), accept_text.c_str(), nullptr};
	gtk_alert_dialog_set_buttons(alert.get(), buttons);
	gtk_alert_dialog_set_cancel_button(alert.get(), kCancelButton);
	gtk_alert_dialog_set_default_button(alert.get(), kCancelButton);

	const auto before = Toplevels();
	gtk_alert_dialog_choose(alert.get(), parent, cancellable, OnConfirmChosen,
	                        new ConfirmCall {std::move(callback)});
	GtkWindow* window = NewToplevel(before);
	if (destructive) {
		if (window == nullptr) {
			g_debug("confirmation \"%s\": alert window not found, the accept button keeps its "
			        "default style",
			        std::string(heading).c_str());
		} else if (GtkWidget* button = FindLastButton(GTK_WIDGET(window), accept_text);
		           button != nullptr) {
			gtk_widget_add_css_class(button, "destructive-action");
		} else {
			g_debug("confirmation \"%s\": accept button \"%s\" not found, it keeps its default "
			        "style",
			        std::string(heading).c_str(), accept_text.c_str());
		}
	}
	return window;
}

GtkWindow* ShowError(GtkWindow* parent, std::string_view heading, std::string_view body,
                     GCancellable* cancellable) {
	auto       alert  = NewAlert(heading, body);
	const auto before = Toplevels();
	gtk_alert_dialog_choose(alert.get(), parent, cancellable, OnErrorClosed, nullptr);
	return NewToplevel(before);
}

} // namespace kyty::gtk
