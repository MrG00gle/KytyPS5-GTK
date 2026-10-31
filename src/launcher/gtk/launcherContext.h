#pragma once

// What the windows of the GTK launcher share (tasks 19.3-19.5; plan D14). KytyApplication owns the
// one LauncherContext of the process and the real services; the visual harness and the tests build
// their own, with fakes where they need them. Every window and dialog takes a LauncherContext&, so
// none of them owns a copy of the Settings_Store, of the run state, or of a service. A later unit
// adds the game library.

#include "launcher/core/compatibility.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/environment.h"
#include "launcher/core/httpClient.h"
#include "launcher/core/runState.h"
#include "launcher/core/scheduler.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/installation.h"
#include "updater/core/processRunner.h"
#include "updater/core/updatePolicy.h"

#include <filesystem>
#include <memory>
#include <string>

namespace kyty::gtk {

class ControllerService;
class DesktopLauncher;
class DeviceLists;
class FileChooser;
class TitleSoundPlayer;

inline constexpr const char* kApplicationId    = "io.github.MrG00gle.KytyPS5_GTK";
inline constexpr const char* kResourceBasePath = "/io/github/MrG00gle/KytyPS5_GTK";

// The services the windows use (plan D14), by reference: their owner (KytyApplication, the harness,
// a test) keeps them alive longer than the context.
struct Services {
	core::HttpClient&        http;             // services/soupHttpClient.h in the launcher
	core::Scheduler&         scheduler;        // services/glibScheduler.h
	const core::Environment& environment;      // core::MakePosixEnvironment()
	updater::ProcessRunner&  process_runner;   // updater::MakePosixProcessRunner()
	FileChooser&             file_chooser;     // services/fileChooser.h
	DesktopLauncher&         desktop_launcher; // services/desktopLauncher.h
	DeviceLists&             device_lists;     // services/deviceLists.h
	ControllerService&       controller;       // services/controllerService.h
	TitleSoundPlayer&        title_sound;      // services/titleSoundPlayer.h
};

struct LauncherContext {
	explicit LauncherContext(const Services& context_services): services(context_services) {}

	Services            services;
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
// (core::ResolvePaths with services.environment and the Emulator's directory), creates the
// Settings_Store's directory inside the Flatpak_Sandbox (a failure is logged; the first save then
// names the path), and opens the Settings_Store (core::OpenSettingsStore; a load error stays in
// settings.load_error).
std::unique_ptr<LauncherContext> CreateLauncherContext(const LauncherContextOptions& options,
                                                       const Services&               services);

} // namespace kyty::gtk
