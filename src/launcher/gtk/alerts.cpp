#include "launcher/gtk/alerts.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <adwaita.h>
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

// ---- ShowErrorList (phase 7 plan D22)

constexpr int kErrorListWidth     = 480; // the default width and the minimum
constexpr int kErrorListMaxHeight = 320; // of the list; beyond it the list scrolls

// The connection of an error list window to the caller's cancellable, deleted when the window is
// destroyed (by its Close button, Escape, its parent, or the cancellable).
struct ErrorListLink {
	SignalScope cancelled;
	SignalScope destroyed;
};

void OnErrorListCancelled(GCancellable* /*cancellable*/, gpointer window) {
	gtk_window_destroy(GTK_WINDOW(window));
}

void OnErrorListDestroyed(GtkWidget* /*window*/, gpointer data) {
	delete static_cast<ErrorListLink*>(data);
}

GtkWidget* NewWrappingLabel(const std::string& text, float xalign) {
	GtkWidget* label = gtk_label_new(text.c_str());
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), xalign);
	return label;
}

// The list of items: one row each, with selectable text (to copy a path); the rows themselves take
// no focus and do nothing when activated.
GtkWidget* NewItemList(const std::vector<std::string>& items, const std::string& name) {
	GtkWidget* list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               name.c_str(), -1);
	for (const auto& item: items) {
		GtkWidget* label = NewWrappingLabel(item, 0.0F);
		gtk_label_set_selectable(GTK_LABEL(label), TRUE);
		gtk_widget_set_margin_start(label, 12);
		gtk_widget_set_margin_end(label, 12);
		gtk_widget_set_margin_top(label, 8);
		gtk_widget_set_margin_bottom(label, 8);
		GtkWidget* row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_widget_set_focusable(row, FALSE);
		gtk_list_box_append(GTK_LIST_BOX(list), row);
	}
	return list;
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

GtkWindow* ShowErrorList(GtkWindow* parent, std::string_view heading, std::string_view body,
                         const std::vector<std::string>& items, GCancellable* cancellable) {
	if (AlreadyCancelled(cancellable)) {
		return nullptr;
	}
	const std::string title(heading);
	const std::string text(body);
	auto*             window = GTK_WINDOW(adw_window_new());
	gtk_window_set_title(window, title.c_str());
	gtk_window_set_modal(window, TRUE);
	gtk_window_set_transient_for(window, parent);
	gtk_window_set_destroy_with_parent(window, TRUE);
	gtk_window_set_default_size(window, kErrorListWidth, -1);
	gtk_widget_set_size_request(GTK_WIDGET(window), kErrorListWidth, -1);

	GtkWidget* header = adw_header_bar_new();
	adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
	GtkWidget* heading_label = NewWrappingLabel(title, 0.5F);
	gtk_label_set_justify(GTK_LABEL(heading_label), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(heading_label, "title-2");
	GtkWidget* body_label = NewWrappingLabel(text, 0.0F);

	GtkWidget* scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), kErrorListMaxHeight);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), NewItemList(items, text));
	gtk_widget_set_vexpand(scrolled, TRUE);

	GtkWidget* close = gtk_button_new_with_mnemonic(_("_Close"));
	gtk_actionable_set_action_name(GTK_ACTIONABLE(close), "window.close");
	gtk_widget_set_halign(close, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(close, "pill");

	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_box_append(GTK_BOX(box), heading_label);
	gtk_box_append(GTK_BOX(box), body_label);
	gtk_box_append(GTK_BOX(box), scrolled);
	gtk_box_append(GTK_BOX(box), close);
	GtkWidget* toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
	adw_window_set_content(ADW_WINDOW(window), toolbar);
	gtk_window_set_default_widget(window, close);
	gtk_widget_grab_focus(close);

	// Escape closes it, as it closes the GtkAlertDialogs.
	GtkEventController* keys = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(
	    GTK_SHORTCUT_CONTROLLER(keys),
	    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, GdkModifierType(0)),
	                     gtk_named_action_new("window.close")));
	gtk_widget_add_controller(GTK_WIDGET(window), keys);

	if (cancellable != nullptr) {
		auto* link      = new ErrorListLink;
		link->cancelled = SignalScope::Connect(cancellable, "cancelled",
		                                       G_CALLBACK(OnErrorListCancelled), window);
		link->destroyed =
		    SignalScope::Connect(window, "destroy", G_CALLBACK(OnErrorListDestroyed), link);
	}
	gtk_window_present(window);
	return window;
}

} // namespace kyty::gtk
