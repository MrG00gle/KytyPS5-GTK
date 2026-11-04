#pragma once

// The log pane of the main window (task 20.5; plan D35; Requirements 6.10, 10.5, 10.7): a
// GtkListView over a KytyLogModel (logModel.h, no selection) in a GtkScrolledWindow, with one
// monospace GtkInscription per line (ellipsized at the end; Save log writes the whole lines), whose
// tooltip shows the whole line (phase 7 plan D27). While the model has no row, a dimmed "No output
// yet" lies over the empty list; the first row hides it. New rows scroll the view to the end only
// while it is scrolled to the bottom (as it is while all rows fit, and after it followed the
// previous rows) or when they are the first rows after a clear; once the user scrolls up it stays
// where it is until it is scrolled to the bottom again. The same holds while the pane is hidden
// (phase 7 plan D27): a view that followed the end when it was hidden (or that was never shown, or
// cleared since) shows the newest row when it is shown again, in its first frame, and keeps
// following; a view the user had scrolled up comes back where it was. The list view's accessible
// label is "Emulator log", each row's its line ("Empty line" for an empty one). The main window
// puts Widget() into the end child of its GtkPaned.

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/logModel.h"

#include <gtk/gtk.h>

namespace kyty::gtk {

class LogView {
public:
	explicit LogView(KytyLogModel* model);
	~LogView();

	LogView(const LogView&)            = delete;
	LogView& operator=(const LogView&) = delete;

	// The pane's content: the scrolled window with the list view, and the placeholder over it.
	[[nodiscard]] GtkWidget*   Widget() const { return m_root.get(); }
	[[nodiscard]] GtkListView* ListView() const { return m_list; }
	// The "No output yet" label, visible while the model has no row.
	[[nodiscard]] GtkWidget* Placeholder() const { return m_placeholder; }
	// Whether the view is scrolled to the bottom, so that new rows scroll it to the end.
	[[nodiscard]] bool FollowsEnd() const;

private:
	static void OnItemsChanged(GListModel* model, guint position, guint removed, guint added,
	                           gpointer data);
	static void OnMap(GtkWidget* widget, gpointer data);
	static void OnUnmap(GtkWidget* widget, gpointer data);
	static void OnLayout(GdkFrameClock* clock, gpointer data);

	GObjectPtr<KytyLogModel> m_model;
	GObjectPtr<GtkWidget>    m_root;                  // a GtkOverlay
	GtkWidget*               m_scrolled    = nullptr; // the overlay's child
	GtkListView*             m_list        = nullptr; // a child of m_scrolled
	GtkWidget*               m_placeholder = nullptr; // an overlay of m_root
	SignalScope              m_items_changed;
	SignalScope              m_mapped;
	SignalScope              m_unmapped;
	SignalScope              m_layout;         // the jump to the newest row after the view is shown
	bool                     m_cleared = true; // no row was shown since the last clear
	bool                     m_follows = true; // while hidden: it followed the end when hidden
};

} // namespace kyty::gtk
