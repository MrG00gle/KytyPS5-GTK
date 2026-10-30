// Feature: linux-gtk-modernization, Property 43: Installer rollback
//
// Validates Requirement 13.9. Each case draws a universe of at most 12 paths of up to three
// levels (each a file or a directory), an installed tree and a release payload as subsets of it
// (closed under parents, so the payload replaces, adds, and nests files and directories, and
// merges into installed directories), the release's three executables (each already installed or
// not), and random file contents. A first Install of a copy with an InstallFileOps that records
// every mutating call must succeed and install exactly the release over the tree. Then, on a fresh
// copy, the same install runs with that InstallFileOps failing one of those calls, drawn at
// random: Install must return failure, every installed file must be byte-identical to its
// pre-update content, no file or directory added by the update (the lock included) may remain,
// and the payload must be back as it was.

#include "support/property.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/UpdateInstaller.h"
#include "updater/installFileOps.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using UpdaterGen::TreeEntry;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

// Passes every call to DefaultFileOps() and records its kind; the call numbered fail_at (from 1)
// throws std::filesystem::filesystem_error instead, without changing anything.
class FailingFileOps final: public Kyty::Updater::InstallFileOps {
public:
	explicit FailingFileOps(int fail_at = 0): m_fail_at(fail_at) {}

	void Rename(const fs::path& from, const fs::path& to) override {
		Step("Rename", from);
		Kyty::Updater::DefaultFileOps().Rename(from, to);
	}
	bool CreateDirectory(const fs::path& path) override {
		Step("CreateDirectory", path);
		return Kyty::Updater::DefaultFileOps().CreateDirectory(path);
	}
	void CreateDirectories(const fs::path& path) override {
		Step("CreateDirectories", path);
		Kyty::Updater::DefaultFileOps().CreateDirectories(path);
	}
	void Remove(const fs::path& path) override {
		Step("Remove", path);
		Kyty::Updater::DefaultFileOps().Remove(path);
	}
	void RemoveAll(const fs::path& path) override {
		Step("RemoveAll", path);
		Kyty::Updater::DefaultFileOps().RemoveAll(path);
	}

	[[nodiscard]] const std::vector<std::string>& Kinds() const { return m_kinds; }

private:
	void Step(const char* kind, const fs::path& path) {
		m_kinds.emplace_back(kind);
		if (static_cast<int>(m_kinds.size()) == m_fail_at) {
			throw fs::filesystem_error("injected failure", path,
			                           std::make_error_code(std::errc::io_error));
		}
	}

	int                      m_fail_at = 0;
	std::vector<std::string> m_kinds;
};

struct Trees {
	std::vector<TreeEntry> installed;
	std::vector<TreeEntry> release;
};

Trees DrawTrees() {
	// The universe: paths over a small alphabet, so installed and release entries collide.
	std::map<std::string, bool> universe; // path -> directory
	const int                   count = Choose(0, 13);
	for (int i = 0; i < count; i++) {
		std::string path;
		const int   depth = Choose(1, 4);
		for (int level = 0; level < depth; level++) {
			path += (level == 0 ? "" : "/") + std::string(1, static_cast<char>('a' + Choose(0, 3)));
		}
		universe.emplace(path, OneIn(3));
	}
	// A path with an entry below it is a directory, and so are its parents.
	for (const auto& [path, directory]: std::map<std::string, bool>(universe)) {
		for (auto parent = fs::path(path).parent_path(); !parent.empty();
		     parent      = parent.parent_path()) {
			universe[parent.string()] = true;
		}
	}
	while (universe.size() > 12) {
		universe.erase(std::prev(universe.end()));
	}

	// Subsets closed under parents.
	const auto subset = [&universe](int keep_one_in) {
		std::set<std::string> chosen;
		for (const auto& [path, directory]: universe) {
			if (!OneIn(keep_one_in)) {
				continue;
			}
			chosen.insert(path);
			for (auto parent = fs::path(path).parent_path(); !parent.empty();
			     parent      = parent.parent_path()) {
				chosen.insert(parent.string());
			}
		}
		return chosen;
	};
	const auto entry = [&universe](const std::string& path) {
		const bool directory = universe[path];
		return TreeEntry {path, directory,
		                  directory
		                      ? std::string()
		                      : UpdaterGen::PatternBytes(Choose<size_t>(0, 40), Choose(0u, 1000u))};
	};
	Trees trees;
	for (const auto& path: subset(2)) {
		trees.installed.push_back(entry(path));
	}
	for (const auto& path: subset(2)) {
		trees.release.push_back(entry(path));
	}
	// The executables that Validate and Install require.
	for (const std::string name: {"kyty_launcher", "kyty_emulator", "kyty_updater"}) {
		trees.release.push_back({name, false, "#!/bin/sh\necho new " + name + "\n", true});
		if (OneIn(2)) {
			trees.installed.push_back({name, false, "#!/bin/sh\necho old " + name + "\n", true});
		}
	}
	return trees;
}

// An installation and a payload in a workspace beside it, written from trees.
struct Copy {
	explicit Copy(const Trees& trees) {
		RC_ASSERT(UpdaterGen::WriteTree(install, trees.installed));
		RC_ASSERT(UpdaterGen::WriteTree(payload, trees.release));
	}

	KytyTests::TempDir temp {"kyty_p43"};
	fs::path           install = temp.Path() / "install";
	fs::path           payload = temp.Path() / "install/.kyty-update-P43p43/payload";

	[[nodiscard]] std::map<std::string, std::string> Installation() const {
		auto tree = UpdaterGen::SnapshotTree(install);
		for (auto it = tree.begin(); it != tree.end();) {
			it = it->first.starts_with(".kyty-update-P43p43") ? tree.erase(it) : std::next(it);
		}
		return tree;
	}

	Kyty::Updater::Result Install(Kyty::Updater::InstallFileOps& ops) const {
		return Kyty::Updater::Install(payload, install, "kyty_launcher", ops);
	}
};

} // namespace

KYTY_PROPERTY(43, "Installer rollback") {
	return rc::check(name, []() {
		const auto trees = DrawTrees();

		// The successful install, which also counts the mutating calls.
		std::vector<std::string> kinds;
		{
			const Copy     copy(trees);
			const auto     before = copy.Installation();
			FailingFileOps ops;
			const auto     result = copy.Install(ops);
			RC_ASSERT(result.success);
			RC_ASSERT(result.message.empty());
			RC_ASSERT(copy.Installation() == UpdaterGen::OverlaySnapshot(before, trees.release));
			kinds = ops.Kinds();
		}
		RC_ASSERT(kinds.size() >= 5u);

		// The same install with one of those calls failing.
		const int      fail_at = Choose(1, static_cast<int>(kinds.size()) + 1);
		const Copy     copy(trees);
		const auto     installation = copy.Installation();
		const auto     payload      = UpdaterGen::SnapshotTree(copy.payload);
		FailingFileOps ops(fail_at);
		const auto     result = copy.Install(ops);
		RC_LOG() << trees.installed.size() << " installed and " << trees.release.size()
		         << " release entries, " << kinds.size() << " calls, failing call " << fail_at
		         << " (" << kinds[static_cast<size_t>(fail_at - 1)] << "): " << result.message
		         << "\n";
		RC_ASSERT(!result.success);
		RC_ASSERT(result.message.find("injected failure") != std::string::npos);
		RC_ASSERT(copy.Installation() == installation);
		RC_ASSERT(UpdaterGen::SnapshotTree(copy.payload) == payload);
		RC_TAG(fail_at == 1 ? "the lock"
		       : fail_at == static_cast<int>(kinds.size())
		           ? "the final cleanup"
		           : kinds[static_cast<size_t>(fail_at - 1)]);
	});
}
