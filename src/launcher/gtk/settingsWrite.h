#pragma once

// Explicit saves of the Settings_Store from an editor window (Y2, HC-16, Requirement 12.8): the
// settings window (task 21.1) and the input mapping editor (task 21.2) write through here, so both
// ask before replacing a Kyty.ini that could not be loaded and word their errors the same way.

#include "launcher/core/atomicFile.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/launcherContext.h"

#include <functional>
#include <gtk/gtk.h>
#include <string>

namespace kyty::gtk {

// Why the store's file is not replaced while store.load_error is set: its path and the reason,
// and what Overwrite does. A path below a regular file (ENOTDIR) names the entry that is not a
// directory (HC-16).
std::string SettingsRefusalText(const core::SettingsStoreFile& store);

// A failed write: "The settings file <path> was not changed:" and the reason, or the ENOTDIR
// wording ("<parent> is not a directory, so the settings file cannot be created there.").
std::string SettingsSaveErrorText(const core::IoError& error);

// Writes next as the Settings_Store (WriteSettingsStore). On success
// context.settings.loaded.settings becomes next, then saved runs. While the store could not be
// loaded and overwriting it is not confirmed, the user is asked first ("The settings file could
// not be loaded", Overwrite and Cancel); Overwrite confirms it (core::ConfirmOverwrite) and writes
// again. Any other failure is an error alert with the path and the reason, and nothing changes.
// The alerts are modal over parent and go away without calling anything when cancellable is
// cancelled (the caller cancels it when it goes away).
void SaveSettingsInteractively(GtkWindow* parent, LauncherContext& context,
                               core::LauncherSettings next, GCancellable* cancellable,
                               std::function<void()> saved);

} // namespace kyty::gtk
