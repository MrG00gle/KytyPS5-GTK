#pragma once

// The trophy details dialog (task 21.3; plan D42; Requirement 11.1; LFI-25), modal for its parent
// (the trophy window), from resources/ui/trophy-details-dialog.xml, ported from the Qt launcher's
// trophy inspector (ShowTrophyInspector in src/launcher/src/trophyViewerDialog.cpp): the trophy's
// icon (grayed while it is locked), its name, its grade, whether it is earned, the earned date in
// local time (yyyy-MM-dd HH:mm:ss), its details, and its reward when the package defines one.
// A hidden trophy that is locked and not revealed (core::TrophyRow::masked) shows the hidden trophy
// image and "Hidden trophy" instead of its name, grade, details, and reward, plus "Show hidden
// trophy", which reveals it in the dialog and, through the revealed callback, in the trophy window.
// Escape and Close close it.

#include "launcher/core/trophyModel.h"

#include <adwaita.h>
#include <functional>

G_BEGIN_DECLS

#define KYTY_TYPE_TROPHY_DETAILS_DIALOG (kyty_trophy_details_dialog_get_type())
G_DECLARE_FINAL_TYPE(KytyTrophyDetailsDialog, kyty_trophy_details_dialog, KYTY,
                     TROPHY_DETAILS_DIALOG, AdwWindow)

G_END_DECLS

// The details of shown (the row as the trophy window shows it), transient for and modal over
// parent (may be nullptr). real is the same trophy revealed (its row from core::BuildTrophyViewer
// with its ID among the revealed ones; equal to shown when shown is not masked). icon is the
// trophy's own icon (transfer none; nullptr when the package has none). revealed (may be empty)
// runs once, with the trophy ID, when the user reveals the trophy. Not presented yet; GTK owns the
// window.
KytyTrophyDetailsDialog* kyty_trophy_details_dialog_new(GtkWindow*                   parent,
                                                        const kyty::core::TrophyRow& shown,
                                                        const kyty::core::TrophyRow& real,
                                                        GdkTexture*                  icon,
                                                        std::function<void(int id)>  revealed);
