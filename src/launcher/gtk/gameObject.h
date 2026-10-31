#pragma once

// The item of the game list (task 20.1; plan D26): KytyGame, a GObject that owns one
// core::GameEntry, its core::GameRow (the values the list view shows and sorts by), and its
// ordinal (the entry's position in the scan result, which breaks sort ties, D27), plus the
// presentation state: the compatibility status and comment, the running flag, the icon texture,
// and the size. Entry and row stay together by construction.
//
// Every property is read-only for GObject and notifies only when its value changes. The status,
// comment, and size setters also update the row (the status as its display text, so the Status
// column sorts as text, Requirement 7.12). A caller that changes them in games of a GameListModel
// calls GameListModel::ColumnChanged() afterwards, so that the sort follows.
//
// Properties: "title" (string), "status" (int, a core::CompatibilityStatus), "comment" (string),
// "running" (boolean), "texture" (GdkTexture, NULL for the placeholder icon), "size" (guint64,
// G_MAXUINT64 while unknown), "metadata-unavailable" (boolean).

#include "launcher/core/compatibility.h"
#include "launcher/core/gameLibrary.h"
#include "launcher/core/gameMetadata.h"

#include <cstdint>
#include <gdk/gdk.h>
#include <glib-object.h>
#include <optional>
#include <string>

G_BEGIN_DECLS

#define KYTY_TYPE_GAME (kyty_game_get_type())
G_DECLARE_FINAL_TYPE(KytyGame, kyty_game, KYTY, GAME, GObject)

G_END_DECLS

// A new game (transfer full) for the entry at position ordinal of the scan result: the row is
// core::RowFromEntry(entry) with the status text of Unknown; no comment, size, or texture; not
// running.
KytyGame* kyty_game_new(kyty::core::GameEntry entry, guint ordinal);

const kyty::core::GameEntry& kyty_game_get_entry(KytyGame* game);
const kyty::core::GameRow&   kyty_game_get_row(KytyGame* game);
guint                        kyty_game_get_ordinal(KytyGame* game);

kyty::core::CompatibilityStatus kyty_game_get_status(KytyGame* game);
void kyty_game_set_status(KytyGame* game, kyty::core::CompatibilityStatus status);

const std::string& kyty_game_get_comment(KytyGame* game);
void               kyty_game_set_comment(KytyGame* game, std::string comment);

bool kyty_game_get_running(KytyGame* game);
void kyty_game_set_running(KytyGame* game, bool running);

// The icon (transfer none); NULL shows the placeholder (Requirement 7.16). The setter takes its
// own reference.
GdkTexture* kyty_game_get_texture(KytyGame* game);
void        kyty_game_set_texture(KytyGame* game, GdkTexture* texture);

// The size in bytes; empty while unknown (LFI-39).
std::optional<uint64_t> kyty_game_get_size(KytyGame* game);
void                    kyty_game_set_size(KytyGame* game, std::optional<uint64_t> size);

// True when the entry's sce_sys/param.json was missing or unusable (Requirement 7.9).
bool kyty_game_get_metadata_unavailable(KytyGame* game);

namespace kyty::gtk {

// The translated display text of a status ("In game", "Main menu", "Logo", "Doesn't boot",
// "Unknown", as the Qt launcher named them).
const char* StatusText(core::CompatibilityStatus status);

// The tooltip of the game's row and grid item (the Qt launcher's lines): the title, then the
// serial, the game version, the firmware version, the path, and the comment when they are not
// empty, and a line when the metadata is unavailable.
std::string GameTooltip(KytyGame* game);

} // namespace kyty::gtk
