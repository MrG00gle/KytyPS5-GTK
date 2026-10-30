#include "UpdateInstaller.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Kyty::Updater {
namespace {
namespace fs = std::filesystem;
struct Entry {
	fs::path      relative;
	fs::file_type type;
};
struct Plan {
	fs::path           source;
	fs::path           target;
	std::vector<Entry> entries;
};

bool IsWithin(const fs::path& path, const fs::path& root) {
	auto part = path.begin();
	for (const auto& component: root) {
		if (part == path.end() || *part++ != component) {
			return false;
		}
	}
	return true;
}

bool IsRelative(const fs::path& path) {
	if (path.empty() || path.is_absolute() || path.has_root_name()) {
		return false;
	}
	for (const auto& component: path) {
		if (component == ".." || component == "." || component.empty()) {
			return false;
		}
	}
	return true;
}

fs::file_type Type(const fs::path& path) {
	std::error_code error;
	const auto      status = fs::symlink_status(path, error);
	if (error && error != std::errc::no_such_file_or_directory) {
		throw fs::filesystem_error("Cannot inspect update path", path, error);
	}
	return status.type();
}

void CheckLink(const fs::path& path, const fs::path& root) {
	const auto link = fs::read_symlink(path);
	if (link.is_absolute() || !IsWithin(fs::canonical(path), root)) {
		throw std::runtime_error("Update contains a link outside its installation: " +
		                         path.string());
	}
}

void CheckDestination(const fs::path& target, const Entry& entry) {
	const auto destination = target / entry.relative;
	const auto type        = Type(destination);
	if (type == fs::file_type::not_found) {
		return;
	}
	if ((entry.type == fs::file_type::directory) != (type == fs::file_type::directory) ||
	    (type != fs::file_type::regular && type != fs::file_type::directory &&
	     type != fs::file_type::symlink)) {
		throw std::runtime_error("Update would replace a different file type: " +
		                         destination.string());
	}
	if (type == fs::file_type::symlink) {
		CheckLink(destination, target);
	}
}

Plan MakePlan(const fs::path& source, const fs::path& target, const fs::path& launcher) {
	if (!IsRelative(launcher) || Type(source) != fs::file_type::directory ||
	    Type(target) != fs::file_type::directory) {
		throw std::runtime_error(
		    "Update source, installation directory, or launcher path is invalid.");
	}
	Plan plan {fs::canonical(source), fs::canonical(target), {}};
	if (IsWithin(plan.target, plan.source)) {
		throw std::runtime_error("Update source must not contain the installation directory.");
	}
	const auto binary_directory = launcher.parent_path();
	for (const auto& required:
	     {launcher, binary_directory / "kyty_emulator", binary_directory / "kyty_updater"}) {
		if (Type(plan.source / required) != fs::file_type::regular) {
			throw std::runtime_error("Release is missing an executable: " + required.string());
		}
		const auto executable =
		    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
		if ((fs::status(plan.source / required).permissions() & executable) == fs::perms::none) {
			throw std::runtime_error("Release executable lacks execute permissions: " +
			                         required.string());
		}
	}
	for (const auto& item: fs::recursive_directory_iterator(plan.source)) {
		const auto relative = item.path().lexically_relative(plan.source);
		const auto type     = item.symlink_status().type();
		if (relative.begin()->string().starts_with(".kyty-update")) {
			throw std::runtime_error("Release uses a reserved updater filename.");
		}
		if (type == fs::file_type::symlink) {
			CheckLink(item.path(), plan.source);
		} else if (type != fs::file_type::directory && type != fs::file_type::regular) {
			throw std::runtime_error("Release contains an unsupported file: " + relative.string());
		}
		plan.entries.push_back({relative, type});
	}
	// Checking directories before their children prevents destination symlink traversal.
	std::sort(plan.entries.begin(), plan.entries.end(),
	          [](const Entry& left, const Entry& right) { return left.relative < right.relative; });
	for (const auto& entry: plan.entries) {
		CheckDestination(plan.target, entry);
	}
	return plan;
}

struct InstallLock {
	explicit InstallLock(InstallFileOps& file_ops): ops(file_ops) {}
	InstallFileOps& ops;
	fs::path        path;
	void            Acquire(const fs::path& target) {
		auto candidate = target / ".kyty-update-lock";
		if (!ops.CreateDirectory(candidate)) {
			throw std::runtime_error(
			    "Another update is running, or a previous update was interrupted. "
			    "Close all updater processes, inspect retained backups, then remove " +
			    candidate.string());
		}
		path = std::move(candidate);
	}
	~InstallLock() {
		if (!path.empty()) {
			try {
				// An incomplete rollback leaves backups here and keeps the lock for recovery.
				ops.Remove(path);
			} catch (const std::exception&) {
			}
		}
	}
};

struct Change {
	fs::path relative;
	bool     old_moved = false;
	bool     installed = false;
};

// True while every replaced file still has its backup, so a rollback restores all of them.
bool BackupsIntact(const fs::path& backup, const std::vector<Change>& changes) {
	return std::all_of(changes.begin(), changes.end(), [&backup](const Change& change) {
		return !change.old_moved || Type(backup / change.relative) != fs::file_type::not_found;
	});
}
} // namespace

Result Validate(const fs::path& source, const fs::path& target, const fs::path& launcher_relative,
                InstallFileOps& ops) {
	try {
		MakePlan(source, target, launcher_relative);
		// Check actual write access, including ACLs, and interrupted/concurrent updates.
		InstallLock lock(ops);
		lock.Acquire(target);
		return {true, {}};
	} catch (const std::exception& error) {
		return {false, error.what()};
	}
}

Result Install(const fs::path& source, const fs::path& target, const fs::path& launcher_relative,
               InstallFileOps& ops) {
	InstallLock           lock(ops);
	fs::path              backup;
	std::vector<Change>   changes;
	std::vector<fs::path> created_directories;
	try {
		const auto plan = MakePlan(source, target, launcher_relative);
		lock.Acquire(plan.target);
		backup = lock.path / "backup";
		changes.reserve(plan.entries.size());
		created_directories.reserve(plan.entries.size());
		// The launcher extracts beside the installation, so files can move directly
		// from that staging directory without another full copy of the release.
		for (const auto& entry: plan.entries) {
			CheckDestination(plan.target, entry);
			const auto destination = plan.target / entry.relative;
			if (entry.type == fs::file_type::directory) {
				if (ops.CreateDirectory(destination)) {
					created_directories.push_back(destination);
				}
				continue;
			}
			changes.push_back({entry.relative});
			auto& change = changes.back();
			if (Type(destination) != fs::file_type::not_found) {
				ops.CreateDirectories((backup / entry.relative).parent_path());
				ops.Rename(destination, backup / entry.relative);
				change.old_moved = true;
			}
			ops.Rename(plan.source / entry.relative, destination);
			change.installed = true;
		}
		// One call removes the backups and the lock. When it fails while every backup is still
		// there, the update is rolled back like any other failure; once a backup is gone, the
		// installed release stays.
		try {
			ops.RemoveAll(lock.path);
		} catch (const std::exception&) {
			if (BackupsIntact(backup, changes)) {
				throw;
			}
			return {true, "Update installed; backup cleanup failed: " + backup.string()};
		}
		lock.path.clear();
		return {true, {}};
	} catch (const std::exception& error) {
		std::string message  = error.what();
		bool        restored = true;
		for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
			try {
				const auto destination = target / change->relative;
				if (change->installed) {
					ops.Rename(destination, source / change->relative);
				}
				if (change->old_moved) {
					ops.Rename(backup / change->relative, destination);
				}
			} catch (const std::exception& rollback_error) {
				restored = false;
				message += "\nRollback failed: " + std::string(rollback_error.what());
			}
		}
		for (auto directory = created_directories.rbegin(); directory != created_directories.rend();
		     ++directory) {
			try {
				ops.Remove(*directory);
			} catch (const std::exception&) {
			}
		}
		if (!backup.empty()) {
			if (restored) {
				try {
					ops.RemoveAll(backup);
				} catch (const std::exception&) {
				}
			} else {
				message += "\nBackup retained at: " + backup.string();
			}
		}
		return {false, message};
	}
}
} // namespace Kyty::Updater
