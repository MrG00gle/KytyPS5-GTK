#pragma once

// One drawn widget per cell of the game views (task 22.4, W1; phase 7 plan D24, option A of the
// U5 pause; Requirements 6.10, 7.8, 7.9, 7.13, 7.16). A search that shows all 1,000 games makes
// GTK create, measure, and allocate every realized row again (about 200 list rows, 150 grid
// items) within Requirement 7.13's 100 ms, so each list cell and each grid item is a single widget
// that draws its text with Pango and its icon with GtkSnapshot instead of a box of labels and
// images. What the widgets showed stays:
// - the text in the widget's CSS font and color (the theme's, light or dark; the views' selection
//   and the running title's bold through CSS classes on the cell), ellipsized as the labels were
//   (Text and Title at the end, Path in the middle, the Tile title after two wrapped lines);
// - the icon: the game's texture, fitted into an icon_size square, or the placeholder theme icon
//   (applications-games-symbolic, recolored with the text color and dimmed as the dim-label style
//   class dims it, libadwaita's opacity 0.55) at placeholder_size in that square (7.16);
// - the metadata-unavailable emblem (7.9) and the running emblem (LFI-11), child GtkImages with
//   their CSS classes, tooltips, and accessible labels, created only for a game that needs one and
//   centered on the corner of the drawn icon (its texture's rectangle or the placeholder glyph's;
//   W4-07, D26), kept inside the icon square;
// - the Status cell's colored indicator, a child widget with the status CSS class;
// - accessibility: the accessible role is label and the text is the widget's GtkAccessibleText
//   contents (GTK 4.14), as GtkLabel provides it; the rows and grid items keep their accessible
//   labels (the title) from the views.
// The views' tooltips, context menus, selection, focus, and keys are unchanged (gameViewSupport.h).
//
// Kinds:
// - Text, Path: one line; no width of its own (the column view's fixed column widths size it).
// - Title (the list's Game cell): the icon square, a 10-pixel gap, then the title on one line.
// - Status: the indicator, an 8-pixel gap, then the status text on one line.
// - Tile (a grid item): the icon square (icon_size) centered at the top, a 6-pixel gap, and the
//   title centered below it in at most two lines; the tile is TileWidth() wide (the content box;
//   CSS padding comes on top).
// The single-line kinds are at least one line high in their font; the line height is measured once
// per font and resolution.

#include "launcher/gtk/gameObject.h"

#include <gtk/gtk.h>
#include <string>
#include <string_view>

G_BEGIN_DECLS

#define KYTY_TYPE_GAME_CELL (kyty_game_cell_get_type())
G_DECLARE_FINAL_TYPE(KytyGameCell, kyty_game_cell, KYTY, GAME_CELL, GtkWidget)

G_END_DECLS

namespace kyty::gtk {

enum class GameCellKind { Text, Path, Title, Status, Tile };

// The gap between a Title cell's icon square and its text.
inline constexpr int kGameCellTitleGap = 10;
// The gap between a Status cell's indicator and its text.
inline constexpr int kGameCellStatusGap = 8;
// The gap between a Tile's icon square and its title.
inline constexpr int kGameCellTileGap = 6;

} // namespace kyty::gtk

// A new cell of kind (floating).
GtkWidget* kyty_game_cell_new(kyty::gtk::GameCellKind kind);

kyty::gtk::GameCellKind kyty_game_cell_get_kind(KytyGameCell* cell);

// The text; a change redraws the cell (and, for a Tile, measures it again).
void               kyty_game_cell_set_text(KytyGameCell* cell, std::string_view text);
const std::string& kyty_game_cell_get_text(KytyGameCell* cell);

// Title and Tile: the icon of game (its texture or the placeholder) in an icon_size square, the
// placeholder at placeholder_size, and the emblems game needs; a null game shows no icon and no
// emblem.
void kyty_game_cell_set_icon(KytyGameCell* cell, KytyGame* game, int icon_size,
                             int placeholder_size);
// The texture shown (transfer none), or nullptr for the placeholder or no icon.
GdkTexture* kyty_game_cell_get_texture(KytyGameCell* cell);
// True when the placeholder icon is shown.
bool kyty_game_cell_get_placeholder(KytyGameCell* cell);
// The drawn icon's rectangle in the cell's coordinates (the texture's or the placeholder glyph's),
// once the cell is allocated; false without an icon or before the first allocation.
bool kyty_game_cell_get_icon_bounds(KytyGameCell* cell, graphene_rect_t* bounds);
// The emblem images, or nullptr while the cell has none (they are created when first needed and
// hidden afterwards).
GtkWidget* kyty_game_cell_get_metadata_emblem(KytyGameCell* cell);
GtkWidget* kyty_game_cell_get_running_emblem(KytyGameCell* cell);

// Status: the indicator's status CSS class (one of the views' status-* classes), or nullptr for
// none.
void       kyty_game_cell_set_status_class(KytyGameCell* cell, const char* css_class);
GtkWidget* kyty_game_cell_get_indicator(KytyGameCell* cell);

// Tile: the width of the content (the grid's icon width).
void kyty_game_cell_set_tile_width(KytyGameCell* cell, int width);
