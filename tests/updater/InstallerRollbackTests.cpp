// Example tests of the installer's file operations and rollback (src/updater/installFileOps.h and
// UpdateInstaller.h, task 17.3; Requirement 13.9): a fixed release installed over a fixed
// installation (files and a symbolic link replaced, files and nested directories added, files
// kept) with an InstallFileOps that fails exactly one mutating call, for every call of the
// successful install: Install fails, every replaced file is restored byte for byte, every added
// file and directory is gone, the payload is back, and the lock is removed. Also a backup cleanup
// that removes a backup before failing, a rollback that fails too, a rollback whose cleanup fails,
// an existing lock, Validate's lock, the release checks that refuse before any change, and the
// std::filesystem implementation. Compiled with -fexceptions: the failing operations throw
// std::filesystem::filesystem_error, as the real ones do.

#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/UpdateInstaller.h"
#include "updater/installFileOps.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using Kyty::Updater::DefaultFileOps;
using Kyty::Updater::InstallFileOps;
using KytyTests::TempDir;
using UpdaterGen::TreeEntry;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "InstallerRollbackTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

// Passes every call to DefaultFileOps() and records it; the call numbered fail_at (from 1) and the
// calls in also_fail throw instead, without changing anything. before_call runs first for every
// call, with its log entry and path.
class FailingFileOps final: public InstallFileOps {
public:
	explicit FailingFileOps(int fail_at = 0): m_fail_at(fail_at) {}

	void Rename(const fs::path& from, const fs::path& to) override {
		Step("Rename " + from.filename().string() + " -> " + to.string(), from);
		DefaultFileOps().Rename(from, to);
	}
	bool CreateDirectory(const fs::path& path) override {
		Step("CreateDirectory " + path.string(), path);
		return DefaultFileOps().CreateDirectory(path);
	}
	void CreateDirectories(const fs::path& path) override {
		Step("CreateDirectories " + path.string(), path);
		DefaultFileOps().CreateDirectories(path);
	}
	void Remove(const fs::path& path) override {
		Step("Remove " + path.string(), path);
		DefaultFileOps().Remove(path);
	}
	void RemoveAll(const fs::path& path) override {
		Step("RemoveAll " + path.string(), path);
		DefaultFileOps().RemoveAll(path);
	}

	[[nodiscard]] int                             Calls() const { return m_calls; }
	[[nodiscard]] const std::vector<std::string>& Log() const { return m_log; }

	std::function<void(const std::string& what, const fs::path& path)> before_call;
	std::vector<int>                                                   also_fail;

private:
	void Step(std::string what, const fs::path& path) {
		m_calls++;
		m_log.push_back(std::move(what));
		if (before_call) {
			before_call(m_log.back(), path);
		}
		bool fail = m_calls == m_fail_at;
		for (const int call: also_fail) {
			fail = fail || call == m_calls;
		}
		if (fail) {
			throw fs::filesystem_error("injected failure", path,
			                           std::make_error_code(std::errc::io_error));
		}
	}

	int                      m_fail_at = 0;
	int                      m_calls   = 0;
	std::vector<std::string> m_log;
};

const std::vector<TreeEntry>& Installed() {
	static const std::vector<TreeEntry> tree = {
	    {"kyty_launcher", false, "#!/bin/sh\necho old launcher\n", true},
	    {"kyty_emulator", false, "old emulator", true},
	    {"kyty_updater", false, "old updater", true},
	    {"keep.txt", false, "user data that is not in the release"},
	    {"assets/a.txt", false, "old a"},
	    {"assets/sounds/old.wav", false, "old wav"},
	    {"assets/empty", true, ""},
	};
	return tree;
}

const std::vector<TreeEntry>& Release() {
	static const std::vector<TreeEntry> tree = {
	    {"kyty_launcher", false, "#!/bin/sh\necho new launcher\n", true},
	    {"kyty_emulator", false, "new emulator", true},
	    {"kyty_updater", false, "new updater", true},
	    {"link.txt", false, "a file where a symbolic link was"},
	    {"assets/a.txt", false, "new a"},
	    {"assets/b.txt", false, "new b"},
	    {"assets/sounds", true, ""},
	    {"assets/empty", true, ""},
	    {"new/nested/c.txt", false, "new c"},
	    {"new/empty", true, ""},
	};
	return tree;
}

// A fresh installation (with a symbolic link that the release replaces by a file) and a fresh
// payload in a workspace beside it.
struct Fixture {
	TempDir  temp {"kyty_rollback"};
	fs::path install = temp.Path() / "install";
	fs::path payload = temp.Path() / "install/.kyty-update-Ab12Cd/payload";
	fs::path lock    = install / ".kyty-update-lock";

	Fixture() {
		Check(UpdaterGen::WriteTree(install, Installed()), "write the installation");
		temp.MakeSymlink("keep.txt", "install/link.txt");
		Check(UpdaterGen::WriteTree(payload, Release()), "write the payload");
	}

	// The installation without the workspace.
	[[nodiscard]] std::map<std::string, std::string> Installation() const {
		auto tree = UpdaterGen::SnapshotTree(install);
		for (auto it = tree.begin(); it != tree.end();) {
			it = it->first.starts_with(".kyty-update-Ab12Cd") ? tree.erase(it) : std::next(it);
		}
		return tree;
	}
};

Kyty::Updater::Result Install(const Fixture& fixture, InstallFileOps& ops) {
	return Kyty::Updater::Install(fixture.payload, fixture.install, "kyty_launcher", ops);
}

// The successful install: the number and the log of its mutating calls.
std::vector<std::string> SuccessfulCalls() {
	Fixture        fixture;
	const auto     before = fixture.Installation();
	FailingFileOps ops;
	const auto     result = Install(fixture, ops);
	Check(result.success && result.message.empty(), "the install succeeds: " + result.message);
	Check(fixture.Installation() == UpdaterGen::OverlaySnapshot(before, Release()),
	      "the release is installed, the other files are kept");
	Check(!Exists(fixture.lock), "no lock after a successful install");
	for (const auto& [path, kind]: UpdaterGen::SnapshotTree(fixture.payload)) {
		Check(kind == "D", "every payload file was moved: " + path);
	}
	// The lock, one call per release directory (6), three per replaced file (5: backup directory,
	// backup, install), one per added file (2), and the removal of the backups and the lock.
	Check(ops.Calls() == 1 + 6 + 3 * 5 + 2 + 1, "the call count: " + std::to_string(ops.Calls()));
	Check(ops.Log().front() == "CreateDirectory " + fixture.lock.string(), "the lock comes first");
	Check(ops.Log().back() == "RemoveAll " + fixture.lock.string(), "one removal at the end");
	return ops.Log();
}

void TestEveryFailingCall() {
	const auto calls = SuccessfulCalls();
	for (int fail_at = 1; fail_at <= static_cast<int>(calls.size()); fail_at++) {
		const std::string name = "failing call " + std::to_string(fail_at) + " (" +
		                         calls[static_cast<size_t>(fail_at - 1)] + ")";
		Fixture           fixture;
		const auto        installation = fixture.Installation();
		const auto        payload      = UpdaterGen::SnapshotTree(fixture.payload);
		FailingFileOps    ops(fail_at);
		const auto        result = Install(fixture, ops);
		Check(!result.success, name + ": Install fails");
		Check(result.message.find("injected failure") != std::string::npos,
		      name + ": the message names the failure: " + result.message);
		Check(result.message.find("Rollback failed") == std::string::npos,
		      name + ": the rollback succeeds");
		Check(fixture.Installation() == installation,
		      name + ": every installed file is byte-identical, nothing added remains");
		Check(!Exists(fixture.lock), name + ": the lock is removed");
		Check(UpdaterGen::SnapshotTree(fixture.payload) == payload, name + ": the payload is back");
		Check(ops.Calls() > fail_at || fail_at == 1, name + ": the rollback used the same ops");
	}
}

// A backup cleanup that deletes a backup and then fails cannot be rolled back: the release stays
// installed and the message names the backup.
void TestPartialCleanup() {
	Fixture        fixture;
	const auto     before = fixture.Installation();
	FailingFileOps ops;
	ops.before_call = [&](const std::string& what, const fs::path& path) {
		if (what.starts_with("RemoveAll ") && path == fixture.lock) {
			std::error_code ec;
			fs::remove(fixture.lock / "backup/assets/a.txt", ec);
			throw fs::filesystem_error("cleanup failure", path,
			                           std::make_error_code(std::errc::io_error));
		}
	};
	const auto result = Install(fixture, ops);
	Check(result.success, "a cleanup failure after a backup is gone keeps the installed release");
	Check(result.message ==
	          "Update installed; backup cleanup failed: " + (fixture.lock / "backup").string(),
	      "the cleanup message: " + result.message);
	auto installation = fixture.Installation();
	Check(installation.erase(".kyty-update-lock") == 1 &&
	          installation.erase(".kyty-update-lock/backup") == 1,
	      "the lock and the backup directory stay for recovery");
	for (auto it = installation.begin(); it != installation.end();) {
		it = it->first.starts_with(".kyty-update-lock/") ? installation.erase(it) : std::next(it);
	}
	Check(installation == UpdaterGen::OverlaySnapshot(before, Release()),
	      "the release is installed");
}

// A rollback that fails too keeps the backup and the lock and says where the backup is.
void TestRollbackFailure() {
	const auto calls = SuccessfulCalls();
	// Fail the install of kyty_emulator (the second replaced file), then its own rollback rename.
	int install_call = 0;
	for (size_t i = 0; i < calls.size(); i++) {
		if (calls[i].starts_with("Rename kyty_emulator -> ") &&
		    calls[i].find(".kyty-update-lock") == std::string::npos) {
			install_call = static_cast<int>(i) + 1;
		}
	}
	Check(install_call > 0, "the install call of kyty_emulator");
	Fixture        fixture;
	FailingFileOps ops(install_call);
	// The first rollback call restores the backup of kyty_emulator.
	ops.also_fail     = {install_call + 1};
	const auto result = Install(fixture, ops);
	Check(!result.success, "the install fails");
	Check(result.message.find("\nRollback failed: ") != std::string::npos,
	      "the rollback failure is reported: " + result.message);
	Check(result.message.ends_with("\nBackup retained at: " + (fixture.lock / "backup").string()),
	      "the backup is retained: " + result.message);
	Check(Exists(fixture.lock / "backup/kyty_emulator"), "the backup of kyty_emulator stays");
	Check(Exists(fixture.lock), "the lock stays for recovery");
	Check(!Exists(fixture.install / "kyty_emulator"), "kyty_emulator could not be restored");
	Check(UpdaterGen::ReadFile(fixture.install / "assets/a.txt") == std::string("old a"),
	      "the file replaced before it is restored");
	Check(UpdaterGen::ReadFile(fixture.install / "kyty_launcher") ==
	          std::string("#!/bin/sh\necho old launcher\n"),
	      "the files after it were never touched");
}

void TestExistingLock() {
	Fixture fixture;
	fs::create_directory(fixture.lock);
	const auto     installation = fixture.Installation();
	FailingFileOps ops;
	auto           result = Install(fixture, ops);
	Check(!result.success && result.message.starts_with("Another update is running") &&
	          result.message.ends_with(fixture.lock.string()),
	      "an existing lock refuses the install: " + result.message);
	Check(ops.Calls() == 1 && fixture.Installation() == installation && Exists(fixture.lock),
	      "nothing changed, the lock of the other update stays");
	result = Kyty::Updater::Validate(fixture.payload, fixture.install, "kyty_launcher", ops);
	Check(!result.success && result.message.starts_with("Another update is running"),
	      "an existing lock fails Validate");
	Check(Exists(fixture.lock), "Validate keeps the other lock");
}

void TestValidate() {
	Fixture        fixture;
	const auto     installation = fixture.Installation();
	FailingFileOps ops;
	auto result = Kyty::Updater::Validate(fixture.payload, fixture.install, "kyty_launcher", ops);
	Check(result.success && result.message.empty(), "Validate succeeds: " + result.message);
	Check(ops.Log() == std::vector<std::string> {"CreateDirectory " + fixture.lock.string(),
	                                             "Remove " + fixture.lock.string()},
	      "Validate creates and removes the lock through ops");
	Check(fixture.Installation() == installation, "Validate changes nothing");

	FailingFileOps no_lock(1);
	result = Kyty::Updater::Validate(fixture.payload, fixture.install, "kyty_launcher", no_lock);
	Check(!result.success && result.message.find("injected failure") != std::string::npos,
	      "a lock that cannot be created fails Validate: " + result.message);

	// As before the file operations existed, a lock that cannot be removed is not an error of
	// Validate (the install then reports it).
	FailingFileOps no_unlock(2);
	result = Kyty::Updater::Validate(fixture.payload, fixture.install, "kyty_launcher", no_unlock);
	Check(result.success && Exists(fixture.lock), "the lock removal failure is ignored");
	result = Install(fixture, no_unlock);
	Check(!result.success && result.message.starts_with("Another update is running"),
	      "the next install reports the lock");

	// An invalid release changes nothing and makes no call.
	Fixture        invalid;
	FailingFileOps untouched;
	fs::remove(invalid.payload / "kyty_updater");
	result = Kyty::Updater::Validate(invalid.payload, invalid.install, "kyty_launcher", untouched);
	Check(!result.success && result.message == "Release is missing an executable: kyty_updater",
	      "a release without kyty_updater: " + result.message);
	result = Install(invalid, untouched);
	Check(!result.success && untouched.Calls() == 0, "Install makes no call for it either");
}

// The release checks of Validate and Install (unchanged by the file operations): each refusal
// happens before any change.
void TestReleaseChecks() {
	struct Case {
		std::string                             name;
		std::function<void(const Fixture&)>     prepare;
		std::function<fs::path(const Fixture&)> source;
		fs::path                                launcher;
		std::string                             message; // the start of the message
	};
	const auto payload = [](const Fixture& fixture) { return fixture.payload; };
	for (const auto& [name, prepare, source, launcher, message]: std::vector<Case> {
	         {"an empty launcher path", [](const Fixture&) {}, payload, "",
	          "Update source, installation directory, or launcher path is invalid."},
	         {"a launcher path with ..", [](const Fixture&) {}, payload, "a/../kyty_launcher",
	          "Update source, installation directory, or launcher path is invalid."},
	         {"a missing source", [](const Fixture&) {},
	          [](const Fixture& fixture) { return fixture.temp.Path() / "missing"; },
	          "kyty_launcher",
	          "Update source, installation directory, or launcher path is invalid."},
	         {"a source below a file", [](const Fixture&) {},
	          [](const Fixture& fixture) { return fixture.install / "keep.txt/payload"; },
	          "kyty_launcher", "filesystem error: Cannot inspect update path"},
	         {"a source that contains the installation", [](const Fixture&) {},
	          [](const Fixture& fixture) { return fixture.temp.Path(); }, "kyty_launcher",
	          "Update source must not contain the installation directory."},
	         {"an executable without execute permission",
	          [](const Fixture& fixture) {
		          fs::remove(fixture.payload / "kyty_emulator");
		          fixture.temp.WriteFile("install/.kyty-update-Ab12Cd/payload/kyty_emulator", "x");
	          },
	          payload, "kyty_launcher",
	          "Release executable lacks execute permissions: kyty_emulator"},
	         {"a reserved name",
	          [](const Fixture& fixture) {
		          fixture.temp.WriteFile("install/.kyty-update-Ab12Cd/payload/.kyty-update-x", "x");
	          },
	          payload, "kyty_launcher", "Release uses a reserved updater filename."},
	         {"an absolute symbolic link",
	          [](const Fixture& fixture) {
		          fixture.temp.MakeSymlink("/etc/hostname",
		                                   "install/.kyty-update-Ab12Cd/payload/assets/link");
	          },
	          payload, "kyty_launcher", "Update contains a link outside its installation: "},
	         {"a FIFO",
	          [](const Fixture& fixture) {
		          Check(::mkfifo((fixture.payload / "pipe").c_str(), 0600) == 0, "mkfifo");
	          },
	          payload, "kyty_launcher", "Release contains an unsupported file: pipe"},
	         {"a directory where the release has a file",
	          [](const Fixture& fixture) { fixture.temp.MakeDirectories("install/assets/b.txt"); },
	          payload, "kyty_launcher", "Update would replace a different file type: "},
	     }) {
		Fixture fixture;
		prepare(fixture);
		const auto     installation = fixture.Installation();
		FailingFileOps ops;
		auto result = Kyty::Updater::Validate(source(fixture), fixture.install, launcher, ops);
		Check(!result.success && result.message.starts_with(message),
		      name + ": Validate refuses: " + result.message);
		result = Kyty::Updater::Install(source(fixture), fixture.install, launcher, ops);
		Check(!result.success && result.message.starts_with(message),
		      name + ": Install refuses: " + result.message);
		Check(ops.Calls() == 0 && fixture.Installation() == installation,
		      name + ": nothing changed");
	}

	// A symbolic link inside the release is installed as a link.
	Fixture fixture;
	fixture.temp.MakeSymlink("a.txt", "install/.kyty-update-Ab12Cd/payload/assets/alias");
	FailingFileOps ops;
	const auto     result = Install(fixture, ops);
	Check(result.success && fs::read_symlink(fixture.install / "assets/alias") == "a.txt",
	      "a relative link within the release: " + result.message);
}

// When the rollback's own cleanup fails, the original failure is still reported and the files are
// restored; the directory and the backup that could not be removed stay.
void TestRollbackCleanupFailure() {
	const auto calls = SuccessfulCalls();
	Fixture    fixture;
	const auto before = fixture.Installation();
	const int  last   = static_cast<int>(calls.size());
	// The final cleanup fails; the rollback then removes new/nested, new/empty, new (calls after
	// the restoring renames), then the backup. Fail the first directory removal and the backup
	// removal.
	FailingFileOps ops(last);
	ops.before_call = [&](const std::string& what, const fs::path&) {
		if (what == "Remove " + (fixture.install / "new/nested").string() ||
		    what == "RemoveAll " + (fixture.lock / "backup").string()) {
			throw fs::filesystem_error("cleanup failure", fixture.install,
			                           std::make_error_code(std::errc::io_error));
		}
	};
	const auto result = Install(fixture, ops);
	Check(!result.success && result.message.find("injected failure") != std::string::npos &&
	          result.message.find("Rollback failed") == std::string::npos,
	      "the original failure: " + result.message);
	auto installation = fixture.Installation();
	Check(installation.erase("new") == 1 && installation.erase("new/nested") == 1,
	      "the directory that could not be removed stays");
	for (auto it = installation.begin(); it != installation.end();) {
		it = it->first.starts_with(".kyty-update-lock") ? installation.erase(it) : std::next(it);
	}
	Check(installation == before, "every file is restored");
	Check(Exists(fixture.lock / "backup"), "the backup that could not be removed stays");
}

// DefaultFileOps() does what std::filesystem does, failures included.
void TestDefaultFileOps() {
	Check(&DefaultFileOps() == &DefaultFileOps(), "one instance");
	const TempDir temp("kyty_file_ops");
	auto&         ops   = DefaultFileOps();
	const auto    dir   = temp.Path() / "dir";
	const auto    file  = temp.WriteFile("file", "bytes");
	const auto    moved = temp.Path() / "dir/moved";
	Check(ops.CreateDirectory(dir) && !ops.CreateDirectory(dir), "CreateDirectory reports it");
	ops.Rename(file, moved);
	Check(!Exists(file) && UpdaterGen::ReadFile(moved) == std::string("bytes"), "Rename");
	ops.CreateDirectories(temp.Path() / "a/b/c");
	Check(fs::is_directory(temp.Path() / "a/b/c"), "CreateDirectories");
	ops.Remove(temp.Path() / "a/b/c");
	Check(!Exists(temp.Path() / "a/b/c"), "Remove");
	ops.RemoveAll(temp.Path() / "a");
	Check(!Exists(temp.Path() / "a"), "RemoveAll");

	int thrown = 0;
	for (const auto& operation: std::vector<std::function<void()>> {
	         [&] { ops.Rename(temp.Path() / "missing", temp.Path() / "other"); },
	         [&] { ops.CreateDirectory(temp.Path() / "missing/child"); },
	         [&] { ops.CreateDirectories(moved / "child"); },
	         [&] { ops.Remove(dir); },
	     }) {
		try {
			operation();
		} catch (const fs::filesystem_error&) {
			thrown++;
		}
	}
	Check(thrown == 4, "every failure throws filesystem_error: " + std::to_string(thrown));
}

} // namespace

int main() {
	TestEveryFailingCall();
	TestPartialCleanup();
	TestRollbackFailure();
	TestExistingLock();
	TestValidate();
	TestReleaseChecks();
	TestRollbackCleanupFailure();
	TestDefaultFileOps();
	std::printf("InstallerRollbackTests: all tests passed\n");
	return 0;
}
