#pragma once

// The main window (task 20.3; plan D29-D33; Requirements 6.2, 6.8, 6.11, 7.3, 7.10, 7.11, 7.17;
// LFI-01-LFI-14, LFI-32, LFI-36, LFI-40, LFI-47; HC-17), an AdwApplicationWindow from
// resources/ui/main-window.xml:
// - the header bar: Run, Stop, Refresh (F5), the grid toggle (Ctrl+G) with the icon width slider
//   (128-320 in steps of 16, shown in the grid view), the search entry (Ctrl+F focuses it, Escape
//   clears it), Global settings (Ctrl+comma), Input mapping, Edit game config (Ctrl+E), Clear game
//   config, Trophy overview, the game menu (resources/ui/game-menu.xml), and the primary menu;
// - the banners: the Settings_Store could not be loaded (HC-17: its path and reason, nothing is
//   saved until "Allow saving" and a confirmation, core::ConfirmOverwrite), "Emulator not found at
//   <path>" (LFI-40), and "<n> folder(s) could not be read" with a Details alert that names every
//   path and its reason (D33, 7.10);
// - the library: the list and grid views (gameListView.h, gameGridView.h) over
//   LauncherContext::library in a GtkStack, over the selected game's background art (LFI-08) and a
//   translucent scrim; with no Game_Folder, the empty state with "Add game folder" (LFI-36), which
//   opens the global settings on their Folders page;
// - the footer: the Settings_Store path, the Emulator path, and the Emulator version (LFI-32);
// - a toast overlay for messages.
// The view mode and the icon width (500 ms after the slider stops) are saved when they change, the
// window size and maximized state when the window closes or the application quits (D32, LFI-04,
// LFI-47), all through SaveSettingsAutomatically (nothing is written while the Settings_Store could
// not be loaded and saving is not allowed). The background work is a LibraryController's.
//
// Actions (D30). The window registers the app.* actions on its application (without one, in a
// group of its own named "app", where quit closes the window), and inserts the game.* group (the
// selected game). Actions of later tasks exist but stay disabled: app.input-mapping,
// app.trophy-overview, app.toggle-log, app.save-log, app.check-updates (Official builds),
// app.open-releases-page (Other builds), game.run, game.stop, game.open-folder, game.trophies,
// game.cheats, game.additional-content, game.compress, game.remove-save-data, game.clear-config,
// game.input-mapping, game.export-settings, game.import-settings. Enabled: app.refresh,
// app.search, app.toggle-grid (stateful), app.global-settings, app.add-game-folder, and
// game.edit-config (a selected game that is not running). Delete, Enter, Shift+F10, and the Menu
// key are the views' own keys (D31).

#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_MAIN_WINDOW (kyty_main_window_get_type())
G_DECLARE_FINAL_TYPE(KytyMainWindow, kyty_main_window, KYTY, MAIN_WINDOW, AdwApplicationWindow)

G_END_DECLS

// The main window over context (which must outlive it) for application (nullptr without one: the
// registry's main-window for the UI tests and the screenshot harness). It starts the library work
// (the compatibility database and the first scan). Not presented yet; GTK owns the window.
KytyMainWindow* kyty_main_window_new(kyty::gtk::LauncherContext& context,
                                     GtkApplication*             application);

// The Emulator version text in the context changed: the footer shows it.
void kyty_main_window_update_footer(KytyMainWindow* window);

// Stores the window size and maximized state in the Settings_Store (D32), together with every
// change that is only in memory (LauncherContext::stored_settings), and writes nothing when the
// file already holds it all. Closing the window does it by itself; app.quit calls it before the
// application quits.
void kyty_main_window_save_state(KytyMainWindow* window);

namespace kyty::gtk {

class LibraryController;

// The window's library work and its game.* actions (for the harness and the UI tests).
LibraryController& MainWindowLibrary(KytyMainWindow* window);
GActionGroup*      MainWindowGameActions(KytyMainWindow* window);

// The accelerators of the D30 action table (app.* and game.*), set on application.
void SetMainWindowAccelerators(GtkApplication* application);

} // namespace kyty::gtk
