#pragma once

// The per-game actions of the main window that open no window of their own (task 21.7; plan D30;
// Requirements 9.3, 9.8-9.10, 9.15, 10.10, 11.13, 11.18; LFI-16, LFI-30, LFI-31, LFI-45; HC-19,
// HC-32), ported from the Qt launcher's open_game_folder, delete_configuartion, and
// remove_save_data (src/launcher/src/configurationListWidget.cpp):
// - OpenGameFolder (game.open-folder): the file manager at a directory Game_Entry, or at the
//   folder that holds a .zar Game_Entry with the archive selected (DesktopLauncher::RevealFile;
//   when the desktop cannot select it, the folder is opened, as Qt did);
// - ClearGameConfig (game.clear-config): after a confirmation, the game's custom settings leave
//   the Settings_Store (core::ClearCustomSettings), so every later run uses the
//   Global_Configuration;
// - RemoveGameSaveData (game.remove-save-data): a confirmation that names the game and lists its
//   save data directory (core::ListSaveData; "No save data folder found for this game." without
//   one), then core::RemoveSaveData through LauncherContext::path_remover; every path that could
//   not be deleted is listed with its reason. Never while the game runs (11.18);
// - ExportGameConfig (game.export-settings): the configuration the game runs with (its custom
//   settings, else the global ones) to a Game_Settings_File chosen through FileChooser::SaveFile,
//   suggested as <TITLE_ID>.json; a failure names the file and changes nothing there (9.15);
// - ImportGameConfig (game.import-settings): a Game_Settings_File chosen through
//   FileChooser::OpenFile; the member for the game's title ID, or, after a confirmation, the only
//   member of a file exported for another title (core::SelectGameImport), set over the game's
//   current configuration and saved as its custom settings (9.9); an error names the setting or
//   says why the file was rejected, and nothing changes (9.10, HC-19).
// The settings are written through SaveSettingsInteractively (settingsWrite.h): a Settings_Store
// that could not be loaded is replaced only after the user chooses Overwrite. The alerts are modal
// over parent; once cancellable is cancelled (the main window goes away), they close and nothing
// runs any more.

#include "launcher/core/gameMetadata.h"
#include "launcher/core/saveData.h"
#include "launcher/gtk/launcherContext.h"

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <string_view>
#include <vector>

namespace kyty::gtk {

// The game has a title ID (after core::NormalizeTitleId), which a Game_Settings_File needs.
bool CanExportGameConfig(const core::GameEntry& entry);

void OpenGameFolder(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                    GCancellable* cancellable);

// changed runs after the Settings_Store was written.
void ClearGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                     GCancellable* cancellable, std::function<void()> changed);

void RemoveGameSaveData(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                        GCancellable* cancellable);

void ExportGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                      GCancellable* cancellable);

// changed runs after the Settings_Store was written.
void ImportGameConfig(LauncherContext& context, GtkWindow* parent, const core::GameEntry& entry,
                      GCancellable* cancellable, std::function<void()> changed);

// The alerts of these actions (the registry opens them with sample data). The confirmations call
// done(true) for the accept button and done(false) otherwise; they return the alert's window, or
// nullptr when it could not be found (alerts.h).
GtkWindow* ConfirmClearGameConfig(GtkWindow* parent, const core::GameEntry& entry,
                                  std::function<void(bool accepted)> done,
                                  GCancellable*                      cancellable);
GtkWindow* ConfirmRemoveSaveData(GtkWindow* parent, const core::GameEntry& entry,
                                 const std::vector<std::filesystem::path>& directories,
                                 std::function<void(bool accepted)>        done,
                                 GCancellable*                             cancellable);
// Every path that could not be deleted, with its reason (11.13).
GtkWindow* ShowSaveDataFailures(GtkWindow* parent, const core::GameEntry& entry,
                                const std::vector<core::RemovalFailure>& failures,
                                GCancellable*                            cancellable);
// The file's only member was exported for member_title, which is not the game's title ID.
GtkWindow* ConfirmForeignGameConfig(GtkWindow* parent, const core::GameEntry& entry,
                                    std::string_view                   member_title,
                                    std::function<void(bool accepted)> done,
                                    GCancellable*                      cancellable);

} // namespace kyty::gtk
