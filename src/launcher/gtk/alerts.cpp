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

// The caller's cancellable is cancelled before the alert opens (its owner is going away): the
// helpers then create nothing and never call back, so no alert outlives its parent.
bool AlreadyCancelled(GCancellable* cancellable) {
	return cancellable != nullptr && g_cancellable_is_cancelled(cancellable) != FALSE;
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

// The life of an alert opened with a cancellable. The alert runs with a GCancellable of its own,
// cancelled when the caller's cancellable is cancelled or when the parent window is destroyed,
// whichever comes first: the alert then closes and its callback does not run, also when the
// parent's owner never cancels. GTK's destroy-with-parent is off for such an alert, because
// destroying the window after its cancellable closed it destroyed it a second time (Gtk-CRITICAL
// gtk_window_destroy: assertion 'GTK_IS_WINDOW (window)' failed, U5 harness). Main thread only.
class AlertLink {
public:
	AlertLink(GtkWindow* parent, GCancellable* owner)
	    : m_own(GObjectPtr<GCancellable>::Adopt(g_cancellable_new())),
	      m_owner(GObjectPtr<GCancellable>::Ref(owner)) {
		if (g_cancellable_is_cancelled(owner) != FALSE) {
			g_cancellable_cancel(m_own.get());
			return;
		}
		m_owner_cancelled = SignalScope::Connect(owner, "cancelled", G_CALLBACK(OnGone), this);
		if (parent != nullptr) {
			m_parent_destroyed = SignalScope::Connect(parent, "destroy", G_CALLBACK(OnGone), this);
		}
	}
	AlertLink(const AlertLink&)            = delete;
	AlertLink& operator=(const AlertLink&) = delete;
	AlertLink(AlertLink&&)                 = delete;
	AlertLink& operator=(AlertLink&&)      = delete;
	~AlertLink()                           = default;

	[[nodiscard]] GCancellable* Cancellable() const { return m_own.get(); }

	// The alert's window closes through the cancellable only.
	static void Attach(GtkWindow* window) {
		if (window != nullptr) {
			gtk_window_set_destroy_with_parent(window, FALSE);
		}
	}

private:
	// The caller's cancellable was cancelled, or the parent is being destroyed. Cancelling can
	// finish the alert at once, which deletes this link, so the cancellable is held by a copy.
	static void OnGone(gpointer /*instance*/, gpointer data) {
		const auto own = static_cast<AlertLink*>(data)->m_own;
		g_cancellable_cancel(own.get());
	}

	GObjectPtr<GCancellable> m_own;
	GObjectPtr<GCancellable> m_owner;
	SignalScope              m_owner_cancelled;
	SignalScope              m_parent_destroyed;
};

std::unique_ptr<AlertLink> LinkFor(GtkWindow* parent, GCancellable* cancellable) {
	return cancellable != nullptr ? std::make_unique<AlertLink>(parent, cancellable) : nullptr;
}

struct ConfirmCall {
	std::function<void(bool accepted)> callback;
	std::unique_ptr<AlertLink>         link;
};

struct ChoiceCall {
	std::function<void(int index)> callback;
	int                            cancel_index = 0;
	std::unique_ptr<AlertLink>     link;
};

struct ErrorCall {
	std::unique_ptr<AlertLink> link;
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

void OnChoiceChosen(GObject* source, GAsyncResult* result, gpointer data) {
	std::unique_ptr<ChoiceCall> call(static_cast<ChoiceCall*>(data));
	GErrorPtr                   error;
	const int                   button =
	    gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, error.Out());
	if (WasCancelled(error)) {
		return;
	}
	if (call->callback) {
		call->callback(button >= 0 ? button : call->cancel_index);
	}
}

void OnErrorClosed(GObject* source, GAsyncResult* result, gpointer data) {
	std::unique_ptr<ErrorCall> call(static_cast<ErrorCall*>(data));
	GErrorPtr                  error;
	static_cast<void>(
	    gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, error.Out()));
}

} // namespace

GtkWindow* Confirm(GtkWindow* parent, std::string_view heading, std::string_view body,
                   std::string_view accept_label, std::string_view cancel_label, bool destructive,
                   std::function<void(bool accepted)> callback, GCancellable* cancellable) {
	if (AlreadyCancelled(cancellable)) {
		return nullptr;
	}
	auto              alert       = NewAlert(heading, body);
	const std::string cancel_text = std::string(cancel_label);
	const std::string accept_text = std::string(accept_label);
	const char*       buttons[]   = {cancel_text.c_str(), accept_text.c_str(), nullptr};
	gtk_alert_dialog_set_buttons(alert.get(), buttons);
	gtk_alert_dialog_set_cancel_button(alert.get(), kCancelButton);
	gtk_alert_dialog_set_default_button(alert.get(), kCancelButton);

	auto call            = std::make_unique<ConfirmCall>();
	call->callback       = std::move(callback);
	call->link           = LinkFor(parent, cancellable);
	GCancellable* own    = call->link ? call->link->Cancellable() : nullptr;
	const auto    before = Toplevels();
	gtk_alert_dialog_choose(alert.get(), parent, own, OnConfirmChosen, call.release());
	GtkWindow* window = NewToplevel(before);
	if (own != nullptr) {
		AlertLink::Attach(window);
	}
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

GtkWindow* Choose(GtkWindow* parent, std::string_view heading, std::string_view body,
                  const std::vector<std::string>& labels, int cancel_index, int default_index,
                  std::function<void(int index)> callback, GCancellable* cancellable) {
	if (AlreadyCancelled(cancellable)) {
		return nullptr;
	}
	auto                     alert = NewAlert(heading, body);
	std::vector<const char*> buttons;
	for (const auto& label: labels) {
		buttons.push_back(label.c_str());
	}
	buttons.push_back(nullptr);
	gtk_alert_dialog_set_buttons(alert.get(), buttons.data());
	gtk_alert_dialog_set_cancel_button(alert.get(), cancel_index);
	gtk_alert_dialog_set_default_button(alert.get(), default_index);

	auto call            = std::make_unique<ChoiceCall>();
	call->callback       = std::move(callback);
	call->cancel_index   = cancel_index;
	call->link           = LinkFor(parent, cancellable);
	GCancellable* own    = call->link ? call->link->Cancellable() : nullptr;
	const auto    before = Toplevels();
	gtk_alert_dialog_choose(alert.get(), parent, own, OnChoiceChosen, call.release());
	GtkWindow* window = NewToplevel(before);
	if (own != nullptr) {
		AlertLink::Attach(window);
	}
	return window;
}

GtkWindow* ShowError(GtkWindow* parent, std::string_view heading, std::string_view body,
                     GCancellable* cancellable) {
	if (AlreadyCancelled(cancellable)) {
		return nullptr;
	}
	auto alert           = NewAlert(heading, body);
	auto call            = std::make_unique<ErrorCall>();
	call->link           = LinkFor(parent, cancellable);
	GCancellable* own    = call->link ? call->link->Cancellable() : nullptr;
	const auto    before = Toplevels();
	gtk_alert_dialog_choose(alert.get(), parent, own, OnErrorClosed, call.release());
	GtkWindow* window = NewToplevel(before);
	if (own != nullptr) {
		AlertLink::Attach(window);
	}
	return window;
}

} // namespace kyty::gtk
