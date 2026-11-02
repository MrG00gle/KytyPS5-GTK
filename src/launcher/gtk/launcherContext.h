#pragma once

// What the windows of the GTK launcher share (tasks 19.3-19.5; plan D14). KytyApplication owns the
// one LauncherContext of the process and the real services; the visual harness and the tests build
// their own, with fakes where they need them. Every window and dialog takes a LauncherContext&, so
// none of them owns a copy of the Settings_Store, of the run state, of the game library, or of a
// service.

#include "launcher/core/compatibility.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/environment.h"
#include "launcher/core/httpClient.h"
#include "launcher/core/runState.h"
#include "launcher/core/saveData.h"
#include "launcher/core/scheduler.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/installation.h"
#include "updater/core/processRunner.h"
#include "updater/core/updatePolicy.h"

#include <filesystem>
#include <memory>
#include <optional>
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
	// settings.loaded.settings and save through WriteSettingsStore (core::SaveSettingsStore, never
	// core::SaveSettings).
	core::SettingsStoreFile  settings;
	// What the Settings_Store holds as far as the launcher knows: the settings OpenSettingsStore
	// loaded (the defaults when the file does not exist or could not be loaded), then those of
	// every successful WriteSettingsStore. settings.loaded.settings is ahead of it while a change
	// is only in memory (an automatic save that was skipped or failed, the settings window's
	// size); the main window's close-time save writes such changes (plan D32, D40).
	core::LauncherSettings   stored_settings;
	core::RunState           run_state;
	core::CompatibilityModel compatibility;
	// The game library of the last scan (empty until the first scan replaces it). The settings
	// window's Game configs import and export list its games.
	GameListModel            library;
	updater::UpdateUiMode    update_mode = updater::UpdateUiMode::Other;
	// This installation for the update UI (task 21.6; HC-05, HC-34): the launcher's directory (the
	// installation directory) and the directory it started in, both absolute in the launcher, and
	// this build's release tag (updater::CurrentBuildFlags().release_tag). The harness and the UI
	// tests replace them.
	std::filesystem::path launcher_directory;
	std::filesystem::path working_directory;
	std::string           release_tag;
	// What deletes the save data of a game (task 21.7, core::RemoveSaveData): the host's
	// std::filesystem::remove; the harness and the UI tests inject failures through it.
	core::PathRemover* path_remover = &core::HostPathRemover();
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
// (core::ResolvePaths with services.environment and the Emulator's directory), keeps the launcher
// and working directories and this build's release tag for the update UI, creates the
// Settings_Store's directory inside the Flatpak_Sandbox (a failure is logged; the first save then
// names the path), and opens the Settings_Store (core::OpenSettingsStore; a load error stays in
// settings.load_error).
std::unique_ptr<LauncherContext> CreateLauncherContext(const LauncherContextOptions& options,
                                                       const Services&               services);

// core::SaveSettingsStore(context.settings, settings); on success context.stored_settings becomes
// settings (the caller updates settings.loaded.settings). Every write of the Settings_Store goes
// through here.
core::Expected<void, core::IoError> WriteSettingsStore(LauncherContext&              context,
                                                       const core::LauncherSettings& settings);

// An automatic save of the launcher's own state (plan D32: view mode, icon width, window size, the
// migrated game paths): next becomes the in-memory settings, then the Settings_Store is written
// through WriteSettingsStore. While the store could not be loaded and the user has not allowed
// overwriting it (HC-17), nothing is written, quietly. The error of a failed write, or nullopt. A
// skipped or failed save leaves next ahead of stored_settings, so the next save writes it.
std::optional<core::IoError> SaveSettingsAutomatically(LauncherContext&       context,
                                                       core::LauncherSettings next);

} // namespace kyty::gtk
