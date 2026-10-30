#pragma once

// What the windows of the GTK launcher share (task 19.3; plan D14). KytyApplication owns the one
// LauncherContext of the process; the visual harness and the tests build their own. Every window
// and dialog takes a LauncherContext&, so none of them owns a copy of the Settings_Store or of the
// run state. Later units add the game library and the services (HTTP client, scheduler, file
// chooser, desktop launcher, device lists, controller, title sound).

#include "launcher/core/compatibility.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/environment.h"
#include "launcher/core/runState.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/installation.h"
#include "updater/core/updatePolicy.h"

#include <filesystem>
#include <memory>
#include <string>

namespace kyty::gtk {

inline constexpr const char* kApplicationId    = "io.github.MrG00gle.KytyPS5_GTK";
inline constexpr const char* kResourceBasePath = "/io/github/MrG00gle/KytyPS5_GTK";

struct LauncherContext {
	core::ResolvedPaths paths;
	// The Emulator; the version text is "" until EmulatorVersionProbe reports it.
	EmulatorLocation emulator;
	std::string      emulator_version;
	// The one in-memory Settings_Store of the process: windows edit copies of
	// settings.loaded.settings and save through core::SaveSettingsStore (never core::SaveSettings).
	core::SettingsStoreFile  settings;
	core::RunState           run_state;
	core::CompatibilityModel compatibility;
	updater::UpdateUiMode    update_mode = updater::UpdateUiMode::Other;
	// --local: the compatibility database is ./compatibility_db.json, with editable entries.
	bool local = false;

	LauncherContext()                                  = default;
	LauncherContext(const LauncherContext&)            = delete;
	LauncherContext& operator=(const LauncherContext&) = delete;
};

struct LauncherContextOptions {
	std::filesystem::path working_directory;  // absolute: the launcher's current directory
	std::filesystem::path launcher_directory; // the directory of the launcher executable
	updater::UpdateUiMode update_mode = updater::UpdateUiMode::Other;
	bool                  local       = false;
};

// Locates the Emulator (LocateEmulator), resolves the Settings_Store and Data_Directory
// (core::ResolvePaths with the Emulator's directory), creates the Settings_Store's directory inside
// the Flatpak_Sandbox (a failure is logged; the first save then names the path), and opens the
// Settings_Store (core::OpenSettingsStore; a load error stays in settings.load_error).
std::unique_ptr<LauncherContext> CreateLauncherContext(const core::Environment&      environment,
                                                       const LauncherContextOptions& options);

} // namespace kyty::gtk
