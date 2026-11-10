#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_

#include "emulator.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Emulator {

enum class OptionKind {
	Flag,     // Takes no value.
	Value,    // Takes the next argument as its value.
	Help,     // --help and -h.
	Terminal, // --guest-exec: takes the next argument and passes every later one to the guest.
};

struct OptionSpec {
	std::string_view name;
	OptionKind       kind;
	std::string_view value_hint;
	std::string_view help;
};

// The single option table. ParseOptions and UsageText both read it.
std::span<const OptionSpec> Options();

struct ParseOutcome {
	enum Status { Run, Help, Error } status;
	RunOptions  options;
	std::string error;
};

// args holds argv[1..argc). No arguments gives Help.
ParseOutcome ParseOptions(std::span<const std::string> args);

// The synopsis followed by one line per Options() entry.
std::string UsageText();

// What ApplyDataDirectory reads from and does to the process; tests pass their own.
struct DataDirectoryEnvironment {
	bool                       flatpak = false; // Inside the Flatpak_Sandbox (/.flatpak-info).
	std::optional<std::string> xdg_data_home;   // XDG_DATA_HOME; nullopt when unset.
	std::optional<std::string> home;            // HOME; nullopt when unset.
	std::filesystem::path      cwd;             // The original working directory.
	// Each returns 0 or the errno of the failure.
	std::function<int(const std::filesystem::path&)> create_directories;
	std::function<int(const std::filesystem::path&)> change_directory;
};

// This process: /.flatpak-info, XDG_DATA_HOME, HOME, the working directory (empty when it cannot
// be read), std::filesystem::create_directories, and chdir().
DataDirectoryEnvironment CurrentDataDirectoryEnvironment();

// The Data_Directory inside the Flatpak_Sandbox, $XDG_DATA_HOME/Kyty, by the launcher's rule
// (kyty::core::ResolvePaths): an unset, empty, or relative XDG_DATA_HOME gives
// $HOME/.local/share, an unset or empty HOME is "/", and a relative HOME or cwd is made absolute
// against env.cwd or "/". The result is absolute and lexically normal, without a trailing
// separator.
std::filesystem::path FlatpakDataDirectory(const DataDirectoryEnvironment& env);

// Requirement 12.2. Outside the Flatpak_Sandbox it changes nothing. Inside it, it creates
// FlatpakDataDirectory(env) and makes it the working directory, then makes a relative app0_dir,
// game_patch, and addcont_dir absolute against env.cwd, and a relative printf_output_file,
// shader_log_folder, and command_buffer_dump_folder absolute against the Data_Directory. Empty and
// absolute paths stay as they are. Returns an empty string, or "cannot create the data directory
// <path>: <strerror>" or "cannot enter the data directory <path>: <strerror>" with options
// unchanged.
std::string ApplyDataDirectory(RunOptions& options, const DataDirectoryEnvironment& env);

// Requirement 14.6: the asset relative (for example "sounds/trophy-unlock.wav") of the Emulator in
// exe_dir: exe_dir/assets/relative when exists() is true for it, else
// exe_dir/../share/kyty/assets/relative (the fhs layout) when exists() is true for that, else the
// first path, so that a "not found" message names it.
std::filesystem::path
FindAssetFile(const std::filesystem::path& exe_dir, const std::filesystem::path& relative,
              const std::function<bool(const std::filesystem::path&)>& exists);

// FindAssetFile with Common::File::IsFileExisting.
std::filesystem::path FindAssetFile(const std::filesystem::path& exe_dir,
                                    const std::filesystem::path& relative);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOROPTIONS_H_ */
