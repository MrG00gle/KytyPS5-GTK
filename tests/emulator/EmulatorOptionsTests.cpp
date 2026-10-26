#include "common/archive.h"
#include "common/emulatorConfig.h"
#include "emulatorOptions.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Unreadable paths cannot be made with chmod when the tests run as root (the CI container does),
// so the parser's access() calls are interposed instead. The static kyty_emulator_options library
// binds to this definition at link time.
namespace {

struct DeniedAccess {
	std::string path;
	int         mode; // access() fails with EACCES when the requested mode has any of these bits.
};

std::vector<DeniedAccess>& DeniedAccesses() {
	static std::vector<DeniedAccess> denied;
	return denied;
}

int g_denied_calls = 0;

} // namespace

extern "C" int access(const char* path, int mode) noexcept {
	for (const auto& denied: DeniedAccesses()) {
		if ((mode & denied.mode) != 0 && denied.path == path) {
			g_denied_calls++;
			errno = EACCES;
			return -1;
		}
	}
	using AccessFunction      = int (*)(const char*, int);
	static const auto forward = reinterpret_cast<AccessFunction>(dlsym(RTLD_NEXT, "access"));
	return forward(path, mode);
}

namespace {

using Emulator::OptionKind;
using Emulator::ParseOutcome;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "EmulatorOptionsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Join(const std::vector<std::string>& args) {
	std::string text;
	for (const auto& arg: args) {
		text += text.empty() ? "" : " ";
		text += arg;
	}
	return text;
}

ParseOutcome Parse(const std::vector<std::string>& args) {
	return Emulator::ParseOptions(args);
}

ParseOutcome CheckRun(const std::vector<std::string>& args) {
	auto outcome = Parse(args);
	Check(outcome.status == ParseOutcome::Run,
	      "'" + Join(args) + "' gives Run, got status " + std::to_string(outcome.status) + " '" +
	          outcome.error + "'");
	return outcome;
}

void CheckHelpOutcome(const std::vector<std::string>& args) {
	const auto outcome = Parse(args);
	Check(outcome.status == ParseOutcome::Help && outcome.error.empty(),
	      "'" + Join(args) + "' gives Help");
}

void CheckError(const std::vector<std::string>& args, const std::string& expected) {
	const auto outcome = Parse(args);
	Check(outcome.status == ParseOutcome::Error && outcome.error == expected,
	      "'" + Join(args) + "' gives the error '" + expected + "', got status " +
	          std::to_string(outcome.status) + " '" + outcome.error + "'");
}

// Runs the parser while access() denies mode on path, and checks that the denial was used.
void CheckDeniedError(const std::filesystem::path& path, int mode,
                      const std::vector<std::string>& args, const std::string& expected) {
	DeniedAccesses().push_back({path.string(), mode});
	const int calls = g_denied_calls;
	CheckError(args, expected);
	Check(g_denied_calls > calls, "the injected access() failure ran for '" + Join(args) + "'");
	DeniedAccesses().clear();
}

std::string InvalidValue(const std::string& name, const std::string& value,
                         const std::string& reason) {
	return "invalid value for " + name + ": " + value + " (" + reason + ")";
}

// True when text has an option line for name: two spaces, the name, then a space or the end.
bool ListsOption(const std::string& text, std::string_view name) {
	const std::string line = "\n  " + std::string(name);
	for (auto position = text.find(line); position != std::string::npos;
	     position      = text.find(line, position + 1)) {
		const auto next = position + line.size();
		if (next == text.size() || text[next] == ' ' || text[next] == '\n') {
			return true;
		}
	}
	return false;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
	return text.size() >= suffix.size() &&
	       text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void WriteFile(const std::filesystem::path& path, std::string_view data) {
	std::ofstream stream(path, std::ios::binary);
	stream.write(data.data(), static_cast<std::streamsize>(data.size()));
	Check(stream.good(), "write " + path.string());
}

void CreateDirectory(const std::filesystem::path& path) {
	std::error_code error;
	Check(std::filesystem::create_directories(path, error) && !error, "create " + path.string());
}

void Pack(const std::filesystem::path& source, const std::filesystem::path& archive) {
	Check(Common::PackArchive(source, archive, [](uint64_t, uint64_t) { return true; }),
	      "pack " + archive.string());
}

class TempDirectory {
public:
	TempDirectory() {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto name =
		    "kyty_emulator_options_" + std::to_string(getpid()) + "_" + std::to_string(unique);
		m_path = std::filesystem::temp_directory_path() / name;
		CreateDirectory(m_path);
	}

	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	TempDirectory(const TempDirectory&)            = delete;
	TempDirectory& operator=(const TempDirectory&) = delete;

	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};

struct Fixtures {
	std::filesystem::path root;
	std::filesystem::path game_dir;
	std::filesystem::path elf;
	std::filesystem::path patch;
	std::filesystem::path zar;
	std::filesystem::path zar_without_eboot;
};

Fixtures CreateFixtures(const std::filesystem::path& root) {
	Fixtures fixtures;
	fixtures.root              = root;
	fixtures.game_dir          = root / "game";
	fixtures.elf               = root / "game.elf";
	fixtures.patch             = root / "patch.json";
	fixtures.zar               = root / "game.zar";
	fixtures.zar_without_eboot = root / "noeboot.zar";
	CreateDirectory(fixtures.game_dir);
	WriteFile(fixtures.game_dir / "eboot.bin", "eboot");
	WriteFile(fixtures.elf, "elf");
	WriteFile(fixtures.patch, "{}");

	const auto zar_source = root / "zar-source";
	CreateDirectory(zar_source / "sce_sys");
	WriteFile(zar_source / "eboot.bin", "eboot");
	WriteFile(zar_source / "sce_sys/param.json", "{}");
	Pack(zar_source, fixtures.zar);

	const auto other_source = root / "noeboot-source";
	CreateDirectory(other_source);
	WriteFile(other_source / "other.bin", "other");
	Pack(other_source, fixtures.zar_without_eboot);
	return fixtures;
}

// A valid value for every Value option. TestValueOptions fails on any Options() entry that is
// missing here, so a new option needs a case.
std::vector<std::pair<std::string_view, std::string>> ValidValues(const Fixtures& fixtures) {
	return {
	    {"--game", fixtures.game_dir.string()},
	    {"--game-patch", fixtures.patch.string()},
	    {"--addcont-dir", fixtures.root.string()},
	    {"--screen-width", "1920"},
	    {"--screen-height", "1080"},
	    {"--user-name", "1234567890123456"},
	    {"--user-id", "1000"},
	    {"--mic", "default"},
	    {"--controller-color", "#112233"},
	    {"--controller-volume", "100"},
	    {"--controller-vibration", "0"},
	    {"--keyboard-player", "4"},
	    {"--present-mode", "Immediate"},
	    {"--gpu", "0"},
	    {"--vblank-frequency", "60"},
	    {"--console-language", "29"},
	    {"--shader-optimization-type", "Performance"},
	    {"--shader-log-direction", "File"},
	    {"--shader-log-folder", (fixtures.root / "shaders").string()},
	    {"--command-buffer-dump-folder", (fixtures.root / "buffers").string()},
	    {"--printf-direction", "Console"},
	    {"--printf-output-file", (fixtures.root / "printf.txt").string()},
	    {"--trophy-duration", "6"},
	    {"--keymap", "Cross=Return"},
	};
}

void TestOptionTable() {
	int flags  = 0;
	int values = 0;

	std::vector<std::string_view> help;
	std::vector<std::string_view> terminal;
	for (const auto& option: Emulator::Options()) {
		Check(option.name.starts_with('-') && !option.help.empty(),
		      "option " + std::string(option.name) + " has a name and help");
		switch (option.kind) {
			case OptionKind::Flag: flags++; break;
			case OptionKind::Value: values++; break;
			case OptionKind::Help: help.push_back(option.name); break;
			case OptionKind::Terminal: terminal.push_back(option.name); break;
		}
		Check((option.kind == OptionKind::Value || option.kind == OptionKind::Terminal) ==
		          !option.value_hint.empty(),
		      "only Value and Terminal options have a value hint: " + std::string(option.name));
	}
	Check(Emulator::Options().size() == 50, "Options() has 50 entries");
	Check(flags > 0 && values > 0, "Options() has Flag and Value options");
	Check(help == std::vector<std::string_view> {"--help", "-h"}, "Help options are --help and -h");
	Check(terminal == std::vector<std::string_view> {"--guest-exec"},
	      "the Terminal option is --guest-exec");
}

void TestFlagOptions(const Fixtures& fixtures) {
	const auto game = fixtures.game_dir.string();
	for (const auto& option: Emulator::Options()) {
		if (option.kind == OptionKind::Flag) {
			CheckRun({"--game", game, std::string(option.name)});
		}
	}

	const auto defaults = CheckRun({"--game", game});
	Check(defaults.options.config.splash_enabled && defaults.options.config.trophy_enabled &&
	          !defaults.options.config.fullscreen_enabled,
	      "flag defaults without flags");
	const auto set = CheckRun({"--game", game, "--fullscreen", "--no-splash",
	                           "--no-trophy-notifications", "--gpu-assisted-validation"});
	Check(set.options.config.fullscreen_enabled && !set.options.config.splash_enabled &&
	          !set.options.config.trophy_enabled &&
	          set.options.config.gpu_assisted_validation_enabled &&
	          set.options.config.vulkan_validation_enabled,
	      "flags store their values, and --gpu-assisted-validation implies --vulkan-validation");
}

void TestValueOptions(const Fixtures& fixtures) {
	const auto values = ValidValues(fixtures);
	const auto game   = fixtures.game_dir.string();
	for (const auto& [name, value]: values) {
		bool listed = false;
		for (const auto& option: Emulator::Options()) {
			listed = listed || (option.name == name && option.kind == OptionKind::Value);
		}
		Check(listed, "the value table entry " + std::string(name) + " is a Value option");
	}

	for (const auto& option: Emulator::Options()) {
		if (option.kind != OptionKind::Value) {
			continue;
		}
		const std::string name(option.name);
		const std::string* value = nullptr;
		for (const auto& entry: values) {
			if (entry.first == option.name) {
				value = &entry.second;
			}
		}
		Check(value != nullptr, "the value table covers " + name);
		if (name == "--game") {
			CheckRun({name, *value});
		} else {
			CheckRun({"--game", game, name, *value});
		}
		CheckError({name}, "missing value for " + name);
	}

	const auto applied = CheckRun({"--game", game, "--screen-width", "1920", "--user-name", "Ab",
	                               "--present-mode", "Immediate", "--keymap", "Cross=Return",
	                               "--keymap", "Circle=Escape", "--controller-color", "#112233"});
	const auto& config = applied.options.config;
	Check(config.screen_width == 1920 && config.user_name == "Ab" &&
	          config.present_mode == Config::PresentMode::Immediate &&
	          config.keymap == Config::Keymap {"Cross=Return", "Circle=Escape"} &&
	          config.controller_color == Config::ControllerColor {0x11, 0x22, 0x33},
	      "value options store their values, and --keymap may be repeated");
}

void TestHelp(const Fixtures& fixtures) {
	CheckHelpOutcome({"--help"});
	CheckHelpOutcome({"-h"});
	CheckHelpOutcome({});
	CheckHelpOutcome({"--help", "--game", fixtures.game_dir.string(), "--fullscreen"});
	CheckHelpOutcome({"--fullscreen", "-h"});
	CheckError({"--help", "--screen-width", "0"},
	           InvalidValue("--screen-width", "0", "expected a positive integer"));
	CheckError({"--help", "--redzone"}, "unknown option: --redzone");
	CheckError({"-h", "--screen-width"}, "missing value for --screen-width");
}

void TestGuestExec(const Fixtures& fixtures) {
	const auto game = fixtures.game_dir.string();
	const auto run  = CheckRun({"--game", game, "--guest-exec", "/app0/guest.elf", "a",
	                            "--fullscreen", "--redzone", "-h"});
	Check(run.options.elf == "/app0/guest.elf", "--guest-exec sets the ELF");
	Check(run.options.config.guest_arguments ==
	          std::vector<std::string> {"a", "--fullscreen", "--redzone", "-h"},
	      "--guest-exec passes every later argument to the guest");
	Check(!run.options.config.fullscreen_enabled, "arguments after --guest-exec are not parsed");

	const auto bare = CheckRun({"--game", game, "--guest-exec", "/app0/guest.elf"});
	Check(bare.options.config.guest_arguments.empty(), "--guest-exec without arguments");
	const auto plain = CheckRun({"--game", game});
	Check(plain.options.config.guest_arguments == std::vector<std::string> {""},
	      "an ordinary boot keeps the empty argv[0]");

	CheckError({"--game", game, "--guest-exec"}, "missing value for --guest-exec");
	CheckError({"--guest-exec", "/app0/guest.elf", "--game", game},
	           "missing required option: --game");
	Check(ListsOption(Emulator::UsageText(), "--guest-exec"), "the help lists --guest-exec");
}

void TestUsageText() {
	const auto text = Emulator::UsageText();
	Check(text.starts_with("kyty_emulator --game <dir|elf|zar> [options]\n"),
	      "the usage text starts with the synopsis");
	for (const auto& option: Emulator::Options()) {
		Check(ListsOption(text, option.name), "the usage text lists " + std::string(option.name));
	}
	Check(text.find("--redzone") == std::string::npos, "the usage text omits --redzone");
}

void TestErrorMessages(const Fixtures& fixtures) {
	const auto game = fixtures.game_dir.string();
	CheckError({"--redzone"}, "unknown option: --redzone");
	CheckError({"--redzone", "--game", game}, "unknown option: --redzone");
	CheckError({"--game", game, "--redzone", "1"}, "unknown option: --redzone");
	CheckError({"-x"}, "unknown option: -x");
	CheckError({"game.elf"}, "unexpected argument: game.elf (pass the game with --game)");
	CheckError({"--game", game, "extra"}, "unexpected argument: extra (pass the game with --game)");
	CheckError({"--screen-width"}, "missing value for --screen-width");
	CheckError({"--fullscreen"}, "missing required option: --game");
	CheckError({"--screen-width", "1920"}, "missing required option: --game");

	// One case per validation handler, with the exact reason text.
	const std::string user_id_reason =
	    "expected 0-" + std::to_string(Config::MAX_USER_ID) + ", except 254 and 255";
	const auto missing_patch = (fixtures.root / "missing.json").string();

	const std::vector<std::vector<std::string>> invalid = {
	    {"--screen-width", "0", "expected a positive integer"},
	    {"--screen-height", "abc", "expected a positive integer"},
	    {"--user-name", "12345678901234567", "must contain 1-16 bytes"},
	    {"--user-name", "", "must contain 1-16 bytes"},
	    {"--user-id", "254", user_id_reason},
	    {"--user-id", "-1", user_id_reason},
	    {"--controller-color", "112233", "expected #RRGGBB"},
	    {"--controller-color", "#11223g", "expected #RRGGBB"},
	    {"--controller-volume", "101", "expected 0-100"},
	    {"--controller-vibration", "101", "expected 0-100"},
	    {"--keyboard-player", "5", "expected 0-4"},
	    {"--present-mode", "Invalid", "expected Fifo, Mailbox, or Immediate"},
	    {"--gpu", "x", "expected an integer"},
	    {"--vblank-frequency", "-1", "expected a non-negative integer"},
	    {"--console-language", "30", "expected 0-29"},
	    {"--shader-optimization-type", "Fast", "expected None, Size, or Performance"},
	    {"--shader-log-direction", "Up", "expected Silent, Console, or File"},
	    {"--printf-direction", "Up", "expected Silent, Console, or File"},
	    {"--trophy-duration", "x", "expected a non-negative integer"},
	    {"--keymap", "Cross", "expected Control=Input"},
	    {"--keymap", "=Return", "expected Control=Input"},
	    {"--game-patch", missing_patch, "must point to an existing file"},
	    {"--game-patch", game, "must point to an existing file"},
	};
	for (const auto& entry: invalid) {
		CheckError({entry[0], entry[1]}, InvalidValue(entry[0], entry[1], entry[2]));
	}

	CheckError({"--game", game, "--game", game}, "--game can only be specified once");
	const auto patch = fixtures.patch.string();
	CheckError({"--game-patch", patch, "--game-patch", patch},
	           "--game-patch can only be specified once");
	const auto addcont = fixtures.root.string();
	CheckError({"--addcont-dir", addcont, "--addcont-dir", addcont},
	           "--addcont-dir can only be specified once");
}

void TestGamePaths(const Fixtures& fixtures) {
	const auto missing = (fixtures.root / "missing").string();
	CheckError({"--game", missing}, "--game path does not exist: " + missing);
	const auto under_file = (fixtures.elf / "eboot.bin").string();
	CheckError({"--game", under_file}, "--game path does not exist: " + under_file);

	const auto elf = fixtures.elf.string();
	CheckDeniedError(fixtures.elf, R_OK, {"--game", elf}, "--game path is not readable: " + elf);
	const auto zar = fixtures.zar.string();
	CheckDeniedError(fixtures.zar, R_OK, {"--game", zar}, "--game path is not readable: " + zar);
	const auto dir = fixtures.game_dir.string();
	CheckDeniedError(fixtures.game_dir, R_OK, {"--game", dir},
	                 "--game path is not readable: " + dir);
	// Readable but not searchable: a directory game also needs X_OK.
	CheckDeniedError(fixtures.game_dir, X_OK, {"--game", dir},
	                 "--game path is not readable: " + dir);
	// Only directories need search permission.
	DeniedAccesses().push_back({elf, X_OK});
	CheckRun({"--game", elf});
	DeniedAccesses().clear();

	const auto directory = CheckRun({"--game", dir});
	Check(directory.options.app0_dir == fixtures.game_dir &&
	          directory.options.elf == "/app0/eboot.bin",
	      "a directory game mounts the directory and runs eboot.bin");
	const auto file = CheckRun({"--game", elf});
	Check(file.options.app0_dir == fixtures.root && file.options.elf == "/app0/game.elf",
	      "an ELF game mounts its directory and runs the ELF");
}

void TestArchives(const Fixtures& fixtures) {
	const auto root = Common::MakeArchivePath(fixtures.zar);
	const auto zar  = CheckRun({"--game", fixtures.zar.string()});
	Check(zar.options.app0_dir == root && zar.options.elf == "/app0/eboot.bin",
	      "a .zar game mounts game.zar! and runs eboot.bin");

	const auto member_root = CheckRun({"--game", root.string()});
	Check(member_root.options.app0_dir == root && member_root.options.elf == "/app0/eboot.bin",
	      "game.zar! runs like a directory");

	const auto member = (root / "eboot.bin").string();
	const auto eboot  = CheckRun({"--game", member});
	Check(eboot.options.app0_dir == root && eboot.options.elf == "/app0/eboot.bin",
	      "game.zar!/eboot.bin mounts game.zar! and runs eboot.bin");

	const auto missing_zar = Common::MakeArchivePath(fixtures.root / "missing.zar");
	const auto missing     = (missing_zar / "eboot.bin").string();
	CheckError({"--game", missing}, "--game path does not exist: " + missing);
	CheckDeniedError(fixtures.zar, R_OK, {"--game", member},
	                 "--game path is not readable: " + member);

	const auto no_member = (root / "missing.bin").string();
	CheckError({"--game", no_member},
	           InvalidValue("--game", no_member, "expected a directory, ELF, or archive"));
	const auto no_eboot = fixtures.zar_without_eboot.string();
	CheckError({"--game", no_eboot},
	           InvalidValue("--game", no_eboot, "archive does not contain eboot.bin"));
}

struct ProcessResult {
	int         exit_code = -1; // -1 unless the process exited normally.
	std::string out;
};

// Runs program with args, stdin from /dev/null, and captures its stdout.
ProcessResult RunProcess(const std::string& program, const std::vector<std::string>& args) {
	std::vector<std::string> storage {program};
	storage.insert(storage.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (auto& arg: storage) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);

	int pipe_fds[2] = {-1, -1};
	Check(pipe2(pipe_fds, O_CLOEXEC) == 0, "create the stdout pipe");
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
	pid_t     pid   = 0;
	const int error = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_fds[1]);
	Check(error == 0, "spawn " + program + ": " + std::strerror(error));

	ProcessResult result;
	char          buffer[4096];
	for (;;) {
		const auto count = read(pipe_fds[0], buffer, sizeof(buffer));
		if (count > 0) {
			result.out.append(buffer, static_cast<size_t>(count));
		} else if (count < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	close(pipe_fds[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		Check(errno == EINTR, "wait for " + program);
	}
	if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
	}
	return result;
}

// CTest emulator_help: kyty_emulator --help exits 0 and prints UsageText(), which names every
// option.
void CheckEmulatorHelp(const std::string& emulator) {
	const auto result = RunProcess(emulator, {"--help"});
	Check(result.exit_code == 0, "--help exits 0, got " + std::to_string(result.exit_code));
	Check(EndsWith(result.out, Emulator::UsageText()), "--help prints UsageText() last");
	for (const auto& option: Emulator::Options()) {
		Check(ListsOption(result.out, option.name), "--help lists " + std::string(option.name));
	}
}

// CTest emulator_redzone_rejected: kyty_emulator --redzone exits 1, names the unknown option on
// the first line, and prints UsageText().
void CheckEmulatorRedzone(const std::string& emulator) {
	const auto result = RunProcess(emulator, {"--redzone"});
	Check(result.exit_code == 1, "--redzone exits 1, got " + std::to_string(result.exit_code));
	const auto first_line = result.out.substr(0, result.out.find('\n'));
	Check(first_line == "unknown option: --redzone",
	      "the first line is 'unknown option: --redzone', got '" + first_line + "'");
	Check(EndsWith(result.out, Emulator::UsageText()), "--redzone prints UsageText() last");
}

} // namespace

int main(int argc, char* argv[]) {
	if (argc == 3 && std::string_view(argv[1]) == "--check-help") {
		CheckEmulatorHelp(argv[2]);
		std::printf("EmulatorOptionsTests: kyty_emulator --help passed\n");
		return 0;
	}
	if (argc == 3 && std::string_view(argv[1]) == "--check-redzone") {
		CheckEmulatorRedzone(argv[2]);
		std::printf("EmulatorOptionsTests: kyty_emulator --redzone passed\n");
		return 0;
	}
	if (argc != 1) {
		std::fprintf(stderr, "usage: %s [--check-help <emulator> | --check-redzone <emulator>]\n",
		             argv[0]);
		return 2;
	}

	TempDirectory temporary;
	const auto    fixtures = CreateFixtures(temporary.Path());
	TestOptionTable();
	TestFlagOptions(fixtures);
	TestValueOptions(fixtures);
	TestHelp(fixtures);
	TestGuestExec(fixtures);
	TestUsageText();
	TestErrorMessages(fixtures);
	TestGamePaths(fixtures);
	TestArchives(fixtures);

	std::printf("EmulatorOptionsTests: all cases passed\n");
	return 0;
}
