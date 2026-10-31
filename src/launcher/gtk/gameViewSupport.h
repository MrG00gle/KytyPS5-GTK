#pragma once

// What the list view and the grid view share (task 20.2; plan D31): the game icon with its
// placeholder and emblems, and GameViewShell, which wraps a GtkColumnView or GtkGridView and gives
// it the context menu (resources/ui/game-menu.xml, LFI-12) and the view-local keys:
// - right-click on an item selects it and pops up the game menu; on empty space it pops up the
//   empty-space menu when one is set (U6: global settings, LFI-13);
// - Shift+F10 and the Menu key pop up the game menu for the selected game;
// - Delete activates game.clear-config, and activating an item (double-click, Enter) selects it
//   and activates game.run.
// The keys are bound with a GtkShortcutController on the view only, never application-wide, so text
// entries keep them. The game.* actions are looked up from the view upwards (the main window
// provides them). The shell also gives every item its tooltip (GameTooltip), from one
// query-tooltip handler on the view.
//
// The view shows the selection model only while it is mapped. GTK keeps about 200 rows of a list
// realized and rebuilds them at every filter change, so a hidden view (the other page of the main
// window's stack) that kept the model would double the cost of a search (Requirement 7.13: 100 ms
// for 1,000 games). When the view is mapped again it scrolls to the selected game, as the Qt
// launcher did when the view changed.

#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"

#include <gtk/gtk.h>
#include <unordered_map>

namespace kyty::gtk {

inline constexpr const char* kGameMenuResource = "/io/github/MrG00gle/KytyPS5_GTK/ui/game-menu.xml";

// The "game-menu" model of resources/ui/game-menu.xml.
GObjectPtr<GMenuModel> LoadGameMenu();

// The icon of a game: the texture or the placeholder (Requirement 7.16), the metadata-unavailable
// emblem (Requirement 7.9), and the running emblem (LFI-11) over it. The overlay and the emblems
// are created the first time a game needs them (the overlay then takes the image's place in its
// parent box): a filter change rebuilds every realized row (about 200), so items stay as small as
// possible (Requirement 7.13).
struct GameIcon {
	GtkWidget* image           = nullptr; // a child of a GtkBox, or of overlay
	GtkWidget* overlay         = nullptr; // nullptr until an emblem is needed
	GtkWidget* metadata_emblem = nullptr; // nullptr until needed
	GtkWidget* running_emblem  = nullptr; // nullptr until needed
};

// The image; the caller appends icon.image to a GtkBox.
GameIcon MakeGameIcon();
// Shows game's texture at texture_size pixels, or the placeholder icon at placeholder_size pixels
// (in a texture_size square), and the emblems the game needs; a null game shows nothing.
void UpdateGameIcon(GameIcon& icon, KytyGame* game, int texture_size, int placeholder_size);

class GameViewShell {
public:
	// view is a new GtkColumnView or GtkGridView without a model; the shell gives it selection
	// while it is mapped and puts it in a scrolled window inside Root(), a GtkBox that also parents
	// the menus.
	GameViewShell(GtkWidget* view, GtkSingleSelection* selection);
	~GameViewShell();

	GameViewShell(const GameViewShell&)            = delete;
	GameViewShell& operator=(const GameViewShell&) = delete;

	[[nodiscard]] GtkWidget*  Root() const { return m_root; }
	[[nodiscard]] GtkWidget*  View() const { return m_view; }
	[[nodiscard]] GtkPopover* GameMenu() const { return GTK_POPOVER(m_game_menu); }

	// Every item widget registers itself, so that a click on it finds its list item (a GtkListItem
	// of the grid view or a GtkColumnViewCell of the list view). anchor marks the widget that
	// keyboard popups point at (one per item).
	void TrackItem(GtkWidget* widget, GObject* item, bool anchor);
	void UntrackItem(GtkWidget* widget);

	// Pops up the game menu for the item at (x, y) in View() coordinates after selecting it, or the
	// empty-space menu when no item is there. False when nothing was shown.
	bool PopupAt(double x, double y);
	// Pops up the game menu for the selected game (or the empty-space menu without a selection).
	bool PopupForSelection();
	// The menu for a click on empty space; nullptr removes it.
	void SetEmptySpaceMenu(GMenuModel* menu);

private:
	static void OnPressed(GtkGestureClick* gesture, int presses, double x, double y, gpointer data);
	static void OnActivate(GtkWidget* view, guint position, gpointer data);
	static gboolean   OnMenuKey(GtkWidget* widget, GVariant* args, gpointer data);
	static gboolean   OnQueryTooltip(GtkWidget* view, int x, int y, gboolean keyboard,
	                                 GtkTooltip* tooltip, gpointer data);
	static void       OnMap(GtkWidget* view, gpointer data);
	static void       OnUnmap(GtkWidget* view, gpointer data);
	static GtkWidget* NewMenu(GMenuModel* model);
	void              Show(GtkWidget* menu, const graphene_rect_t& target);
	// The list item of the tracked widget at or around picked, and that widget.
	GObject* ItemAt(GtkWidget* picked, GtkWidget** widget = nullptr) const;

	// A reference of its own: the widget keeps working when the GameListModel object is gone.
	GObjectPtr<GtkSingleSelection> m_selection;
	GtkWidget*                     m_root       = nullptr; // owned by whoever owns the shell's root
	GtkWidget*                     m_view       = nullptr;
	GtkWidget*                     m_game_menu  = nullptr; // child of m_root
	GtkWidget*                     m_empty_menu = nullptr; // child of m_root, or nullptr
	struct Tracked {
		GObject* item   = nullptr;
		bool     anchor = false;
	};
	std::unordered_map<GtkWidget*, Tracked> m_items;
};

} // namespace kyty::gtk
