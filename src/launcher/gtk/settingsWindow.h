#pragma once

// The settings editor (task 21.1; plan D36-D40; Requirements 7.1, 7.2, 9.1, 9.2, 9.4, 9.5, 9.13,
// 9.14, 11.15, 12.5, 12.8; LFI-15, LFI-17-LFI-20, LFI-22, LFI-32, LFI-34, LFI-47). One window
// serves both editors, modal for its parent:
// - global: the pages General, Network, Debug, Folders (Game_Folders and the add-ons directory,
//   with a warning on every path that contains a backslash), Controller (lightbar color with a
//   live preview through ControllerService, vibration, speaker, keyboard player), Updates (by
//   UpdateUiMode: Official the startup check, Other the releases page, Flatpak no page), and About
//   this installation (Settings_Store, Data_Directory, Emulator path and version), plus the "Game
//   configs" menu: Import applies a Game_Settings_File to every library game whose title ID it
//   has, after one confirmation; Export writes one game's configuration to <TITLE_ID>.json;
// - per game: General, Network, and Debug, opened with the game's effective values
//   (core::EnableCustomSettings, Requirement 9.2) and saved as its custom settings
//   (core::SaveCustomSettings, 9.13).
// The rows come from the core::GameConfigurationFields() descriptors; the output path rows are
// enabled only while their output is on (LFI-19); the GPU and microphone lists come from
// DeviceLists when the window opens (9.5).
//
// The window edits copies. Save validates (core::Validate, ValidationScope::Global or Game; an
// error names the field and keeps the window open, 9.4; invalid input mapping entries are listed
// and can be dropped, Y1), merges only its own part into LauncherContext::settings, and writes the
// Settings_Store through core::SaveSettingsStore: a store that could not be loaded is replaced only
// after the user chooses Overwrite (Y2), and a failed write names the path and the reason (12.8).
// Cancel, Escape, and closing write nothing (9.14). Reset restores the defaults of the fields the
// window shows (global: the default configuration; per game: the Global_Configuration) and keeps
// the folders. The window size and maximized state come from LauncherSettings::settings_window
// ([SettingsWindow], LFI-47); closing keeps the new size in the context's settings, and the next
// write of the Settings_Store stores it.
//
// Signal "saved" (no arguments): emitted after the window wrote the Settings_Store (Save, or a
// Game configs import), so that the library can follow (rescan after a Game_Folder change, custom
// settings markers).

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_SETTINGS_WINDOW (kyty_settings_window_get_type())
G_DECLARE_FINAL_TYPE(KytySettingsWindow, kyty_settings_window, KYTY, SETTINGS_WINDOW, AdwWindow)

G_END_DECLS

// The global settings editor, transient for and modal over parent (may be nullptr). Not presented
// yet; GTK owns the window (gtk_window_destroy or closing it ends it). context must outlive it.
KytySettingsWindow* kyty_settings_window_new_global(kyty::gtk::LauncherContext& context,
                                                    GtkWindow*                  parent);

// The per-game editor for entry (copied).
KytySettingsWindow* kyty_settings_window_new_for_game(kyty::gtk::LauncherContext&  context,
                                                      GtkWindow*                   parent,
                                                      const kyty::core::GameEntry& entry);

// Shows the page named name: "general", "network", "debug", and in the global editor "folders",
// "controller", "updates" (not in Flatpak builds), "about".
void kyty_settings_window_show_page(KytySettingsWindow* window, const char* name);
