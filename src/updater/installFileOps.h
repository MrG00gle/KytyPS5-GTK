#pragma once

// The file operations through which the installer changes the installation tree (task 17.3;
// design.md "Updater interfaces"). Validate and Install (UpdateInstaller.h) make every rename,
// directory creation, and removal through an InstallFileOps, so tests can fail any one of them
// and check the rollback (Property 43); reads (status, canonical paths, directory iteration) stay
// direct std::filesystem calls.

#include <filesystem>

namespace Kyty::Updater {

// Each operation does what the std::filesystem function of the same name does and fails the same
// way, by throwing std::filesystem::filesystem_error. Every member is pure, so an implementation
// built without RTTI can derive from it.
struct InstallFileOps {
	virtual ~InstallFileOps() = default;

	virtual void Rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
	// True when it created the directory, false when a directory was already there.
	virtual bool CreateDirectory(const std::filesystem::path& path)   = 0;
	virtual void CreateDirectories(const std::filesystem::path& path) = 0;
	virtual void Remove(const std::filesystem::path& path)            = 0;
	virtual void RemoveAll(const std::filesystem::path& path)         = 0;
};

// The std::filesystem implementation (one static instance).
InstallFileOps& DefaultFileOps();

} // namespace Kyty::Updater
