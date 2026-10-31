#pragma once

// The list view of the game library (task 20.2; Requirements 6.10, 7.8, 7.9, 7.11, 7.12, 7.16):
// a GtkColumnView over GameListModel::Selection() with the LFI-01 columns Game (icon and name),
// Serial, Game Version, Firmware Version, Size, Path, Status (colored indicator and text), and
// Comments, the Qt launcher's widths, and a per-row tooltip (GameTooltip). Every header sorts
// (LFI-06): the first click on a column sorts ascending, the next one reverses it, and the model's
// sort shows in the header. Each row's accessible label is the title. With editable (--local,
// LFI-21) the Status cell is a drop-down and the Comment cell an entry, for games with a title ID;
// an edit sets the game's status or comment, calls the callback, and sorts again. The context menu,
// the view-local keys, and the model only while the view is mapped come from GameViewShell
// (gameViewSupport.h, D31).
//
// The model must outlive this object, which calls it. The widget may outlive both (inside a
// window): it keeps its own reference to the selection, shows the games, and no longer calls the
// model or the callbacks.

#include "launcher/core/compatibility.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"

#include <functional>
#include <gtk/gtk.h>
#include <string>

namespace kyty::gtk {

struct GameListViewState;

class GameListView {
public:
	struct Callbacks {
		std::function<void(KytyGame*, core::CompatibilityStatus)> status_edited;
		std::function<void(KytyGame*, const std::string&)>        comment_edited;
	};

	GameListView(GameListModel& model, bool editable, Callbacks callbacks = {});
	~GameListView();

	GameListView(const GameListView&)            = delete;
	GameListView& operator=(const GameListView&) = delete;

	// The widget to put in a window: a box with the scrolled GtkColumnView and its menus.
	[[nodiscard]] GtkWidget*     Widget() const { return m_root.get(); }
	[[nodiscard]] GtkColumnView* ColumnView() const;
	[[nodiscard]] GtkPopover*    GameMenu() const;

	// See GameViewShell.
	void SetEmptySpaceMenu(GMenuModel* menu);
	bool PopupMenuAt(double x, double y);
	bool PopupMenuForSelection();

private:
	GObjectPtr<GtkWidget> m_root;
	GameListViewState*    m_state = nullptr; // owned by m_root
};

} // namespace kyty::gtk
