#pragma once

// The log pane of the main window (task 20.5; plan D35; Requirements 6.10, 10.5, 10.7): a
// GtkListView over a KytyLogModel (logModel.h, no selection) in a GtkScrolledWindow, with one
// monospace GtkInscription per line (ellipsized at the end; Save log writes the whole lines). New
// rows scroll the view to the end only while it is scrolled to the bottom (as it is while all rows
// fit, and after it followed the previous rows) or when they are the first rows after a clear;
// once the user scrolls up it stays where it is until it is scrolled to the bottom again. The
// list view's accessible label is "Emulator
// log", each row's its line ("Empty line" for an empty one). The main window puts Widget() into
// the end child of its GtkPaned.

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

	// The scrolled window with the list view.
	[[nodiscard]] GtkWidget*   Widget() const { return m_scrolled.get(); }
	[[nodiscard]] GtkListView* ListView() const { return m_list; }
	// Whether the view is scrolled to the bottom, so that new rows scroll it to the end.
	[[nodiscard]] bool FollowsEnd() const;

private:
	static void OnItemsChanged(GListModel* model, guint position, guint removed, guint added,
	                           gpointer data);

	GObjectPtr<KytyLogModel> m_model;
	GObjectPtr<GtkWidget>    m_scrolled;
	GtkListView*             m_list = nullptr; // a child of m_scrolled
	SignalScope              m_items_changed;
	bool                     m_cleared = true; // no row was shown since the last clear
};

} // namespace kyty::gtk
