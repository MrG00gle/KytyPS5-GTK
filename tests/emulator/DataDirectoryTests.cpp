// Emulator::ApplyDataDirectory and FlatpakDataDirectory (task 25.1, Requirement 12.2): nothing
// changes outside the Flatpak_Sandbox; inside it the Data_Directory is $XDG_DATA_HOME/Kyty by the
// launcher's rule, it is created and entered, the game, patch, and add-on paths become absolute
// against the original working directory, and relative log, shader, and buffer paths resolve in the
// Data_Directory. Failures are injected through the environment's callbacks (no chmod), and one
// real failure uses a regular file where a directory is needed.

#include "common/archive.h"
#include "emulatorOptions.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

using Emulator::DataDirectoryEnvironment;
using Emulator::RunOptions;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "DataDirectoryTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void CheckPath(const fs::path& actual, const std::string& expected, const std::string& what) {
	Check(actual.native() == expected,
	      what + " is '" + expected + "', got '" + actual.native() + "'");
}

// An environment whose callbacks record their calls and fail with the given errno values.
struct Recorder {
	std::vector<std::string> calls;
	int                      create_error = 0;
	int                      enter_error  = 0;

	DataDirectoryEnvironment Environment(bool flatpak, std::optional<std::string> xdg_data_home,
	                                     std::optional<std::string> home, fs::path cwd) {
		DataDirectoryEnvironment env;
		env.flatpak            = flatpak;
		env.xdg_data_home      = std::move(xdg_data_home);
		env.home               = std::move(home);
		env.cwd                = std::move(cwd);
		env.create_directories = [this](const fs::path& dir) {
			calls.push_back("create " + dir.native());
			return create_error;
		};
		env.change_directory = [this](const fs::path& dir) {
			calls.push_back("enter " + dir.native());
			return enter_error;
		};
		return env;
	}
};

RunOptions RelativeOptions() {
	RunOptions options;
	options.app0_dir                          = "games/Title";
	options.elf                               = "/app0/eboot.bin";
	options.game_patch                        = "patches/PPSA00001.json";
	options.addcont_dir                       = "../dlc";
	options.config.guest_arguments            = {"relative-argument"};
	options.config.screen_width               = 1920;
	options.config.printf_output_file         = "_kyty.txt";
	options.config.shader_log_folder          = "_Shaders";
	options.config.command_buffer_dump_folder = "logs/_Buffers/";
	return options;
}

void CheckUnchanged(const RunOptions& actual, const RunOptions& expected, const std::string& what) {
	Check(actual.app0_dir == expected.app0_dir && actual.elf == expected.elf &&
	          actual.game_patch == expected.game_patch &&
	          actual.addcont_dir == expected.addcont_dir &&
	          actual.config.guest_arguments == expected.config.guest_arguments &&
	          actual.config.screen_width == expected.config.screen_width &&
	          actual.config.printf_output_file.native() ==
	              expected.config.printf_output_file.native() &&
	          actual.config.shader_log_folder.native() ==
	              expected.config.shader_log_folder.native() &&
	          actual.config.command_buffer_dump_folder.native() ==
	              expected.config.command_buffer_dump_folder.native(),
	      what + ": the options are unchanged");
}

// Outside the Flatpak_Sandbox nothing is created, entered, or rewritten.
void TestOutsideFlatpak() {
	Recorder   recorder;
	RunOptions options = RelativeOptions();
	const auto error   = Emulator::ApplyDataDirectory(
	    options, recorder.Environment(false, "/xdg", "/home/u", "/work"));
	Check(error.empty(), "outside Flatpak: no error, got '" + error + "'");
	Check(recorder.calls.empty(), "outside Flatpak: nothing is created or entered");
	CheckUnchanged(options, RelativeOptions(), "outside Flatpak");
}

// Inside it: the directory is created, then entered, and the paths are rewritten.
void TestInsideFlatpak() {
	Recorder   recorder;
	RunOptions options = RelativeOptions();
	const auto error   = Emulator::ApplyDataDirectory(
	    options, recorder.Environment(true, "/xdg/data/", "/home/u", "/work/dir/"));
	Check(error.empty(), "inside Flatpak: no error, got '" + error + "'");
	Check(recorder.calls ==
	          std::vector<std::string> {"create /xdg/data/Kyty", "enter /xdg/data/Kyty"},
	      "inside Flatpak: /xdg/data/Kyty is created, then entered");
	CheckPath(options.app0_dir, "/work/dir/games/Title", "the game directory");
	CheckPath(options.game_patch, "/work/dir/patches/PPSA00001.json", "the game patch");
	CheckPath(options.addcont_dir, "/work/dir/../dlc", "the add-on directory");
	CheckPath(options.config.printf_output_file, "/xdg/data/Kyty/_kyty.txt", "the printf file");
	CheckPath(options.config.shader_log_folder, "/xdg/data/Kyty/_Shaders", "the shader log folder");
	CheckPath(options.config.command_buffer_dump_folder, "/xdg/data/Kyty/logs/_Buffers/",
	          "the command buffer dump folder");
	CheckPath(options.elf, "/app0/eboot.bin", "the guest ELF path");
	Check(options.config.guest_arguments == std::vector<std::string> {"relative-argument"},
	      "the guest arguments are unchanged");

	// Absolute and empty paths stay as they are.
	RunOptions absolute;
	absolute.app0_dir                          = "/games/Title";
	absolute.elf                               = "/app0/eboot.bin";
	absolute.config.printf_output_file         = "/logs/kyty.txt";
	absolute.config.shader_log_folder          = "/logs/shaders";
	absolute.config.command_buffer_dump_folder = "/logs/buffers";
	RunOptions copy                            = absolute;
	Check(Emulator::ApplyDataDirectory(copy, recorder.Environment(true, "/xdg", "/home/u", "/work"))
	          .empty(),
	      "absolute paths: no error");
	CheckUnchanged(copy, absolute, "absolute and empty paths");
	Check(copy.game_patch.empty() && copy.addcont_dir.empty(),
	      "an empty game patch and add-on directory stay empty");

	// An archive game keeps its member marker and its host archive.
	RunOptions archive;
	archive.app0_dir = Common::MakeArchivePath("games/Title.zar");
	Check(Emulator::ApplyDataDirectory(archive, recorder.Environment(true, "/xdg", {}, "/work"))
	          .empty(),
	      "archive game: no error");
	Check(Common::IsArchivePath(archive.app0_dir) &&
	          Common::GetArchiveHostPath(archive.app0_dir).native() == "/work/games/Title.zar",
	      "the archive game resolves against the working directory, got '" +
	          archive.app0_dir.native() + "'");
}

// The XDG_DATA_HOME and HOME rules (the launcher's dataPaths rule).
void TestDataDirectoryRule() {
	struct Case {
		std::optional<std::string> xdg;
		std::optional<std::string> home;
		std::string                cwd;
		std::string                expected;
	};
	const std::vector<Case> cases = {
	    {"/xdg", "/home/u", "/work", "/xdg/Kyty"},
	    {"/xdg//data/./x/../", "/home/u", "/work", "/xdg/data/Kyty"},
	    {std::nullopt, "/home/u", "/work", "/home/u/.local/share/Kyty"},
	    {"", "/home/u/", "/work", "/home/u/.local/share/Kyty"},
	    {"relative/xdg", "/home/u", "/work", "/home/u/.local/share/Kyty"},
	    {std::nullopt, std::nullopt, "/work", "/.local/share/Kyty"},
	    {std::nullopt, "", "/work", "/.local/share/Kyty"},
	    {std::nullopt, "h/../home", "/work/dir", "/work/dir/home/.local/share/Kyty"},
	    {std::nullopt, "h", "", "/h/.local/share/Kyty"},
	    {"/", "/home/u", "/work", "/Kyty"},
	};
	for (const auto& c: cases) {
		Recorder   recorder;
		const auto env  = recorder.Environment(true, c.xdg, c.home, c.cwd);
		const auto what = "XDG_DATA_HOME " + (c.xdg ? "'" + *c.xdg + "'" : std::string("unset")) +
		                  ", HOME " + (c.home ? "'" + *c.home + "'" : std::string("unset")) +
		                  ", cwd '" + c.cwd + "'";
		CheckPath(Emulator::FlatpakDataDirectory(env), c.expected, what);
		RunOptions options = RelativeOptions();
		Check(Emulator::ApplyDataDirectory(options, env).empty(), what + ": no error");
		CheckPath(options.config.printf_output_file, c.expected + "/_kyty.txt",
		          what + ": the printf file");
	}
}

// Injected failures: the message names the path and strerror, and nothing else happens.
void TestInjectedFailures() {
	{
		Recorder recorder;
		recorder.create_error = EACCES;
		RunOptions options    = RelativeOptions();
		const auto error      = Emulator::ApplyDataDirectory(
		    options, recorder.Environment(true, "/xdg", "/home/u", "/work"));
		const std::string expected =
		    "cannot create the data directory /xdg/Kyty: " + std::string(std::strerror(EACCES));
		Check(error == expected, "create failure gives '" + expected + "', got '" + error + "'");
		Check(recorder.calls == std::vector<std::string> {"create /xdg/Kyty"},
		      "create failure: the directory is not entered");
		CheckUnchanged(options, RelativeOptions(), "create failure");
	}
	{
		Recorder recorder;
		recorder.enter_error = ENOENT;
		RunOptions options   = RelativeOptions();
		const auto error     = Emulator::ApplyDataDirectory(
		    options, recorder.Environment(true, {}, "/home/u", "/work"));
		const std::string expected = "cannot enter the data directory /home/u/.local/share/Kyty: " +
		                             std::string(std::strerror(ENOENT));
		Check(error == expected, "enter failure gives '" + expected + "', got '" + error + "'");
		Check(recorder.calls.size() == 2, "enter failure: created, then entered");
		CheckUnchanged(options, RelativeOptions(), "enter failure");
	}
}

class TempDirectory {
public:
	TempDirectory() {
		std::string pattern = (fs::temp_directory_path() / "kyty-data-directory-XXXXXX").string();
		Check(::mkdtemp(pattern.data()) != nullptr, "mkdtemp");
		m_path = pattern;
	}
	~TempDirectory() {
		std::error_code ec;
		fs::remove_all(m_path, ec);
	}
	TempDirectory(const TempDirectory&)            = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	[[nodiscard]] const fs::path& Path() const { return m_path; }

private:
	fs::path m_path;
};

// This process's environment, and the real callbacks on a temporary tree.
void TestCurrentEnvironment() {
	const auto env = Emulator::CurrentDataDirectoryEnvironment();
	Check(env.flatpak == fs::exists("/.flatpak-info"), "flatpak follows /.flatpak-info");
	const char* xdg  = std::getenv("XDG_DATA_HOME");
	const char* home = std::getenv("HOME");
	Check(env.xdg_data_home == (xdg != nullptr ? std::optional<std::string>(xdg) : std::nullopt),
	      "xdg_data_home follows XDG_DATA_HOME");
	Check(env.home == (home != nullptr ? std::optional<std::string>(home) : std::nullopt),
	      "home follows HOME");
	const auto original = fs::current_path();
	CheckPath(env.cwd, original.native(), "cwd");

	TempDirectory temporary;
	const auto    root = temporary.Path().lexically_normal();
	{
		auto real          = env;
		real.flatpak       = true;
		real.xdg_data_home = (root / "xdg").native();
		RunOptions options = RelativeOptions();
		const auto error   = Emulator::ApplyDataDirectory(options, real);
		const auto entered = fs::current_path();
		Check(::chdir(original.c_str()) == 0, "chdir back");
		Check(error.empty(), "real environment: no error, got '" + error + "'");
		Check(fs::is_directory(root / "xdg" / "Kyty"), "real environment: the directory exists");
		Check(fs::equivalent(entered, root / "xdg" / "Kyty"),
		      "real environment: the directory was entered, got '" + entered.native() + "'");
		CheckPath(options.app0_dir, (original / "games/Title").native(),
		          "real environment: the game directory");
	}
	{
		// A regular file where the directory would go.
		std::ofstream(root / "file") << "x";
		auto real                 = env;
		real.flatpak              = true;
		real.xdg_data_home        = (root / "file" / "xdg").native();
		RunOptions        options = RelativeOptions();
		const auto        error   = Emulator::ApplyDataDirectory(options, real);
		const std::string prefix =
		    "cannot create the data directory " + (root / "file" / "xdg" / "Kyty").native() + ": ";
		Check(error.starts_with(prefix) && error.size() > prefix.size(),
		      "real failure names the path and a reason, got '" + error + "'");
		Check(fs::current_path() == original, "real failure: the working directory is unchanged");
		CheckUnchanged(options, RelativeOptions(), "real failure");
	}
}

} // namespace

int main() {
	TestOutsideFlatpak();
	TestInsideFlatpak();
	TestDataDirectoryRule();
	TestInjectedFailures();
	TestCurrentEnvironment();
	std::printf("DataDirectoryTests: all cases passed\n");
	return 0;
}
