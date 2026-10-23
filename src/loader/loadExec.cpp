#include "common/file.h"
#include "common/logging/log.h"
#include "emulator.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#if   defined(__linux__)
#include <linux/close_range.h>
#include <unistd.h>
#endif

namespace Emulator {


int LoadExec(const char* path, const char* const argv[]) {
	using namespace Libs::SystemService;
	if (path == nullptr || !std::string_view(path).starts_with("/app0/") ||
	    !Common::File::IsFileExisting(Libs::LibKernel::FileSystem::GetRealFilename(path))) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}
#if defined(__linux__)
	std::vector<std::string> arguments {"--guest-exec", path};
	size_t                   bytes = 0;
	for (size_t i = 0; argv != nullptr && argv[i] != nullptr; ++i) {
		const auto size = strnlen(argv[i], 4096 - bytes) + 1;
		if (size > 4096 - bytes) {
			return SYSTEM_SERVICE_ERROR_PARAMETER;
		}
		bytes += size;
		arguments.emplace_back(argv[i], size - 1);
	}
	// Shown on the console too: the output that follows comes from the new program.
	auto message = fmt::format("LoadExec: switching to {}", path);
	for (size_t i = 2; i < arguments.size(); ++i) {
		message += fmt::format(" \"{}\"", arguments[i]);
	}
	Log::WriteToConsoleAndLog(message + "\n");
	// The new program writes to the same outputs, so everything written so far goes out first.
	// Neither exec nor quick_exit writes out buffered streams.
	Log::Flush();
	std::fflush(nullptr);
	std::ifstream            command("/proc/self/cmdline", std::ios::binary);
	std::vector<std::string> host_arguments;
	for (std::string argument;
	     std::getline(command, argument, '\0') && argument != "--guest-exec";) {
		host_arguments.push_back(std::move(argument));
	}
	if (host_arguments.empty()) {
		return SYSTEM_SERVICE_ERROR_INTERNAL;
	}
	host_arguments.insert(host_arguments.end(), arguments.begin(), arguments.end());
	std::vector<char*> host_argv;
	for (auto& argument: host_arguments) {
		host_argv.push_back(argument.data());
	}
	host_argv.push_back(nullptr);
	// The new program continues this one's log file. It is given its environment explicitly:
	// setenv is not safe while other threads may read the environment.
	const std::string  relaunched       = std::string(Log::RelaunchedVariable) + "=";
	std::string        relaunched_entry = relaunched + "1";
	std::vector<char*> environment;
	for (char** variable = environ; variable != nullptr && *variable != nullptr; ++variable) {
		if (!std::string_view(*variable).starts_with(relaunched)) {
			environment.push_back(*variable);
		}
	}
	environment.push_back(relaunched_entry.data());
	environment.push_back(nullptr);
	if (close_range(3, ~0u, CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) == 0) {
		execvpe(host_argv[0], host_argv.data(), environment.data());
	}
	LOGF("LoadExec failed: %s\n", std::strerror(errno));
	return SYSTEM_SERVICE_ERROR_INTERNAL;
#else
	return SYSTEM_SERVICE_ERROR_UNAVAILABLE;
#endif
}

} // namespace Emulator
