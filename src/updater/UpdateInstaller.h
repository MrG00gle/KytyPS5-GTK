#pragma once

#include "installFileOps.h"

#include <filesystem>
#include <string>

namespace Kyty::Updater {
struct Result {
	bool        success;
	std::string message;
};

// Only release files are replaced. Files absent from the release are preserved.
// Successful installation consumes staged files; stage on the target filesystem.
// Every change to the installation goes through ops. When Install fails, it restores every
// replaced file, moves every installed file back to source, removes every directory it created
// and the lock, using the same ops; a rollback that fails keeps the backup and the lock.
Result Validate(const std::filesystem::path& source, const std::filesystem::path& target,
                const std::filesystem::path& launcher_relative,
                InstallFileOps&              ops = DefaultFileOps());
Result Install(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::filesystem::path& launcher_relative,
               InstallFileOps&              ops = DefaultFileOps());
} // namespace Kyty::Updater
