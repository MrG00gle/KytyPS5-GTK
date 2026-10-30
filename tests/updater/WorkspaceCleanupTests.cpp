// Example tests of kyty::updater::CleanupStaleWorkspaces (src/updater/core/workspaceCleanup.h): the
// Qt launcher's startup removal of update workspaces that nothing touched for an hour. Times are
// set with std::filesystem::last_write_time (and utimensat for a link's own time) relative to a
// fixed "now": an old workspace is removed; a recent one, one with a recent direct entry, names
// with a 5- or 7-character suffix, a symbolic link or a regular file named like a workspace, and
// .kyty-update-result as a file or an old directory are kept; a link inside an old workspace is
// removed without touching its target; a missing installation directory gives an empty result
// and one that is a regular file a failure. Everything else in the installation directory stays
// byte-identical.

#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/updateResult.h"
#include "updater/core/updateSession.h"
#include "updater/core/workspaceCleanup.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::CleanupStaleWorkspaces;
using KytyTests::TempDir;
using namespace std::chrono_literals;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "WorkspaceCleanupTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// Sets the modification time of path (following a symbolic link, as last_write_time does).
void SetTime(const fs::path& path, fs::file_time_type time) {
	std::error_code ec;
	fs::last_write_time(path, time, ec);
	Check(!ec, "set the time of " + path.string() + ": " + ec.message());
}

// Sets the modification time of the symbolic link itself.
void SetLinkTime(const fs::path& link, fs::file_time_type time) {
	const auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                             std::chrono::file_clock::to_sys(time).time_since_epoch())
	                             .count();
	timespec   times[2] {};
	times[0].tv_sec  = static_cast<std::time_t>(since_epoch / 1000000000);
	times[0].tv_nsec = static_cast<long>(since_epoch % 1000000000);
	times[1]         = times[0];
	Check(::utimensat(AT_FDCWD, link.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0,
	      "set the time of the link " + link.string());
}

// A workspace as UpdateSession::Install() leaves it (the helper copy, the package, the payload
// with one file), every entry and then the directory last modified at time.
fs::path MakeWorkspace(const fs::path& install, const std::string& name, fs::file_time_type time) {
	const auto dir = install / name;
	Check(UpdaterGen::WriteExecutable(dir / kyty::updater::kHelperFileName, "#!/bin/sh\n") &&
	          UpdaterGen::WriteExecutable(dir / kyty::updater::kPackageFileName, "package") &&
	          UpdaterGen::WriteExecutable(dir / kyty::updater::kPayloadFileName / "kyty_launcher",
	                                      "launcher"),
	      "write the workspace " + name);
	SetTime(dir / kyty::updater::kPayloadFileName / "kyty_launcher", time);
	for (const auto entry: {kyty::updater::kHelperFileName, kyty::updater::kPackageFileName,
	                        kyty::updater::kPayloadFileName}) {
		SetTime(dir / entry, time);
	}
	SetTime(dir, time);
	return dir;
}

// The snapshot without the entries at or below the given top-level names.
std::map<std::string, std::string> Without(std::map<std::string, std::string> tree,
                                           const std::vector<std::string>&    names) {
	std::erase_if(tree, [&](const auto& item) {
		for (const auto& name: names) {
			if (item.first == name || item.first.starts_with(name + "/")) {
				return true;
			}
		}
		return false;
	});
	return tree;
}

std::vector<fs::path> Paths(const fs::path& install, const std::vector<std::string>& names) {
	std::vector<fs::path> paths;
	for (const auto& name: names) {
		paths.push_back(install / name);
	}
	return paths;
}

const auto kNow = std::chrono::file_clock::now();
const auto kOld = kNow - 2h;

void TestOldAndRecent() {
	const TempDir temp("kyty_workspace_cleanup");
	const auto    install = temp.MakeDirectories("install");
	MakeWorkspace(install, ".kyty-update-AAAAAA", kOld);
	MakeWorkspace(install, ".kyty-update-zz9Z_x", kNow - 61min);
	MakeWorkspace(install, ".kyty-update-recent", kNow - 10min);
	MakeWorkspace(install, ".kyty-update-future", kNow + 10min);
	MakeWorkspace(install, ".kyty-update-at1hou", kNow - 1h);
	// One recent entry keeps an old workspace; only direct entries count.
	const auto entry = MakeWorkspace(install, ".kyty-update-entry1", kOld);
	SetTime(entry / kyty::updater::kPackageFileName, kNow - 5min);
	const auto deep = MakeWorkspace(install, ".kyty-update-deep01", kOld);
	SetTime(deep / kyty::updater::kPayloadFileName / "kyty_launcher", kNow);
	// The launcher itself and other files.
	Check(UpdaterGen::WriteExecutable(install / "kyty_launcher", "launcher") &&
	          UpdaterGen::WriteExecutable(install / "kyty_updater", "helper"),
	      "write the installation");
	SetTime(install / "kyty_launcher", kOld);
	const auto before = UpdaterGen::SnapshotTree(install);

	const auto result = CleanupStaleWorkspaces(install, kNow);
	Check(result.removed ==
	          Paths(install, {".kyty-update-AAAAAA", ".kyty-update-deep01", ".kyty-update-zz9Z_x"}),
	      "the workspaces untouched for more than an hour are removed, in name order");
	Check(result.failed.empty(), "no failure");
	Check(
	    UpdaterGen::SnapshotTree(install) ==
	        Without(before, {".kyty-update-AAAAAA", ".kyty-update-deep01", ".kyty-update-zz9Z_x"}),
	    "the recent workspaces (also one exactly an hour old, one from the future, and one with a "
	    "recent entry) and every other file stay byte-identical");

	// A shorter maximum age.
	const auto shorter = CleanupStaleWorkspaces(install, kNow, 30min);
	Check(shorter.removed == Paths(install, {".kyty-update-at1hou"}) && shorter.failed.empty(),
	      "with a 30-minute maximum age the hour-old workspace goes too; the one with a "
	      "5-minute-old entry stays");
}

void TestNamesAndKinds() {
	const TempDir temp("kyty_workspace_cleanup");
	const auto    install = temp.MakeDirectories("install");
	const auto    outside = temp.MakeDirectories("outside/old");
	temp.WriteFile("outside/old/keep.bin", "outside");
	SetTime(outside / "keep.bin", kOld);
	SetTime(outside, kOld);

	MakeWorkspace(install, ".kyty-update-ABCDE", kOld);            // 5 characters
	MakeWorkspace(install, ".kyty-update-ABCDEFG", kOld);          // 7 characters
	MakeWorkspace(install, ".kyty-update-", kOld);                 // none
	MakeWorkspace(install, "kyty-update-ABCDEF", kOld);            // no leading dot
	MakeWorkspace(install, ".Kyty-update-ABCDEF", kOld);           // other letter case
	MakeWorkspace(install, ".kyty-update-\xC3\xA9\xC3\xA9", kOld); // 4 bytes
	// A symbolic link named like a workspace, to an old directory outside: kept, never followed.
	temp.MakeSymlink(outside, "install/.kyty-update-LINK01");
	SetLinkTime(install / ".kyty-update-LINK01", kOld);
	temp.MakeSymlink("/nonexistent/kyty", "install/.kyty-update-DANGLE");
	// A regular file named like a workspace.
	temp.WriteFile("install/.kyty-update-FILE01", "a file");
	SetTime(install / ".kyty-update-FILE01", kOld);
	const auto before         = UpdaterGen::SnapshotTree(install);
	const auto outside_before = UpdaterGen::SnapshotTree(temp.Path() / "outside");

	const auto result = CleanupStaleWorkspaces(install, kNow);
	Check(result.removed.empty() && result.failed.empty(), "nothing matches");
	Check(UpdaterGen::SnapshotTree(install) == before,
	      "names with other suffixes, links, and files are kept");
	Check(UpdaterGen::SnapshotTree(temp.Path() / "outside") == outside_before,
	      "the link target is untouched");
}

void TestUpdateResult() {
	const TempDir temp("kyty_workspace_cleanup");
	const auto    install     = temp.MakeDirectories("install");
	const auto    result_path = install / kyty::updater::kUpdateResultFileName;
	temp.WriteFile(fs::path("install") / kyty::updater::kUpdateResultFileName, "success\n");
	SetTime(result_path, kOld);
	auto kept = CleanupStaleWorkspaces(install, kNow);
	Check(kept.removed.empty() && kept.failed.empty() &&
	          UpdaterGen::ReadFile(result_path) == std::optional<std::string>("success\n"),
	      ".kyty-update-result as an old file is kept");

	fs::remove(result_path);
	MakeWorkspace(install, std::string(kyty::updater::kUpdateResultFileName), kOld);
	kept = CleanupStaleWorkspaces(install, kNow);
	Check(kept.removed.empty() && kept.failed.empty() && fs::is_directory(result_path),
	      ".kyty-update-result as an old directory is kept too (the next start reports it)");
}

void TestLinkInsideWorkspace() {
	const TempDir temp("kyty_workspace_cleanup");
	const auto    install = temp.MakeDirectories("install");
	// A recent target: the link's own (old) time counts, not the target's.
	temp.WriteFile("target/file.bin", "target");
	temp.WriteFile("target/dir/inner.bin", "inner");
	const auto target_before = UpdaterGen::SnapshotTree(temp.Path() / "target");

	const auto workspace = MakeWorkspace(install, ".kyty-update-LINKIN", kOld);
	temp.MakeSymlink(temp.Path() / "target", "install/.kyty-update-LINKIN/to_dir");
	temp.MakeSymlink(temp.Path() / "target/file.bin", "install/.kyty-update-LINKIN/to_file");
	SetLinkTime(workspace / "to_dir", kOld);
	SetLinkTime(workspace / "to_file", kOld);
	SetTime(workspace, kOld);

	const auto result = CleanupStaleWorkspaces(install, kNow);
	Check(result.removed == std::vector<fs::path> {workspace} && result.failed.empty() &&
	          !fs::exists(fs::symlink_status(workspace)),
	      "an old workspace with links inside is removed");
	Check(UpdaterGen::SnapshotTree(temp.Path() / "target") == target_before,
	      "the link targets are untouched");

	// A link inside with a recent time of its own keeps the workspace.
	const auto recent = MakeWorkspace(install, ".kyty-update-LINKRC", kOld);
	temp.MakeSymlink(temp.Path() / "target", "install/.kyty-update-LINKRC/to_dir");
	SetTime(recent, kOld);
	const auto again = CleanupStaleWorkspaces(install, kNow);
	Check(again.removed.empty() && again.failed.empty() && fs::is_directory(recent),
	      "a recent link inside keeps the workspace");
}

void TestInstallDirectory() {
	const TempDir temp("kyty_workspace_cleanup");
	const auto    missing = CleanupStaleWorkspaces(temp.Path() / "missing", kNow);
	Check(missing.removed.empty() && missing.failed.empty(),
	      "a missing installation directory gives an empty result");

	const auto file = temp.WriteFile("file", "not a directory");
	const auto bad  = CleanupStaleWorkspaces(file, kNow);
	Check(bad.removed.empty() && bad.failed.size() == 1 && bad.failed[0].first == file &&
	          bad.failed[0].second == std::errc::not_a_directory,
	      "an installation directory that cannot be listed is a failure");
	Check(UpdaterGen::ReadFile(file) == std::optional<std::string>("not a directory"),
	      "and is untouched");

	const auto empty = temp.MakeDirectories("empty");
	Check(CleanupStaleWorkspaces(empty, kNow).removed.empty(), "an empty installation directory");
}

} // namespace

int main() {
	TestOldAndRecent();
	TestNamesAndKinds();
	TestUpdateResult();
	TestLinkInsideWorkspace();
	TestInstallDirectory();
	std::printf("WorkspaceCleanupTests: all tests passed\n");
	return 0;
}
