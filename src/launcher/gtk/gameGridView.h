#pragma once

// The grid view of the game library (task 20.2; Requirements 6.10, 7.8, 7.11, 7.16; LFI-02,
// LFI-03): a GtkGridView over GameListModel::Selection() (the list view's selection) that shows
// each game's icon and title, with the placeholder icon, the metadata-unavailable and running
// emblems, the GameTooltip, and the title as the item's accessible label. The icon width goes from
// 128 to 320 pixels in steps of 16 (default 224); as in the Qt launcher, the icon area is
// width x width * 9 / 16 and the square icon fills its height. The main window's slider sets it.
// The context menu, the view-local keys, and the model only while the view is mapped come from
// GameViewShell (gameViewSupport.h, D31).
//
// The widget keeps its own reference to the selection, so it may outlive this object and the model
// (inside a window).

#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gobjectPtr.h"

#include <gtk/gtk.h>

namespace kyty::gtk {

struct GameGridViewState;

class GameGridView {
public:
	static constexpr int kMinIconWidth     = 128;
	static constexpr int kMaxIconWidth     = 320;
	static constexpr int kIconWidthStep    = 16;
	static constexpr int kDefaultIconWidth = 224;

	// width clamped to [kMinIconWidth, kMaxIconWidth] and rounded to the nearest step.
	static int SnapIconWidth(int width);

	explicit GameGridView(GameListModel& model);
	~GameGridView();

	GameGridView(const GameGridView&)            = delete;
	GameGridView& operator=(const GameGridView&) = delete;

	// The widget to put in a window: a box with the scrolled GtkGridView and its menus.
	[[nodiscard]] GtkWidget*   Widget() const { return m_root.get(); }
	[[nodiscard]] GtkGridView* GridView() const;
	[[nodiscard]] GtkPopover*  GameMenu() const;

	// Applies SnapIconWidth(width) to every item.
	void              SetIconWidth(int width);
	[[nodiscard]] int IconWidth() const;

	// See GameViewShell.
	void SetEmptySpaceMenu(GMenuModel* menu);
	bool PopupMenuAt(double x, double y);
	bool PopupMenuForSelection();

private:
	GObjectPtr<GtkWidget> m_root;
	GameGridViewState*    m_state = nullptr; // owned by m_root
};

} // namespace kyty::gtk
