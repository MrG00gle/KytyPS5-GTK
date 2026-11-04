#pragma once

// KytyApplication (task 19.3; plan D16): the AdwApplication of the GTK launcher, with the App_ID
// io.github.MrG00gle.KytyPS5_GTK and G_APPLICATION_NON_UNIQUE (as the Qt launcher, several
// launchers may run at once).
//
// - Command line: --local (the compatibility database is ./compatibility_db.json).
// - startup: creates the real services (HTTP, timers, processes, file chooser, desktop launcher,
//   devices, controllers, title sound) and builds the LauncherContext on them (paths, Emulator,
//   Settings_Store); finalize destroys both and shuts SDL down (SDL_Quit). Sets the App_ID as the
//   default window icon name, adds the GResource icons to the icon theme, follows the desktop's
//   color scheme (ADW_COLOR_SCHEME_DEFAULT), registers app.quit and sets the accelerators of the
//   D30 action table (Ctrl+Q quits), starts the Emulator version probe (the main window's footer
//   shows the version when it arrives), and logs "startup complete" (g_debug).
// - activate: creates and presents the main window (task 20.3; mainWindow.h), which adds the other
//   app.* actions, or presents the existing one; "main window presented" (g_debug) is logged when
//   it is first mapped.
// - app.quit stores the main window's size first; while a game runs it asks "Stop the running game
//   and quit?" and quits after the game exited (plan D34, MainWindowRequestQuit). shutdown destroys
//   the main window (and with it the windows it opened) before the context goes away.
// - kyty_application_new_for_testing (phase 7 plan D13): the same application for the UI tests,
//   with their fake services, launcher directory, and update mode.

#include "launcher/gtk/launcherContext.h"
#include "updater/core/updatePolicy.h"

#include <adwaita.h>
#include <filesystem>
#include <optional>

G_BEGIN_DECLS

#define KYTY_TYPE_APPLICATION (kyty_application_get_type())
G_DECLARE_FINAL_TYPE(KytyApplication, kyty_application, KYTY, APPLICATION, AdwApplication)

KytyApplication* kyty_application_new(void);

G_END_DECLS

namespace kyty::gtk {

// The application's context; nullptr before startup. It stays valid through shutdown until the
// application is finalized.
LauncherContext* ApplicationContext(KytyApplication* application);

// SIGINT and SIGTERM quit the running KytyApplication as app.quit does, so the launcher ends with
// status 0 (plan D16), except that a running game is stopped without asking first (plan D34).
// main() installs them before anything else. They stay installed for the whole process lifetime: a
// signal that arrives before startup quits right after it, and one that arrives while the process
// is already ending is caught and changes nothing.
void InstallQuitSignalHandlers();

// What the UI tests replace in a KytyApplication (phase 7 plan D13). An empty member keeps what
// the launcher uses: the real services (ApplicationServices), LauncherDirectory(), and
// updater::CurrentUpdateUiMode().
struct ApplicationTestHooks {
	// Copied by kyty_application_new_for_testing; the services it names must outlive the
	// application.
	const Services*                      services = nullptr;
	std::filesystem::path                launcher_directory;
	std::optional<updater::UpdateUiMode> update_mode;
};

} // namespace kyty::gtk

// A KytyApplication as kyty_application_new() makes it, with the hooks applied at startup. Tests
// only; the launcher never calls it.
KytyApplication* kyty_application_new_for_testing(const kyty::gtk::ApplicationTestHooks& hooks);
