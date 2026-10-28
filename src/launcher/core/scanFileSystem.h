#pragma once

// The file system operations the game scanner needs (design.md, "Launcher_Core interfaces"), behind
// an interface so that tests can permute listings and inject unreadable paths
// (tests/support/fakeScanFileSystem.h) instead of changing permissions.

#include "launcher/core/expected.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kyty::core {

struct FsEntry {
	std::string name; // the file name only, in native encoding
	// The entry's own type: a symbolic link is Symlink whatever it points to.
	enum Type { File, Directory, Symlink, Other } type = Other;
};

class ScanFileSystem {
public:
	virtual ~ScanFileSystem() = default;

	// The entries of a directory (without "." and ".."), in any order. The directory itself is
	// resolved through symbolic links. An error (missing, not a directory, unreadable) is the
	// error text, for example "No such file or directory".
	virtual Expected<std::vector<FsEntry>, std::string> List(const std::filesystem::path& dir) = 0;

	// True when zar opens as a ZArchive whose root has a regular file named eboot.bin.
	virtual bool ArchiveHasRootEboot(const std::filesystem::path& zar) = 0;

	// The absolute path with every symbolic link, ".", and ".." resolved, or nullopt when the path
	// does not exist or cannot be resolved.
	virtual std::optional<std::filesystem::path> Canonical(const std::filesystem::path& path) = 0;
};

// The host file system: std::filesystem with error_code (entry types from symlink_status, so links
// are reported, not followed), Common::OpenArchive for ArchiveHasRootEboot, and
// std::filesystem::canonical for Canonical.
std::unique_ptr<ScanFileSystem> MakeHostScanFileSystem();

} // namespace kyty::core
