#pragma once

// The trophy overview (task 21.3; plan D42; HC-29; Requirement 11.2; LFI-26), modal for its
// parent, from resources/ui/trophy-overview-window.xml, ported from the Qt launcher's overview
// (TrophyViewerDialog::ShowOverview in src/launcher/src/trophyViewerDialog.cpp). It reads the
// trophy data of every game of LauncherContext::library in a GTask thread (core::LoadGameTrophies
// with each game's effective user ID and console language) and shows, through
// core::BuildTrophyOverview:
// - the earned total and the earned count of each grade over every game (the Qt launcher's
//   header);
// - one row per game with trophy data and none for the others: the game icon, its name (its title
//   ID when the name is empty), its title ID, the grade-weighted percentage (HC-01) with a progress
//   bar, "Earned <earned>/<total>", and the earned count of each grade (Platinum only when the
//   game has a platinum trophy, as Qt);
// - "No games with trophies found in the game folders." when no game has trophy data.
// Activating a row (click, Enter) opens that game's trophy window (trophyWindow.h). Escape closes
// the overview.

#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_TROPHY_OVERVIEW_WINDOW (kyty_trophy_overview_window_get_type())
G_DECLARE_FINAL_TYPE(KytyTrophyOverviewWindow, kyty_trophy_overview_window, KYTY,
                     TROPHY_OVERVIEW_WINDOW, AdwWindow)

G_END_DECLS

// The overview of the games in context.library as they are now, transient for and modal over
// parent (may be nullptr). It starts reading at once. Not presented yet; GTK owns the window.
// context must outlive it.
KytyTrophyOverviewWindow* kyty_trophy_overview_window_new(kyty::gtk::LauncherContext& context,
                                                          GtkWindow*                  parent);

// True once the trophy data was read and is shown (for the harness and the UI tests).
bool kyty_trophy_overview_window_is_loaded(KytyTrophyOverviewWindow* window);
