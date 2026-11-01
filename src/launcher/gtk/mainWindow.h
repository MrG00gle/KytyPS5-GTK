#pragma once

// The main window (task 20.3; plan D29-D33; Requirements 6.2, 6.8, 6.11, 7.3, 7.10, 7.11, 7.17;
// LFI-01-LFI-14, LFI-32, LFI-36, LFI-40, LFI-47; HC-17), an AdwApplicationWindow from
// resources/ui/main-window.xml:
// - the header bar: Run, Stop, Refresh (F5), the grid toggle (Ctrl+G) with the icon width slider
//   (128-320 in steps of 16, shown in the grid view), the search entry (Ctrl+F focuses it, Escape
//   clears it), Global settings (Ctrl+comma), Input mapping (the global mapping), Edit game config
//   (Ctrl+E), Clear game config, Trophy overview, the game menu (resources/ui/game-menu.xml, with
//   the game's own input mapping), and the primary menu;
// - the banners: the Settings_Store could not be loaded (HC-17: its path and reason, nothing is
//   saved until "Allow saving" and a confirmation, core::ConfirmOverwrite), "Emulator not found at
//   <path>" (LFI-40), and "<n> folder(s) could not be read" with a Details alert that names every
//   path and its reason (D33, 7.10);
// - the library: the list and grid views (gameListView.h, gameGridView.h) over
//   LauncherContext::library in a GtkStack, over the selected game's background art (LFI-08) and a
//   translucent scrim; with no Game_Folder, the empty state with "Add game folder" (LFI-36), which
//   opens the global settings on their Folders page;
// - the log pane (task 20.5; logView.h over a KytyLogModel, logModel.h) in a GtkPaned below the
//   library, hidden until app.toggle-log (Ctrl+L) shows it; app.save-log (Ctrl+S) writes its rows
//   to a file chosen through FileChooser::SaveFile (10.8; an error names the file and the reason
//   and changes nothing, 10.13);
// - the footer: the Settings_Store path, the Emulator path, and the Emulator version (LFI-32);
// - a toast overlay for messages.
// Running games is a RunController's (runController.h): game.run (Ctrl+R, Enter, double-click, the
// game menu) runs the selected game, game.stop (Ctrl+Period) stops the running one; errors are
// alerts, the backslash warning (HC-20) a toast.
// Quitting while a game runs (D34): closing the window or app.quit asks "Stop the running game and
// quit?" (MainWindowRequestQuit); on accept the game gets SIGTERM (SIGKILL 5 s later) and the
// window closes or the application quits after the exit; SIGINT and SIGTERM to the launcher stop
// the game the same way without asking.
// The view mode and the icon width (500 ms after the slider stops) are saved when they change, the
// window size and maximized state when the window closes or the application quits (D32, LFI-04,
// LFI-47), all through SaveSettingsAutomatically (nothing is written while the Settings_Store could
// not be loaded and saving is not allowed). The background work is a LibraryController's.
//
// Actions (D30). The window registers the app.* actions on its application (without one, in a
// group of its own named "app", where quit closes the window), and inserts the game.* group (the
// selected game). Actions of later tasks exist but stay disabled: app.trophy-overview,
// app.check-updates (Official builds), app.open-releases-page (Other builds), game.open-folder,
// game.trophies, game.cheats, game.additional-content, game.compress, game.remove-save-data,
// game.clear-config, game.export-settings, game.import-settings. Enabled: app.refresh,
// app.search, app.toggle-grid (stateful), app.global-settings, app.add-game-folder,
// app.input-mapping (the global input mapping editor, inputMappingWindow.h), app.toggle-log
// (stateful), app.save-log, game.edit-config and game.input-mapping (a selected game that is not
// running), game.run (a selected game while core::RunState::CanRun, so no game while one runs or
// an update installs, 10.4), and game.stop (while core::RunState::CanStop). Delete, Enter,
// Shift+F10, and the Menu key are the views' own keys (D31).

#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/logModel.h"

#include <adwaita.h>
#include <functional>

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
class LogView;
class RunController;

// The window's library work, its game.* actions, its run flow, and its log (for the harness and
// the UI tests).
LibraryController& MainWindowLibrary(KytyMainWindow* window);
GActionGroup*      MainWindowGameActions(KytyMainWindow* window);
RunController&     MainWindowRun(KytyMainWindow* window);
KytyLogModel*      MainWindowLog(KytyMainWindow* window);
LogView&           MainWindowLogView(KytyMainWindow* window);

// A game started from the window has not exited yet.
bool MainWindowGameRunning(KytyMainWindow* window);

// Quitting (D34). Without a running game, done runs at once. While one runs: with ask, the window
// first asks "Stop the running game and quit?" (Cancel keeps everything, and done never runs);
// then, or at once without ask (SIGINT, SIGTERM), the game gets SIGTERM and, if it is still alive
// 5 s later, SIGKILL, and done runs from the main loop after it exited. A request while the
// question is open replaces done (without ask it also stops the game at once); one after the user
// agreed stops the game again (SIGTERM, same SIGKILL deadline). done may close the window.
void MainWindowRequestQuit(KytyMainWindow* window, bool ask, std::function<void()> done);

// The accelerators of the D30 action table (app.* and game.*), set on application.
void SetMainWindowAccelerators(GtkApplication* application);

} // namespace kyty::gtk
