#include "launcher/gtk/logView.h"

#include "launcher/gtk/i18n.h"

namespace kyty::gtk {

namespace {

// A view up to 2 pixels short of the bottom still counts as scrolled to the bottom.
constexpr double kBottomSlack = 2.0;
// The pane's height (the paned keeps it when the window grows, plan D35).
constexpr int kPaneHeight = 200;

void OnSetup(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	auto*      item  = GTK_LIST_ITEM(object);
	GtkWidget* label = GTK_WIDGET(gtk_inscription_new(nullptr));
	gtk_inscription_set_xalign(GTK_INSCRIPTION(label), 0.0F);
	gtk_inscription_set_text_overflow(GTK_INSCRIPTION(label),
	                                  GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END);
	gtk_widget_add_css_class(label, "monospace");
	gtk_widget_add_css_class(label, "log-line");
	gtk_widget_set_hexpand(label, TRUE);
	gtk_list_item_set_child(item, label);
	gtk_list_item_set_activatable(item, FALSE);
	gtk_list_item_set_selectable(item, FALSE);
}

void OnBind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	auto*       item = GTK_LIST_ITEM(object);
	auto*       line = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
	const char* text = line != nullptr ? gtk_string_object_get_string(line) : "";
	GtkWidget*  row  = gtk_list_item_get_child(item);
	gtk_inscription_set_text(GTK_INSCRIPTION(row), text);
	// The whole line, which the row may show ellipsized (phase 7 plan D27).
	gtk_widget_set_tooltip_text(row, *text != '\0' ? text : nullptr);
	gtk_list_item_set_accessible_label(item, *text != '\0' ? text : _("Empty line"));
}

void OnUnbind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	auto*      item = GTK_LIST_ITEM(object);
	GtkWidget* row  = gtk_list_item_get_child(item);
	gtk_inscription_set_text(GTK_INSCRIPTION(row), nullptr);
	gtk_widget_set_tooltip_text(row, nullptr);
}

} // namespace

LogView::LogView(KytyLogModel* model): m_model(GObjectPtr<KytyLogModel>::Ref(model)) {
	GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(&OnSetup), nullptr);
	g_signal_connect(factory, "bind", G_CALLBACK(&OnBind), nullptr);
	g_signal_connect(factory, "unbind", G_CALLBACK(&OnUnbind), nullptr);
	// The selection model takes a reference to the model, and the list view the selection model
	// and the factory (transfer full).
	GtkNoSelection* selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(model)));
	m_list = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory));
	gtk_widget_add_css_class(GTK_WIDGET(m_list), "log-view");
	gtk_accessible_update_property(GTK_ACCESSIBLE(m_list), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               _("Emulator log"), -1);

	m_scrolled     = gtk_scrolled_window_new();
	auto* scrolled = GTK_SCROLLED_WINDOW(m_scrolled);
	gtk_scrolled_window_set_policy(scrolled, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_min_content_height(scrolled, kPaneHeight);
	gtk_scrolled_window_set_child(scrolled, GTK_WIDGET(m_list));

	// The empty pane says that nothing was written yet (phase 7 plan D27).
	m_placeholder = gtk_label_new(_("No output yet"));
	gtk_widget_add_css_class(m_placeholder, "dim-label");
	gtk_widget_set_halign(m_placeholder, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(m_placeholder, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(m_placeholder, FALSE);
	gtk_widget_set_visible(m_placeholder,
	                       g_list_model_get_n_items(G_LIST_MODEL(model)) == 0 ? TRUE : FALSE);

	m_root = GObjectPtr<GtkWidget>::Ref(gtk_overlay_new());
	gtk_overlay_set_child(GTK_OVERLAY(m_root.get()), m_scrolled);
	gtk_overlay_add_overlay(GTK_OVERLAY(m_root.get()), m_placeholder);

	// Connected after the selection model's own handler, so the list view already knows the new
	// rows when OnItemsChanged scrolls to them.
	m_items_changed =
	    SignalScope::Connect(model, "items-changed", G_CALLBACK(&OnItemsChanged), this);
	m_mapped   = SignalScope::Connect(m_list, "map", G_CALLBACK(&OnMap), this);
	m_unmapped = SignalScope::Connect(m_list, "unmap", G_CALLBACK(&OnUnmap), this);
}

LogView::~LogView() {
	m_layout.Disconnect();
	m_unmapped.Disconnect();
	m_mapped.Disconnect();
	m_items_changed.Disconnect();
}

// Before GTK lays the new rows out, the adjustment still describes the rows the user saw.
bool LogView::FollowsEnd() const {
	GtkAdjustment* adjustment =
	    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(m_scrolled));
	const double bottom =
	    gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
	return gtk_adjustment_get_value(adjustment) >= bottom - kBottomSlack;
}

void LogView::OnItemsChanged(GListModel* model, guint /*position*/, guint /*removed*/, guint added,
                             gpointer data) {
	auto*       self  = static_cast<LogView*>(data);
	const guint count = g_list_model_get_n_items(model);
	gtk_widget_set_visible(self->m_placeholder, count == 0 ? TRUE : FALSE);
	if (count == 0) {
		// Cleared for a new run: its rows are followed, wherever the old ones were scrolled to.
		self->m_cleared = true;
		return;
	}
	// A hidden view has no size to scroll in: showing it decides (OnMap).
	if (gtk_widget_get_mapped(GTK_WIDGET(self->m_list)) == FALSE) {
		return;
	}
	if (added > 0 && (self->m_cleared || self->FollowsEnd())) {
		self->m_cleared = false;
		gtk_list_view_scroll_to(self->m_list, count - 1, GTK_LIST_SCROLL_NONE, nullptr);
	}
}

// The pane is hidden: whether it follows the end is decided now, while the adjustment still
// describes what the user saw.
void LogView::OnUnmap(GtkWidget* /*widget*/, gpointer data) {
	auto* self = static_cast<LogView*>(data);
	self->m_layout.Disconnect();
	self->m_follows = self->m_cleared || self->FollowsEnd();
}

// The pane is shown again: a following view jumps to the newest row once it has its size, in the
// layout phase of its first frame (after the window's own layout handler, so the list is
// allocated); the scroll queues another layout, which the frame clock runs before it paints.
void LogView::OnMap(GtkWidget* widget, gpointer data) {
	auto* self = static_cast<LogView*>(data);
	if (!(self->m_cleared || self->m_follows)) {
		return;
	}
	GdkFrameClock* clock = gtk_widget_get_frame_clock(widget);
	if (clock != nullptr) {
		self->m_layout =
		    SignalScope::Connect(clock, "layout", G_CALLBACK(&OnLayout), self, G_CONNECT_AFTER);
	}
}

void LogView::OnLayout(GdkFrameClock* /*clock*/, gpointer data) {
	auto* self = static_cast<LogView*>(data);
	if (gtk_widget_get_height(self->m_scrolled) <= 0) {
		return; // not allocated yet
	}
	self->m_layout.Disconnect();
	const guint count = g_list_model_get_n_items(G_LIST_MODEL(self->m_model.get()));
	if (count > 0) {
		self->m_cleared = false;
		gtk_list_view_scroll_to(self->m_list, count - 1, GTK_LIST_SCROLL_NONE, nullptr);
	}
}

} // namespace kyty::gtk
