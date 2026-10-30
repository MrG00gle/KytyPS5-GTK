#pragma once

// KytyApplication (task 19.3; plan D16): the AdwApplication of the GTK launcher, with the App_ID
// io.github.MrG00gle.KytyPS5_GTK and G_APPLICATION_NON_UNIQUE (as the Qt launcher, several
// launchers may run at once).
//
// - Command line: --local (the compatibility database is ./compatibility_db.json).
// - startup: builds the LauncherContext (paths, Emulator, Settings_Store), sets the App_ID as the
//   default window icon name, adds the GResource icons to the icon theme, follows the desktop's
//   color scheme (ADW_COLOR_SCHEME_DEFAULT), registers app.quit (Ctrl+Q), starts the Emulator
//   version probe, and logs "startup complete" (g_debug).
// - activate: opens nothing until the main window exists (task 20.3), so the process ends after
//   startup.

#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_APPLICATION (kyty_application_get_type())
G_DECLARE_FINAL_TYPE(KytyApplication, kyty_application, KYTY, APPLICATION, AdwApplication)

KytyApplication* kyty_application_new(void);

G_END_DECLS

namespace kyty::gtk {

// The application's context; nullptr before startup. It stays valid through shutdown until the
// application is finalized.
LauncherContext* ApplicationContext(KytyApplication* application);

// SIGINT and SIGTERM activate app.quit of the running KytyApplication, so the launcher ends with
// status 0 (plan D16). main() installs them before anything else. They stay installed for the
// whole process lifetime: a signal that arrives before startup quits right after it, and one that
// arrives while the process is already ending is caught and changes nothing.
void InstallQuitSignalHandlers();

} // namespace kyty::gtk
