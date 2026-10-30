#include "updater/core/workspaceCleanup.h"

#include "updater/core/updateResult.h"
#include "updater/core/updateSession.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <sys/stat.h>

namespace kyty::updater {

namespace {

namespace fs = std::filesystem;

// The six characters that mkdtemp puts in place of XXXXXX.
constexpr std::size_t kWorkspaceSuffixLength = 6;

bool IsWorkspaceName(const std::string& name) {
	return name.size() == kWorkspacePrefix.size() + kWorkspaceSuffixLength &&
	       name.starts_with(kWorkspacePrefix) && name != kUpdateResultFileName;
}

// The modification time of path itself (a symbolic link is not followed); nullopt when it cannot
// be read.
std::optional<fs::file_time_type> ModificationTime(const fs::path& path) {
	struct stat info {};
	if (::lstat(path.c_str(), &info) != 0) {
		return std::nullopt;
	}
	// st_mtim is a system time; file_clock::from_sys is also how file_clock::now() reads the clock.
	const std::chrono::sys_time<std::chrono::nanoseconds> modified(
	    std::chrono::seconds(info.st_mtim.tv_sec) + std::chrono::nanoseconds(info.st_mtim.tv_nsec));
	return std::chrono::time_point_cast<fs::file_time_type::duration>(
	    std::chrono::file_clock::from_sys(modified));
}

// True when nothing in the workspace, the directory itself or one of its direct entries, was
// modified at or after limit. A workspace whose time or entries cannot be read is kept.
bool IsStale(const fs::path& workspace, fs::file_time_type limit) {
	const auto own = ModificationTime(workspace);
	if (!own || *own >= limit) {
		return false;
	}
	std::error_code ec;
	for (fs::directory_iterator it(workspace, ec), end; !ec && it != end; it.increment(ec)) {
		const auto time = ModificationTime(it->path());
		if (time && *time >= limit) {
			return false;
		}
	}
	return !ec;
}

} // namespace

WorkspaceCleanup CleanupStaleWorkspaces(const fs::path& install_dir, fs::file_time_type now,
                                        std::chrono::seconds max_age) {
	WorkspaceCleanup      result;
	const auto            limit = now - max_age;
	std::vector<fs::path> stale;
	std::error_code       ec;
	for (fs::directory_iterator it(install_dir, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code status_ec;
		if (IsWorkspaceName(it->path().filename().string()) &&
		    it->symlink_status(status_ec).type() == fs::file_type::directory &&
		    IsStale(it->path(), limit)) {
			stale.push_back(it->path());
		}
	}
	if (ec && ec != std::errc::no_such_file_or_directory) {
		result.failed.emplace_back(install_dir, ec);
	}
	std::sort(stale.begin(), stale.end());
	for (auto& workspace: stale) {
		std::error_code remove_ec;
		fs::remove_all(workspace, remove_ec);
		if (remove_ec) {
			result.failed.emplace_back(std::move(workspace), remove_ec);
		} else {
			result.removed.push_back(std::move(workspace));
		}
	}
	return result;
}

} // namespace kyty::updater
