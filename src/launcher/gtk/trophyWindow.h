#pragma once

// The trophy window of a game (task 21.3; plan D42; HC-29; Requirement 11.1; LFI-25), modal for its
// parent, from resources/ui/trophy-window.xml, ported from the Qt launcher's trophy viewer
// (src/launcher/src/trophyViewerDialog.cpp). It reads the game's trophy packages and unlock data
// in a GTask thread (core::LoadGameTrophies with the user ID and the console language of the
// game's effective configuration) and shows:
// - one page per readable package (a switcher above them when there are several): the game icon,
//   the package title, the progress (core::BuildTrophyViewer's grade-weighted percentage, HC-01),
//   the earned and total counts, the count of earned trophies of each grade the package has (the
//   Qt launcher's header), and a row per trophy with its icon (grayed while locked), name, grade,
//   description, and the earned date in local time ("Earned" when no time was recorded);
// - for a hidden trophy that is locked and not revealed, the hidden trophy image and "Hidden
//   trophy" instead, and a Reveal button that shows its real name and description;
// - one "Could not read trophy package <file>." per package that cannot be read (Requirement
//   11.1), above the pages, or as the window's message when no package can be read;
// - "No trophy package found in sce_sys/trophy2." when the game has none.
// Activating a trophy row (click, Enter) opens the trophy details dialog (trophyDetailsDialog.h).
// Escape closes the window.

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_TROPHY_WINDOW (kyty_trophy_window_get_type())
G_DECLARE_FINAL_TYPE(KytyTrophyWindow, kyty_trophy_window, KYTY, TROPHY_WINDOW, AdwWindow)

G_END_DECLS

// The trophies of entry (copied), transient for and modal over parent (may be nullptr). It starts
// reading at once. Not presented yet; GTK owns the window. context must outlive it.
KytyTrophyWindow* kyty_trophy_window_new(kyty::gtk::LauncherContext& context, GtkWindow* parent,
                                         const kyty::core::GameEntry& entry);

// True once the trophies were read and are shown (for the harness and the UI tests).
bool kyty_trophy_window_is_loaded(KytyTrophyWindow* window);
