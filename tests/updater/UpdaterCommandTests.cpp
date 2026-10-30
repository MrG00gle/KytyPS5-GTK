// Example tests of the kyty_updater command line (src/updater/updaterCommand.h, task 17.3;
// Requirements 13.9, 13.14). Each case runs RunUpdaterCommand in a fork()ed child, as main() would
// (an exception is printed and gives exit status 1), with standard error in a file, against a
// TempDir installation whose launcher and release executables are small "#!/bin/sh" scripts
// created with mode 0755; a launcher writes a marker file when it is started again. Covered:
// --validate (valid and invalid releases), --install with an exited launcher process (success,
// a failed installation, a launcher that cannot restart, a result file that cannot be written,
// a payload outside a workspace), a launcher still running for a moment, one that never exits
// (a process identifier above pid_t), invalid process identifiers, and usage errors (exit 2).
// Compiled with -fexceptions to catch what RunUpdaterCommand throws.

#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/updateResult.h"
#include "updater/updaterCommand.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using KytyTests::TempDir;
using UpdaterGen::TreeEntry;

constexpr std::string_view kUsage =
    "Usage: kyty_updater --validate SOURCE TARGET LAUNCHER_REL\n"
    "       kyty_updater --install SOURCE TARGET LAUNCHER_REL PARENT_PID WORKDIR\n";

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "UpdaterCommandTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

struct CommandResult {
	int         exit_status = -1;
	std::string standard_error;
};

// Runs RunUpdaterCommand({"kyty_updater", args...}) in a child process with standard error in
// stderr_file and returns its exit status. The child ends with exit(), so it writes its own
// coverage profile.
CommandResult RunCommand(const std::vector<std::string>& args, const fs::path& stderr_file) {
	std::fflush(nullptr);
	const pid_t child = fork();
	Check(child >= 0, "fork");
	if (child == 0) {
		const int fd = ::open(stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
			std::_Exit(120);
		}
		::close(fd);
		std::vector<fs::path> arguments {"kyty_updater"};
		arguments.insert(arguments.end(), args.begin(), args.end());
		int status = 1;
		try {
			status = Kyty::Updater::RunUpdaterCommand(arguments);
		} catch (const std::exception& error) {
			std::cerr << error.what() << '\n';
		}
		std::cerr.flush();
		std::exit(status);
	}
	int status = 0;
	Check(waitpid(child, &status, 0) == child && WIFEXITED(status), "the command exits");
	return {WEXITSTATUS(status), UpdaterGen::ReadFile(stderr_file).value_or("")};
}

// The identifier of a process that has exited and been reaped.
std::string ExitedProcessId() {
	std::fflush(nullptr);
	const pid_t child = fork();
	Check(child >= 0, "fork");
	if (child == 0) {
		std::_Exit(0);
	}
	int status = 0;
	Check(waitpid(child, &status, 0) == child, "reap the exited process");
	return std::to_string(child);
}

// A launcher script that records which launcher started, and in which directory, in marker.
std::string LauncherScript(std::string_view name, const fs::path& marker) {
	return "#!/bin/sh\nprintf '" + std::string(name) + " %s\\n' \"$(pwd -P)\" > '" +
	       marker.string() + ".tmp' && mv '" + marker.string() + ".tmp' '" + marker.string() +
	       "'\n";
}

// The marker text of the launcher name started in directory.
std::string Started(std::string_view name, const fs::path& directory) {
	return std::string(name) + " " + fs::canonical(directory).string() + "\n";
}

// Waits up to 10 seconds for the restarted launcher's marker.
std::optional<std::string> WaitForMarker(const fs::path& marker) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		if (auto text = UpdaterGen::ReadFile(marker)) {
			return text;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return std::nullopt;
}

// An installation, a release payload in a workspace inside it, a working directory, and the
// marker that the restarted launcher writes.
struct Fixture {
	TempDir  temp {"kyty_command"};
	fs::path install   = temp.Path() / "install";
	fs::path workspace = install / ".kyty-update-Qw34Er";
	fs::path payload   = workspace / "payload";
	fs::path workdir   = temp.MakeDirectories("work dir");
	fs::path marker    = temp.Path() / "marker";
	fs::path errors    = temp.Path() / "stderr.txt";
	fs::path result    = install / kyty::updater::kUpdateResultFileName;

	std::vector<TreeEntry> release;

	explicit Fixture(bool with_updater = true) {
		Check(UpdaterGen::WriteTree(install,
		                            {{"kyty_launcher", false, LauncherScript("old", marker), true},
		                             {"kyty_emulator", false, "#!/bin/sh\nexit 0\n", true},
		                             {"kyty_updater", false, "#!/bin/sh\nexit 0\n", true},
		                             {"settings.ini", false, "kept"},
		                             {"assets/a.txt", false, "old a"}}),
		      "write the installation");
		release = {{"kyty_launcher", false, LauncherScript("new", marker), true},
		           {"kyty_emulator", false, "#!/bin/sh\necho new emulator\n", true},
		           {"assets/a.txt", false, "new a"},
		           {"assets/b/c.txt", false, "new c"}};
		if (with_updater) {
			release.push_back({"kyty_updater", false, "#!/bin/sh\necho new updater\n", true});
		}
		Check(UpdaterGen::WriteTree(payload, release), "write the payload");
		Check(UpdaterGen::WriteExecutable(workspace / "kyty_updater", "#!/bin/sh\nexit 0\n"),
		      "the helper copy in the workspace");
	}

	// The installation without the workspace.
	[[nodiscard]] std::map<std::string, std::string> Installation() const {
		auto tree = UpdaterGen::SnapshotTree(install);
		for (auto it = tree.begin(); it != tree.end();) {
			if (it->first.starts_with(".kyty-update-Qw34Er")) {
				it = tree.erase(it);
			} else {
				++it;
			}
		}
		return tree;
	}

	CommandResult Install(const std::string& pid, const fs::path& source, const fs::path& cwd) {
		return RunCommand(
		    {"--install", source.string(), install.string(), "kyty_launcher", pid, cwd.string()},
		    errors);
	}
};

void TestUsage() {
	const TempDir temp("kyty_command");
	const auto    errors = temp.Path() / "stderr.txt";
	for (const auto& args: std::vector<std::vector<std::string>> {
	         {},
	         {"--validate", "a", "b"},
	         {"--validate", "a", "b", "c", "d"},
	         {"--install", "a", "b", "c", "2"},
	         {"--bogus", "a", "b", "c"},
	         {"--install", "a", "b", "c", "2", "e", "f"},
	     }) {
		const auto result = RunCommand(args, errors);
		Check(result.exit_status == 2 && result.standard_error == kUsage,
		      "usage error for " + std::to_string(args.size()) +
		          " arguments: " + result.standard_error);
	}
}

void TestValidate() {
	Fixture    fixture;
	const auto before = fixture.Installation();
	auto       result = RunCommand(
	    {"--validate", fixture.payload.string(), fixture.install.string(), "kyty_launcher"},
	    fixture.errors);
	Check(result.exit_status == 0 && result.standard_error.empty(),
	      "a valid release: " + result.standard_error);
	Check(fixture.Installation() == before, "--validate changes nothing");

	Fixture invalid(false);
	result = RunCommand(
	    {"--validate", invalid.payload.string(), invalid.install.string(), "kyty_launcher"},
	    invalid.errors);
	Check(result.exit_status == 1 &&
	          result.standard_error == "Release is missing an executable: kyty_updater\n",
	      "a release without kyty_updater: " + result.standard_error);
}

void TestInstall() {
	Fixture    fixture;
	const auto before = fixture.Installation();
	const auto result = fixture.Install(ExitedProcessId(), fixture.payload, fixture.workdir);
	Check(result.exit_status == 0 && result.standard_error.empty(),
	      "the install succeeds: " + result.standard_error);
	auto installation = fixture.Installation();
	Check(installation.erase(std::string(kyty::updater::kUpdateResultFileName)) == 1,
	      "a result file");
	Check(installation == UpdaterGen::OverlaySnapshot(before, fixture.release),
	      "the release is installed, settings.ini is kept");
	Check(UpdaterGen::ReadFile(fixture.result) == std::string("success\n"), "the result file");
	Check(WaitForMarker(fixture.marker) == Started("new", fixture.workdir),
	      "the new launcher starts in the working directory");
	Check(!Exists(fixture.workspace), "the workspace is removed");
	Check(!Exists(fixture.install / ".kyty-update-lock"), "no lock");
}

void TestFailedInstall() {
	Fixture    fixture(false);
	const auto before = fixture.Installation();
	const auto result = fixture.Install(ExitedProcessId(), fixture.payload, fixture.workdir);
	Check(result.exit_status == 1 && result.standard_error.empty(),
	      "a failed install exits 1: " + result.standard_error);
	auto installation = fixture.Installation();
	installation.erase(std::string(kyty::updater::kUpdateResultFileName));
	Check(installation == before, "the installation is unchanged");
	Check(UpdaterGen::ReadFile(fixture.result) ==
	          std::string("failure\nRelease is missing an executable: kyty_updater"),
	      "the result file states the reason");
	Check(WaitForMarker(fixture.marker) == Started("old", fixture.workdir),
	      "the old launcher starts again");
	Check(Exists(fixture.payload / "kyty_launcher"), "the workspace stays after a failure");
}

void TestLauncherCannotRestart() {
	Fixture           fixture;
	const auto        missing = fixture.temp.Path() / "missing";
	const auto        result  = fixture.Install(ExitedProcessId(), fixture.payload, missing);
	const std::string restart = "\nThe launcher could not restart. Open it manually.";
	Check(result.exit_status == 1 && result.standard_error == restart + "\n",
	      "the restart failure is printed: " + result.standard_error);
	Check(UpdaterGen::ReadFile(fixture.result) == "success\n" + restart,
	      "and recorded with the installation result");
	Check(UpdaterGen::ReadFile(fixture.install / "kyty_emulator") ==
	          std::string("#!/bin/sh\necho new emulator\n"),
	      "the release was installed");
	Check(Exists(fixture.workspace), "the workspace stays");
}

void TestResultCannotBeWritten() {
	Fixture fixture;
	// A non-empty directory at the result path cannot be replaced.
	fixture.temp.WriteFile("install/.kyty-update-result/keep", "user data");
	const auto result = fixture.Install(ExitedProcessId(), fixture.payload, fixture.workdir);
	Check(result.exit_status == 0, "the install itself succeeds");
	Check(result.standard_error.starts_with("Cannot record update result: Cannot write " +
	                                        fixture.result.string()),
	      "the result failure is printed: " + result.standard_error);
	Check(UpdaterGen::ReadFile(fixture.result / "keep") == std::string("user data"),
	      "the directory is untouched");
	Check(WaitForMarker(fixture.marker) == Started("new", fixture.workdir),
	      "the launcher still restarts");
}

void TestPayloadOutsideWorkspace() {
	Fixture    fixture;
	const auto outside = fixture.temp.Path() / "payload";
	Check(UpdaterGen::WriteTree(outside, fixture.release), "a payload outside the installation");
	const auto result = fixture.Install(ExitedProcessId(), outside, fixture.workdir);
	Check(result.exit_status == 0, "the install succeeds: " + result.standard_error);
	Check(WaitForMarker(fixture.marker) == Started("new", fixture.workdir),
	      "the new launcher starts");
	Check(Exists(outside) && Exists(fixture.workspace),
	      "only a payload inside an update workspace of the installation is removed");
}

// The launcher process exits a moment after the helper started: the helper polls until then.
void TestLauncherStillRunning() {
	Fixture fixture;
	std::fflush(nullptr);
	const pid_t launcher = fork();
	Check(launcher >= 0, "fork");
	if (launcher == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		std::_Exit(0);
	}
	std::fflush(nullptr);
	const pid_t helper = fork();
	Check(helper >= 0, "fork");
	if (helper == 0) {
		int status = 1;
		try {
			status = Kyty::Updater::RunUpdaterCommand({"kyty_updater", "--install", fixture.payload,
			                                           fixture.install, "kyty_launcher",
			                                           std::to_string(launcher), fixture.workdir});
		} catch (const std::exception&) {
		}
		std::exit(status);
	}
	int status = 0;
	// Reaping the launcher makes it disappear for the helper's kill(pid, 0).
	Check(waitpid(launcher, &status, 0) == launcher, "the launcher exits");
	Check(waitpid(helper, &status, 0) == helper && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "the helper waits for it, then installs");
	Check(WaitForMarker(fixture.marker) == Started("new", fixture.workdir),
	      "the new launcher starts");
}

// A process identifier above pid_t can never exit: the helper gives up at once.
void TestLauncherNeverExits() {
	Fixture    fixture;
	const auto before = fixture.Installation();
	const auto result = fixture.Install("99999999999", fixture.payload, fixture.workdir);
	Check(result.exit_status == 1 && result.standard_error.empty(), "the helper gives up");
	Check(
	    UpdaterGen::ReadFile(fixture.result) ==
	        std::string("failure\nUpdate cancelled: the launcher did not exit within two minutes."),
	    "the result file says why");
	auto installation = fixture.Installation();
	installation.erase(std::string(kyty::updater::kUpdateResultFileName));
	Check(installation == before, "nothing was installed");
	Check(!Exists(fixture.marker), "no launcher was started");
}

void TestInvalidProcessIds() {
	for (const std::string pid: {"1", "0", "12x", "-5"}) {
		Fixture    fixture;
		const auto before = fixture.Installation();
		const auto result = fixture.Install(pid, fixture.payload, fixture.workdir);
		Check(result.exit_status == 1 &&
		          result.standard_error == "Invalid launcher process identifier.\n",
		      "process identifier '" + pid + "': " + result.standard_error);
		Check(fixture.Installation() == before && !Exists(fixture.result),
		      "nothing happens for '" + pid + "'");
	}
	Fixture    fixture;
	const auto result = fixture.Install("abc", fixture.payload, fixture.workdir);
	Check(result.exit_status == 1 && !result.standard_error.empty(),
	      "a process identifier that is not a number fails");
}

} // namespace

int main() {
	TestUsage();
	TestValidate();
	TestInstall();
	TestFailedInstall();
	TestLauncherCannotRestart();
	TestResultCannotBeWritten();
	TestPayloadOutsideWorkspace();
	TestLauncherStillRunning();
	TestLauncherNeverExits();
	TestInvalidProcessIds();
	std::printf("UpdaterCommandTests: all tests passed\n");
	return 0;
}
