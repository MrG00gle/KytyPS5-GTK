#pragma once

// Removal of stale update workspaces at launcher startup. UpdateSession::Install() creates
// <install_dir>/.kyty-update-XXXXXX (kWorkspacePrefix plus six mkdtemp characters, updateSession.h)
// and the kyty_updater helper removes it after the installation; a workspace stays behind only
// when the launcher or the helper was interrupted. The Qt launcher's UpdateChecker removed such
// workspaces at startup once nothing in them had changed for an hour
// (src/launcher/src/updateChecker.cpp); CleanupStaleWorkspaces keeps that rule, and the GTK
// launcher calls it at startup in the Official and Other update modes.

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace kyty::updater {

struct WorkspaceCleanup {
	// The workspaces removed, in name order.
	std::vector<std::filesystem::path> removed;
	// What could not be listed or removed, with the error: the installation directory when it
	// exists but cannot be listed, or a workspace that remove_all could not remove (completely).
	std::vector<std::pair<std::filesystem::path, std::error_code>> failed;
};

// Removes every entry of install_dir that
// - is named kWorkspacePrefix (".kyty-update-") plus exactly six more bytes, and is not
//   kUpdateResultFileName (".kyty-update-result", which the same pattern matches; the next start
//   reports and deletes that file, updateResult.h);
// - is a real directory: symlink_status, so a symbolic link with such a name is kept and never
//   followed;
// - has a newest modification time, over the directory itself and its direct entries (the entries'
//   own times, read with lstat, so a link inside counts with its own time), that lies more than
//   max_age before now.
// Each one is removed with std::filesystem::remove_all, which removes symbolic links inside the
// workspace without following them. A missing install_dir gives an empty result. Nothing else in
// install_dir is touched.
WorkspaceCleanup CleanupStaleWorkspaces(const std::filesystem::path&    install_dir,
                                        std::filesystem::file_time_type now,
                                        std::chrono::seconds max_age = std::chrono::hours(1));

} // namespace kyty::updater
