#pragma once

// Run-time data locations (task 15.2; design.md "Run-time data locations"; Requirements 12.1, 12.3,
// 12.4, 12.6, 12.7): where the Settings_Store (Kyty.ini) and the Data_Directory are, and the
// launcher data files below the Data_Directory.

#include "launcher/core/environment.h"
#include "launcher/core/expected.h"

#include <filesystem>
#include <functional>
#include <string>

namespace kyty::core {

struct ResolvedPaths {
	std::filesystem::path settings_file; // the Settings_Store, Kyty.ini
	std::filesystem::path data_dir;      // the Data_Directory
	bool                  flatpak = false;
};

// The Settings_Store and the Data_Directory for this session:
// - inside the Flatpak_Sandbox (env.FileExists("/.flatpak-info")): $XDG_CONFIG_HOME/Kyty/Kyty.ini
//   (a Kyty.ini in launcher_cwd is ignored) and $XDG_DATA_HOME/Kyty;
// - outside it: launcher_cwd/Kyty.ini when env.FileExists says it exists (portable mode), else
//   $XDG_CONFIG_HOME/Kyty/Kyty.ini; the Data_Directory is emulator_dir, the directory that contains
//   the Emulator executable.
// As in the Qt_Launcher (QSettings and QStandardPaths of Qt 6): an unset, empty, or relative
// XDG_CONFIG_HOME or XDG_DATA_HOME is ignored and $HOME/.config or $HOME/.local/share is used; an
// unset or empty HOME counts as "/"; a relative HOME or emulator_dir is made absolute against
// launcher_cwd. launcher_cwd should be absolute (the launcher's working directory); a relative one
// is taken relative to "/". Every result is absolute and lexically normal, without a trailing
// separator.
ResolvedPaths ResolvePaths(const Environment& env, const std::filesystem::path& launcher_cwd,
                           const std::filesystem::path& emulator_dir);

// The directory that contains the Settings_Store (created before the first save, Requirement 12.1).
std::filesystem::path SettingsDirectory(const ResolvedPaths& paths);

// Launcher data files and directories directly below the Data_Directory (Requirement 12.6):
// compatibility_cache.json (CompatibilityLoader's cache), _Patches (cheat files), and _SaveData.
// Trophy data is the Emulator's and lives where Common::Trophies::UnlocksPath(data_dir, title_id,
// user_id, service_label) puts it, <data_dir>/_Trophies/<title_id>/
// trophies_<user_id>_<service_label>.json; LoadGameTrophies (trophyModel.h) reads it through that
// function, so the launcher has no accessor of its own for it.
std::filesystem::path CompatibilityCachePath(const std::filesystem::path& data_dir);
std::filesystem::path CheatDirectory(const std::filesystem::path& data_dir);
std::filesystem::path SaveDataDirectory(const std::filesystem::path& data_dir);

// The working directory of the Emulator process: the Data_Directory (Requirement 12.7).
std::filesystem::path SpawnWorkingDirectory(const ResolvedPaths& paths);

// Creates dir and its parents when missing, then proves that it is writable: creates
// .kyty-write-probe-<16 random hexadecimal digits> in it (O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
// a name that exists is tried again with another suffix), writes one byte, closes it, and removes
// it. Nothing else in dir is touched. On failure the error is "<dir>: cannot <step>: <reason>",
// naming the failed step and the errno text (Requirement 12.4); a probe file that was created is
// removed unless removing it is the step that failed.
Expected<void, std::string> EnsureWritableDirectory(const std::filesystem::path& dir);

// The steps of EnsureWritableDirectory, for fault injection in tests.
enum class WritableProbeStep { CreateDirectories, Create, Write, Close, Remove };

// Tests only, not thread-safe: before each step, EnsureWritableDirectory calls fault(step); a
// non-zero return value is the errno that the step then fails with, without performing it (Close
// still releases the descriptor). Create is asked once per attempted name, and EEXIST tries another
// name, as for a real name collision. An empty function removes the hook.
void SetWritableProbeFaultForTesting(std::function<int(WritableProbeStep)> fault);

} // namespace kyty::core
