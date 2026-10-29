#pragma once

// The Settings_Store, Kyty.ini (design.md, "Settings_Store (Kyty.ini) schema"; Requirements 7.1,
// 7.2, 7.11, 7.17, 9.2, 9.3, 9.6, 9.7, 9.13, 11.14, 12.8; plan D20). The file is the Qt launcher's
// QSettings INI file, read and written through IniFile, so a store that the Qt launcher wrote loads
// without loss and a rewrite keeps every key this launcher does not model.
//
// Schema (the keys LoadSettings reads; "<field>" is every key of GameConfigurationFields()):
// - [Launcher] game_dirs (string list; the legacy single key game_dir is read when game_dirs is
//   missing or empty, and dropped on save), addons_dir, grid_view, grid_icon_width;
// - [MainDialog] check_updates_on_startup, and theme and geometry, kept as their raw INI text;
// - [LauncherWindow] width, height, maximized (new);
// - [GlobalConfiguration] every <field> except input_mapping_custom, plus the GlobalOnlyFields();
// - [GameConfigurations] size and, for each element N from 1 to size, N\name, N\basedir,
//   N\game_path, N\custom_settings, and N\<field>.
//
// Loading (Requirement 9.6) gives each key that is missing, does not parse as its kind, or breaks
// its field's rule (FieldDescriptor::check, an output path that is empty while its output is on,
// grid_icon_width outside 128-320 or not a multiple of 16, a window size below 1, a controller
// color that is not #RRGGBB) the default value and lists it in LoadReport::defaulted_keys, except:
// - optional keys, which Qt never wrote, give the default without a report when they are missing
//   (and are reported when present but invalid): LauncherWindow/*, GameConfigurations/N/
//   input_mapping_custom, and, in a legacy store (game_dirs missing, game_dir present),
//   Launcher/addons_dir, grid_view, and grid_icon_width;
// - MainDialog/theme and MainDialog/geometry are kept as they are, or stay absent;
// - controller integers are clamped (Requirement 9.12), not defaulted; only an unparsable one is
//   reported;
// - host_input_mapping entries are loaded as stored, and a save keeps them, because Qt-written
//   stores hold entries without '=' (tests/fixtures/settings/qt6-special-values.ini). Validate()'s
//   Name=Value rule applies when the settings editor or an import saves a list that is in use: the
//   global list, or a game's own list while its input_mapping_custom is true (ValidationScope).
//   An invalid stored entry is never passed to the Emulator: the launch arguments (task 15.1)
//   keep only the IsValidMappingEntry entries, and the settings editor can list and drop them
//   (FindInvalidMappingEntries, DropInvalidMappingEntries);
// - a stored custom_settings=false reads as custom, as in the Qt launcher (every stored element is
//   a game with custom settings); only a missing or unparsable value is reported;
// - an element whose game_path is missing or empty cannot belong to a game: it is dropped and only
//   GameConfigurations/N/game_path is reported; of two elements with the same game_path the later
//   one is kept (Qt's QMap::insert); an index above size and an index without any key are not
//   elements (Qt drops them on its next save).
//
// Every other key is unknown and kept: LauncherSettings::unknown holds the keys outside the
// elements (other groups, other keys of the schema groups, such as the Qt launcher's
// GlobalConfiguration/name), and GameSettingsEntry::unknown the other keys of an element (such as
// red_zone_protection_enabled). A key below a schema key ("Launcher/grid_view/x") and an element
// key that no IniFile call reaches are dropped.

#include "launcher/core/atomicFile.h"
#include "launcher/core/expected.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/iniFile.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

inline constexpr int32_t kDefaultGridIconWidth = 224;
inline constexpr int32_t kMinGridIconWidth     = 128;
inline constexpr int32_t kMaxGridIconWidth     = 320;
inline constexpr int32_t kGridIconWidthStep    = 16;

// The largest Kyty.ini that LoadSettings reads.
inline constexpr uint64_t kMaxSettingsFileBytes = uint64_t {16} << 20u;

// 128-320 in steps of 16 (Requirement 7.11).
bool IsValidGridIconWidth(int64_t width);

// [LauncherWindow]: the main window's size and maximized state (LFI-47; the position is the
// compositor's).
struct LauncherWindowState {
	int32_t width     = 1100;
	int32_t height    = 720;
	bool    maximized = false;

	friend bool operator==(const LauncherWindowState&, const LauncherWindowState&) = default;
};

// One element of [GameConfigurations]: a game with custom settings. Every element is custom; a
// game without custom settings has no element.
struct GameSettingsEntry {
	std::string name;    // the title shown when the entry was saved
	std::string basedir; // the game directory or .zar file when the entry was saved
	// The game's identity: GameEntry::path. A relative path comes from an old Qt launcher and is
	// matched against GameEntry::legacy_relative_path until it is rewritten.
	std::string       game_path;
	GameConfiguration configuration;
	// The element's keys outside the schema, keyed by the part after "GameConfigurations/N/".
	std::map<std::string, IniValue> unknown;

	friend bool operator==(const GameSettingsEntry&, const GameSettingsEntry&) = default;
};

struct LauncherSettings {
	// [Launcher]: the Game_Folders as stored (NormalizeGameFolders in gameScanner.h makes them
	// absolute and drops duplicates), the additional content directory, the view, and the icon
	// width.
	std::vector<std::string> game_folders;
	std::string              addons_dir;
	bool                     grid_view       = false;
	int32_t                  grid_icon_width = kDefaultGridIconWidth;
	// [MainDialog]: the update check setting (official builds), and the Qt theme and geometry
	// values as raw INI text (theme "0"; geometry "@ByteArray(...)"), absent when not stored.
	bool                       check_updates_on_startup = true;
	std::optional<std::string> theme;
	std::optional<std::string> geometry;
	LauncherWindowState        window;
	// [GlobalConfiguration]; global.input_mapping_custom is not stored and loads as false.
	GameConfiguration  global;
	ControllerSettings controller;
	// [GameConfigurations], in file order.
	std::vector<GameSettingsEntry> games;
	// Every key outside the schema and outside the elements.
	IniFile unknown;

	friend bool operator==(const LauncherSettings&, const LauncherSettings&) = default;
};

struct LoadReport {
	LauncherSettings settings;
	// The keys that got their default, as "<group>/<key>" or "GameConfigurations/<N>/<key>" (N as
	// in the file), in the order they are checked: [Launcher], [MainDialog], [LauncherWindow],
	// [GlobalConfiguration], then each element.
	std::vector<std::string> defaulted_keys;
};

// The settings that bytes (a Kyty.ini) hold; never fails (see the rules above).
LoadReport ParseSettings(std::string_view bytes);

// The Kyty.ini text for settings: settings.unknown, then every schema key, the GameConfigurations
// elements numbered from 1 in vector order with custom_settings=true, and size. game_dir is not
// written; theme and geometry only when present (and when they are one complete INI value).
std::string SerializeSettings(const LauncherSettings& settings);

// Reads the file (at most kMaxSettingsFileBytes) and parses it. A missing file is an IoError with
// ENOENT: the caller starts with LauncherSettings{}.
Expected<LoadReport, IoError> LoadSettings(const std::filesystem::path& ini);

// Writes SerializeSettings(settings) through WriteFileAtomically: on failure the previous file is
// unchanged (Requirement 12.8). The target is never read; its directory must exist.
Expected<void, IoError> SaveSettings(const std::filesystem::path& ini,
                                     const LauncherSettings&      settings);

// ---- The launcher's Kyty.ini (plan D27)
//
// A Settings_Store that exists but cannot be loaded (a directory, a file over
// kMaxSettingsFileBytes, a parent that is not a directory, a read error) is never replaced without
// an explicit user action: the launcher starts with the default settings, shows load_error, and
// SaveSettingsStore refuses to write over the file until ConfirmOverwrite.

struct SettingsStoreFile {
	std::filesystem::path  path;
	LoadReport             loaded;     // the stored settings, or the defaults
	std::optional<IoError> load_error; // why an existing file could not be loaded
	bool                   overwrite_confirmed = false;
};

// LoadSettings(ini): the loaded settings; for a missing file (ENOENT) the defaults and no error;
// for any other failure the defaults and load_error.
SettingsStoreFile OpenSettingsStore(const std::filesystem::path& ini);

// SaveSettings(store.path, settings). While load_error is set and the overwrite is not confirmed,
// nothing is written and the IoError has the path, load_error's errno, and a message naming the
// path and the reason. A successful save clears load_error.
Expected<void, IoError> SaveSettingsStore(SettingsStoreFile&      store,
                                          const LauncherSettings& settings);

// The user's explicit choice to replace the file that could not be loaded.
void ConfirmOverwrite(SettingsStoreFile& store);

// ---- Custom settings (Requirements 9.2, 9.3, 9.13)

// The element for the game: the one whose game_path equals entry.path, else one whose relative
// game_path equals entry.legacy_relative_path; nullptr when the game has no custom settings.
const GameSettingsEntry* FindGameSettings(const LauncherSettings& settings, const GameEntry& entry);
bool HasCustomSettings(const LauncherSettings& settings, const GameEntry& entry);

// The configuration the per-game editor opens with: the game's custom configuration, or, for a
// game without one, the Global_Configuration with input_mapping_custom false. settings is not
// changed (closing the editor without saving changes nothing, Requirement 9.14).
GameConfiguration EnableCustomSettings(const LauncherSettings& settings, const GameEntry& entry);

// Validates configuration (ValidationScope::Game: its host_input_mapping only while
// input_mapping_custom is true) and stores it as the game's custom configuration: the element found
// by FindGameSettings (or a new one at the end) gets the configuration, name = entry.title_name,
// and basedir = game_path = entry.path (so a legacy relative path becomes absolute); other elements
// for the same game are removed. On a ValidationError settings is unchanged.
Expected<void, ValidationError> SaveCustomSettings(LauncherSettings&        settings,
                                                   const GameEntry&         entry,
                                                   const GameConfiguration& configuration);

// Removes every element for the game, so that it runs with the Global_Configuration and the next
// save writes no element for it. False when there was none.
bool ClearCustomSettings(LauncherSettings& settings, const GameEntry& entry);

// After a scan: each element with a relative game_path that equals the legacy_relative_path of one
// of entries (the first in span order) gets that entry's path, unless another element already has
// it. True when an element changed (the caller then saves).
bool MigrateLegacyGamePaths(LauncherSettings& settings, std::span<const GameEntry> entries);

// ---- What the emulator receives (design.md, "Game_Configuration fields")

struct EffectiveConfiguration {
	// The custom configuration, or the Global_Configuration; host_input_mapping is the per-game
	// list when the custom configuration has input_mapping_custom, else the global list, as stored
	// (the launch arguments drop its invalid entries).
	GameConfiguration                    configuration;
	ControllerSettings                   controller; // always the global one
	std::string                          addons_dir; // the global one
	std::optional<std::filesystem::path> cheat_plan; // as the caller passed it

	friend bool operator==(const EffectiveConfiguration&, const EffectiveConfiguration&) = default;
};

// cheat_plan is the game's _Patches/<TITLE_ID>.json when the caller found that file
// (CheatFilePath, task 16.1); this function does not look at the file system.
EffectiveConfiguration
ResolveEffectiveConfiguration(const LauncherSettings& settings, const GameEntry& entry,
                              std::optional<std::filesystem::path> cheat_plan);

} // namespace kyty::core
