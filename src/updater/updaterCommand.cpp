#include "updaterCommand.h"

#include "UpdateInstaller.h"
#include "updater/core/updateResult.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <spawn.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
extern char** environ;

namespace Kyty::Updater {
namespace {
namespace fs = std::filesystem;

bool WaitForParent(unsigned long pid) {
	if (pid > static_cast<unsigned long>(std::numeric_limits<pid_t>::max())) {
		return false;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (kill(static_cast<pid_t>(pid), 0) != 0) {
			return errno == ESRCH;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return false;
}

bool Launch(const fs::path& launcher, const fs::path& directory) {
	if (chdir(directory.c_str()) != 0) {
		return false;
	}
	setsid();
	pid_t child;
	char* arguments[] = {const_cast<char*>(launcher.c_str()), nullptr};
	return posix_spawn(&child, launcher.c_str(), nullptr, nullptr, arguments, environ) == 0;
}

void WriteResult(const fs::path& target, const Result& result) {
	// WriteUpdateResult never opens the destination: its rename safely replaces even an existing
	// symlink.
	const auto written = kyty::updater::WriteUpdateResult(target, {result.success, result.message});
	if (!written) {
		// Reporting must never prevent the original or updated launcher restarting.
		std::cerr << "Cannot record update result: " << written.error() << '\n';
	}
}

void CleanupWorkspace(const fs::path& source, const fs::path& target) try {
	const auto workspace = source.parent_path();
	if (source.filename() != "payload" ||
	    !workspace.filename().string().starts_with(".kyty-update-") ||
	    fs::canonical(workspace.parent_path()) != fs::canonical(target)) {
		return;
	}
	// Remove every entry (the package, the payload, the log, and this helper's own copy, which a
	// running process may unlink), then the workspace itself.
	for (const auto& entry: fs::directory_iterator(workspace)) {
		std::error_code ignored;
		fs::remove_all(entry.path(), ignored);
	}
	std::error_code ignored;
	fs::remove(workspace, ignored);
} catch (const std::exception& error) {
	std::cerr << "Update installed; temporary files could not be removed: " << error.what() << '\n';
}
} // namespace

int RunUpdaterCommand(const std::vector<fs::path>& arguments) {
	const bool validate = arguments.size() == 5 && arguments[1] == "--validate";
	const bool install  = arguments.size() == 7 && arguments[1] == "--install";
	if (!validate && !install) {
		std::cerr
		    << "Usage: kyty_updater --validate SOURCE TARGET LAUNCHER_REL\n"
		       "       kyty_updater --install SOURCE TARGET LAUNCHER_REL PARENT_PID WORKDIR\n";
		return 2;
	}
	const auto source   = fs::absolute(arguments[2]);
	const auto target   = fs::absolute(arguments[3]);
	const auto launcher = arguments[4];
	if (validate) {
		const auto result = Validate(source, target, launcher);
		if (!result.success) {
			std::cerr << result.message << '\n';
		}
		return result.success ? 0 : 1;
	}
	const auto  pid_string = arguments[5].string();
	std::size_t consumed   = 0;
	const auto  pid        = std::stoul(pid_string, &consumed);
	if (pid <= 1 || consumed != pid_string.size() || pid_string.front() == '-') {
		throw std::runtime_error("Invalid launcher process identifier.");
	}
	if (!WaitForParent(pid)) {
		WriteResult(target,
		            {false, "Update cancelled: the launcher did not exit within two minutes."});
		return 1;
	}
	auto result = Install(source, target, launcher);
	WriteResult(target, result);
	if (!Launch(target / launcher, fs::absolute(arguments[6]))) {
		result.message += "\nThe launcher could not restart. Open it manually.";
		WriteResult(target, result);
		std::cerr << result.message << '\n';
		return 1;
	}
	if (result.success) {
		CleanupWorkspace(source, target);
	}
	return result.success ? 0 : 1;
}
} // namespace Kyty::Updater
