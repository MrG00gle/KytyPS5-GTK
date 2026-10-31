#pragma once

// The game picker (task 21.1; plan D39; LFI-17): a modal window that lists library games and
// returns the one the user chooses, for the "Game configs" export of the settings window. It
// lists the games with a non-empty title ID (a Game_Settings_File is keyed by title ID), sorted by
// the normalized title ID, then the title, then the path; each row shows the title (or the title
// ID when the title is empty), the title ID, and the path. Activating a row or the accept button
// chooses the selected game; Cancel, Escape, and closing choose nothing.

#include "launcher/core/gameMetadata.h"

#include <adwaita.h>
#include <functional>
#include <span>
#include <string>
#include <vector>

G_BEGIN_DECLS

#define KYTY_TYPE_GAME_PICKER_DIALOG (kyty_game_picker_dialog_get_type())
G_DECLARE_FINAL_TYPE(KytyGamePickerDialog, kyty_game_picker_dialog, KYTY, GAME_PICKER_DIALOG,
                     AdwWindow)

G_END_DECLS

namespace kyty::gtk {

// Runs once, with a copy of the chosen entry, after the dialog closed; never when nothing was
// chosen.
using GamePickedFn = std::function<void(const core::GameEntry& entry)>;

// The games of entries that can be picked (a non-empty title ID after trimming), in the order the
// dialog lists them.
std::vector<core::GameEntry> PickableGames(std::span<const core::GameEntry> entries);

} // namespace kyty::gtk

// A picker over entries (copied), transient for and modal over parent (may be nullptr), with the
// window title title, the explanation description above the list, and the accept button label
// accept_label (all translated by the caller). Not presented yet; GTK owns the window.
KytyGamePickerDialog*
kyty_game_picker_dialog_new(GtkWindow* parent, std::span<const kyty::core::GameEntry> entries,
                            const std::string& title, const std::string& description,
                            const std::string& accept_label, kyty::gtk::GamePickedFn picked);
