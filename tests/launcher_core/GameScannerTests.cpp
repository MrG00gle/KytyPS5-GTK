// Example tests of the game scanner (src/launcher/core/gameScanner.h, scanFileSystem.h;
// Requirements 7.1, 7.2, 7.10, 7.15). Real temporary trees and .zar files packed with
// Common::PackArchive; unreadable directories and resolution failures are injected through
// KytyTests::FaultInjectingScanFileSystem, never through permissions.

#include "common/archive.h"
#include "launcher/core/cancelToken.h"
#include "launcher/core/gameScanner.h"
#include "launcher/core/scanFileSystem.h"
#include "support/fakeScanFileSystem.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::CancelToken;
using kyty::core::FsEntry;
using kyty::core::GameEntry;
using kyty::core::GameMetadata;
using kyty::core::MakeHostMetadataReader;
using kyty::core::MakeHostScanFileSystem;
using kyty::core::MetadataReader;
using kyty::core::NormalizeGameFolders;
using kyty::core::ScanFileSystem;
using kyty::core::ScanGameFolders;
using kyty::core::ScanResult;
using KytyTests::FaultInjectingScanFileSystem;
using KytyTests::InMemoryMetadataReader;
using KytyTests::InMemoryScanFileSystem;
using KytyTests::TempDir;

constexpr auto kDirectory = GameEntry::Kind::Directory;
constexpr auto kArchive   = GameEntry::Kind::Archive;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameScannerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

// A temporary directory under its canonical path, so expected paths equal canonical ones.
class Tree {
public:
	Tree(): m_dir("kyty_scanner") {
		std::error_code ec;
		m_base = fs::canonical(m_dir.Path(), ec);
		Check(!ec, "canonical temporary directory");
	}

	[[nodiscard]] const fs::path& Base() const { return m_base; }

	fs::path File(const std::string& relative, const std::string& bytes = "x") const {
		m_dir.WriteFile(relative, bytes);
		return m_base / relative;
	}
	fs::path Dir(const std::string& relative) const {
		m_dir.MakeDirectories(relative);
		return m_base / relative;
	}
	fs::path Link(const fs::path& target, const std::string& relative) const {
		m_dir.MakeSymlink(target, relative);
		return m_base / relative;
	}
	fs::path Game(const std::string& relative) const {
		File(relative + "/eboot.bin", "ELF");
		return m_base / relative;
	}
	// A .zar packed from a staging directory outside the scanned folders.
	fs::path Archive(const std::string& relative, bool root_eboot,
	                 const std::string& param_json = "") const {
		static int counter = 0;
		const auto stage   = "stage/" + std::to_string(counter++);
		File(stage + (root_eboot ? "/eboot.bin" : "/sub/eboot.bin"), "ELF");
		if (!param_json.empty()) {
			File(stage + "/sce_sys/param.json", param_json);
		}
		const auto      archive = m_base / relative;
		std::error_code ec;
		fs::create_directories(archive.parent_path(), ec);
		Check(Common::PackArchive(m_base / stage, archive, [](uint64_t, uint64_t) { return true; }),
		      "pack " + archive.string());
		return archive;
	}

private:
	TempDir  m_dir;
	fs::path m_base;
};

ScanResult Scan(const std::vector<fs::path>& folders, ScanFileSystem& files,
                const MetadataReader& metadata) {
	const CancelToken cancel;
	return ScanGameFolders(folders, files, metadata, cancel);
}

ScanResult HostScan(const std::vector<fs::path>& folders) {
	const auto host   = MakeHostScanFileSystem();
	const auto reader = MakeHostMetadataReader();
	return Scan(folders, *host, *reader);
}

std::vector<std::string> Relative(const ScanResult& result, const fs::path& base) {
	std::vector<std::string> paths;
	for (const auto& entry: result.entries) {
		paths.push_back(entry.path.lexically_relative(base).generic_string());
	}
	return paths;
}

std::string Join(const std::vector<std::string>& values) {
	std::string text;
	for (const auto& value: values) {
		text += (text.empty() ? "" : ", ") + value;
	}
	return "[" + text + "]";
}

void CheckPaths(const ScanResult& result, const fs::path& base,
                const std::vector<std::string>& expected, const std::string& what) {
	const auto actual = Relative(result, base);
	Check(actual == expected, what + ": entries " + Join(actual) + ", expected " + Join(expected));
}

const GameEntry& EntryAt(const ScanResult& result, const fs::path& path) {
	for (const auto& entry: result.entries) {
		if (entry.path == path) {
			return entry;
		}
	}
	Check(false, "no entry " + path.string());
	std::abort();
}

class CountingReader final: public MetadataReader {
public:
	explicit CountingReader(std::function<void()> on_read = {}): m_on_read(std::move(on_read)) {}
	[[nodiscard]] GameMetadata Read(const fs::path& entry_path,
	                                GameEntry::Kind kind) const override {
		m_reads.push_back(entry_path);
		if (m_on_read) {
			m_on_read();
		}
		return m_inner->Read(entry_path, kind);
	}
	[[nodiscard]] const std::vector<fs::path>& Reads() const { return m_reads; }

private:
	std::unique_ptr<MetadataReader> m_inner = MakeHostMetadataReader();
	std::function<void()>           m_on_read;
	mutable std::vector<fs::path>   m_reads;
};

void TestNestedGames() {
	const Tree tree;
	tree.Game("f/a/b/c/Deep");
	tree.Game("f/Top");
	tree.File(
	    "f/Top/sce_sys/param.json",
	    R"({"localizedParameters":{"en-US":{"titleName":"Top Title"}},"titleId":"PPSA00001"})");
	tree.Game("f/Top/sub/Inner");               // below a game: never visited
	tree.Archive("f/Top/inside.zar", true);     // in a game directory: checked
	tree.Archive("f/Top/sub/hidden.zar", true); // below a game: never visited
	tree.Dir("f/empty");
	tree.File("f/readme.txt");

	const auto                   host = MakeHostScanFileSystem();
	FaultInjectingScanFileSystem faults(*host);
	const auto                   reader = MakeHostMetadataReader();
	const auto                   result = Scan({tree.Base() / "f"}, faults, *reader);
	CheckPaths(result, tree.Base(), {"f/Top", "f/Top/inside.zar", "f/a/b/c/Deep"}, "nested games");
	Check(result.warnings.empty() && !result.cancelled, "no warnings, not cancelled");

	const auto& top = EntryAt(result, tree.Base() / "f/Top");
	Check(top.kind == kDirectory && top.title_name == "Top Title" && top.title_id == "PPSA00001" &&
	          top.metadata_available && top.legacy_relative_path == "Top",
	      "the Top entry's metadata and legacy path");
	const auto& inside = EntryAt(result, tree.Base() / "f/Top/inside.zar");
	Check(inside.kind == kArchive && inside.title_name == "inside" && !inside.metadata_available &&
	          inside.legacy_relative_path == "Top/inside.zar",
	      "the archive in the game directory");
	const auto& deep = EntryAt(result, tree.Base() / "f/a/b/c/Deep");
	Check(deep.title_name == "Deep" && deep.legacy_relative_path == "a/b/c/Deep",
	      "a game at depth 4");
	for (const auto& listed: faults.ListCalls()) {
		Check(listed != tree.Base() / "f/Top/sub", "a game directory's subdirectory is not listed");
	}
	Check(faults.ListCalls().size() == 7, "f, Top, a, empty, b, c, Deep are listed once each: " +
	                                          std::to_string(faults.ListCalls().size()));
	Check(faults.ListCalls().front() == tree.Base() / "f", "the folder is listed first");
}

void TestFolderIsGame() {
	const Tree tree;
	tree.Game("g");
	tree.Game("g/sub/Inner");
	const auto result = HostScan({tree.Base() / "g"});
	CheckPaths(result, tree.Base(), {"g"}, "a folder with eboot.bin is itself the game");
	Check(result.entries[0].legacy_relative_path == ".", "its legacy path is '.'");
}

void TestSymlinks() {
	const Tree tree;
	tree.Dir("f");
	tree.Link(tree.Base() / "f", "f/loop");
	tree.Link("..", "f/up");
	tree.Link("loop", "f/self-chain");
	tree.Game("outside/Elsewhere");
	tree.Link(tree.Base() / "outside/Elsewhere", "f/linked-game");
	const auto elsewhere_zar = tree.Archive("outside/elsewhere.zar", true);
	tree.Link(elsewhere_zar, "f/linked.zar");
	tree.File("outside/real-eboot.bin", "ELF");
	tree.Link(tree.Base() / "outside/real-eboot.bin", "f/NotGame/eboot.bin");
	tree.Game("f/NotGame/Real");
	tree.Dir("f/DirEboot/eboot.bin");
	tree.Game("f/DirEboot/Real");
	tree.Link("missing-target", "f/dangling");

	const auto                   host = MakeHostScanFileSystem();
	FaultInjectingScanFileSystem faults(*host);
	const auto                   reader = MakeHostMetadataReader();
	const auto                   result = Scan({tree.Base() / "f"}, faults, *reader);
	CheckPaths(result, tree.Base(), {"f/DirEboot/Real", "f/NotGame/Real"},
	           "links are never followed and never count as eboot.bin or .zar");
	Check(result.warnings.empty(), "links give no warnings");
	Check(faults.ListCalls().size() == 6,
	      "f, NotGame, DirEboot, the directory DirEboot/eboot.bin, and the two Real games are "
	      "listed: " +
	          std::to_string(faults.ListCalls().size()));
	Check(faults.ArchiveCalls().empty(), "a linked .zar is never opened");

	// A Game_Folder that is itself a link is resolved: entries get canonical paths.
	tree.Link(tree.Base() / "f", "folder-link");
	const auto through = HostScan({tree.Base() / "folder-link"});
	CheckPaths(through, tree.Base(), {"f/DirEboot/Real", "f/NotGame/Real"},
	           "a linked folder is scanned");
	Check(through.entries[0].legacy_relative_path == "DirEboot/Real",
	      "the legacy path is relative to the folder as given");
}

void TestArchives() {
	const Tree tree;
	tree.Archive("f/good.zar", true,
	             R"({"titleId":"PPSA00777","localizedParameters":{"en-US":{"titleName":"Good"}}})");
	tree.File("f/bad.zar", std::string(8192, 'b'));
	tree.File("f/empty.zar", "");
	tree.Archive("f/noroot.zar", false);
	tree.Archive("f/UPPER.ZAR", true);
	tree.Archive("f/archive.bin", true);
	tree.Game("f/dir.zar");
	// An archive whose root has a directory named eboot.bin.
	tree.File("stage/direboot/eboot.bin/x", "x");
	Check(Common::PackArchive(tree.Base() / "stage/direboot", tree.Base() / "f/direboot.zar",
	                          [](uint64_t, uint64_t) { return true; }),
	      "pack direboot.zar");
	tree.Archive("f/deeper/nested.zar", true);

	const auto result = HostScan({tree.Base() / "f"});
	CheckPaths(result, tree.Base(), {"f/deeper/nested.zar", "f/dir.zar", "f/good.zar"},
	           "only .zar files with a root eboot.bin");
	const auto& good = EntryAt(result, tree.Base() / "f/good.zar");
	Check(good.kind == kArchive && good.title_name == "Good" && good.title_id == "PPSA00777" &&
	          good.metadata_available,
	      "the archive's metadata");
	Check(EntryAt(result, tree.Base() / "f/dir.zar").kind == kDirectory,
	      "a directory named .zar is a directory game");

	const auto host = MakeHostScanFileSystem();
	Check(host->ArchiveHasRootEboot(tree.Base() / "f/good.zar"), "good.zar has a root eboot.bin");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/bad.zar"), "bad.zar is not a ZArchive");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/empty.zar"), "an empty file");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/noroot.zar"), "eboot.bin only in sub/");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/direboot.zar"), "eboot.bin is a directory");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/missing.zar"), "a missing archive");
	Check(!host->ArchiveHasRootEboot(tree.Base() / "f/archive.bin"),
	      "the archive API only opens .zar names");
	Check(host->ArchiveHasRootEboot(tree.Base() / "f/UPPER.ZAR"),
	      "UPPER.ZAR opens, but the scanner's suffix check is case-sensitive");
}

void TestMissingFolders() {
	const Tree tree;
	tree.Game("f/Game");
	tree.File("plain.txt");
	const auto missing1 = tree.Base() / "missing1";
	const auto missing2 = tree.Base() / "missing2";
	const auto result = HostScan({missing1, tree.Base() / "f", missing1 / "",
	                              tree.Base() / "./missing1", missing2, tree.Base() / "plain.txt"});
	CheckPaths(result, tree.Base(), {"f/Game"}, "the other folders are still scanned");
	Check(result.warnings.size() == 3,
	      "one warning per missing path: " + std::to_string(result.warnings.size()));
	Check(result.warnings[0].path == missing1 && result.warnings[0].reason == ErrorText(ENOENT),
	      "missing1 is named once, with the error text: " + result.warnings[0].reason);
	Check(result.warnings[1].path == missing2 && result.warnings[1].reason == ErrorText(ENOENT),
	      "missing2 is named");
	Check(result.warnings[2].path == tree.Base() / "plain.txt" &&
	          result.warnings[2].reason == ErrorText(ENOTDIR),
	      "a folder that is a file: " + result.warnings[2].reason);
	Check(HostScan({}).entries.empty() && HostScan({}).warnings.empty(), "no folders, no result");
}

void TestOverlappingFolders() {
	const Tree tree;
	tree.Game("f/a/Game1");
	tree.Game("f/a/b/Game2");
	tree.Archive("f/a/one.zar", true);
	tree.Game("f/Outer");
	tree.Game("f/Outer/sub/Below");
	const auto f = tree.Base() / "f";
	const auto a = tree.Base() / "f/a";

	const auto forward = HostScan({f, a, f});
	CheckPaths(forward, tree.Base(), {"f/Outer", "f/a/Game1", "f/a/b/Game2", "f/a/one.zar"},
	           "overlapping folders list each entry once");
	Check(EntryAt(forward, a / "Game1").legacy_relative_path == "a/Game1" &&
	          EntryAt(forward, a / "one.zar").legacy_relative_path == "a/one.zar",
	      "legacy paths are relative to the first folder that contains the entry");

	const auto backward = HostScan({a, f});
	CheckPaths(backward, tree.Base(), {"f/Outer", "f/a/Game1", "f/a/b/Game2", "f/a/one.zar"},
	           "the same entries in the other folder order");
	Check(EntryAt(backward, a / "Game1").legacy_relative_path == "Game1" &&
	          EntryAt(backward, a / "b/Game2").legacy_relative_path == "b/Game2",
	      "legacy paths follow the folder order");

	const auto link   = tree.Link(f, "alias");
	const auto merged = HostScan({link, f});
	CheckPaths(merged, tree.Base(), {"f/Outer", "f/a/Game1", "f/a/b/Game2", "f/a/one.zar"},
	           "an aliased folder lists each entry once, under its canonical path");
	Check(EntryAt(merged, a / "Game1").legacy_relative_path == "a/Game1", "legacy path via alias");

	const auto below = HostScan({f, f / "Outer/sub"});
	CheckPaths(below, tree.Base(),
	           {"f/Outer", "f/Outer/sub/Below", "f/a/Game1", "f/a/b/Game2", "f/a/one.zar"},
	           "a folder below another folder's game is scanned itself");

	// Removing a folder removes exactly the entries no remaining folder contains.
	const auto only_a = HostScan({a});
	CheckPaths(only_a, tree.Base(), {"f/a/Game1", "f/a/b/Game2", "f/a/one.zar"},
	           "removing f keeps a's entries");
}

void TestUnreadable() {
	const Tree tree;
	tree.Game("f/a/Hidden");
	tree.Game("f/x/y/Hidden2");
	tree.Game("f/x/Visible");
	tree.Game("f/Other");
	tree.Dir("g");
	const auto f = tree.Base() / "f";
	const auto x = tree.Base() / "f/x";
	const auto g = tree.Base() / "g";

	const auto                   host = MakeHostScanFileSystem();
	FaultInjectingScanFileSystem faults(*host);
	faults.FailList(f / "a");
	faults.FailList(x / "y", "Input/output error");
	faults.FailList(g);
	const auto reader = MakeHostMetadataReader();
	const auto result = Scan({f, x, g}, faults, *reader);
	CheckPaths(result, tree.Base(), {"f/Other", "f/x/Visible"},
	           "the readable directories are still scanned");
	Check(result.warnings.size() == 3, "one warning per unreadable path, also when two folders "
	                                   "reach it: " +
	                                       std::to_string(result.warnings.size()));
	Check(result.warnings[0].path == f / "a" && result.warnings[0].reason == "Permission denied",
	      "f/a is named with its error");
	Check(result.warnings[1].path == x / "y" && result.warnings[1].reason == "Input/output error",
	      "f/x/y is named with its error");
	Check(result.warnings[2].path == g, "an unreadable folder is named");

	// The same directory through an aliased folder still gives one warning, under the first
	// spelling.
	const auto                   alias = tree.Link(f, "alias");
	FaultInjectingScanFileSystem aliased(*host);
	aliased.FailList(f / "a");
	aliased.FailList(alias / "a");
	const auto twice = Scan({f, alias}, aliased, *reader);
	Check(twice.warnings.size() == 1 && twice.warnings[0].path == f / "a",
	      "an unreadable directory reached through two spellings is named once");
}

void TestResolutionFailures() {
	const Tree tree;
	tree.Game("f/Game");
	tree.Game("f/Kept");
	tree.Archive("f/good.zar", true);
	tree.Archive("f/sub/again.zar", true);
	const auto f = tree.Base() / "f";

	const auto                   host = MakeHostScanFileSystem();
	FaultInjectingScanFileSystem faults(*host);
	faults.FailCanonical(f / "Game");
	faults.FailArchiveHasRootEboot(f / "good.zar");
	const auto reader = MakeHostMetadataReader();
	const auto result = Scan({f, f / "sub", f}, faults, *reader);
	CheckPaths(result, tree.Base(), {"f/Kept", "f/sub/again.zar"},
	           "an unresolvable game and a rejected archive are skipped");
	Check(result.warnings.size() == 1 && result.warnings[0].path == f / "Game" &&
	          result.warnings[0].reason == "cannot resolve the canonical path",
	      "an unresolvable game gives one warning");
	// Archives are checked once per canonical path, accepted or rejected.
	Check(std::count(faults.ArchiveCalls().begin(), faults.ArchiveCalls().end(), f / "good.zar") ==
	          1,
	      "a rejected archive is checked once");
	Check(faults.ArchiveCalls().size() == 2, "each archive is checked once");
}

void TestSortOrder() {
	const Tree                     tree;
	const std::vector<std::string> names = {"b", "B", "a", "\xC3\xA9", "Z", "a b", "a-b", "0"};
	for (const auto& name: names) {
		tree.Game("f/" + name);
	}
	const auto result = HostScan({tree.Base() / "f"});
	CheckPaths(result, tree.Base(),
	           {"f/0", "f/B", "f/Z", "f/a", "f/a b", "f/a-b", "f/b", "f/\xC3\xA9"},
	           "entries are sorted by the bytes of their paths");

	// The same tree in memory, in other listing orders, gives the same entries.
	for (const uint64_t seed: {0ull, 1ull, 2ull, 77ull}) {
		InMemoryScanFileSystem memory;
		for (const auto& name: names) {
			memory.AddFile(tree.Base() / "f" / name / "eboot.bin");
		}
		memory.SetListingOrder(seed);
		const InMemoryMetadataReader reader;
		const auto                   in_memory = Scan({tree.Base() / "f"}, memory, reader);
		Check(Relative(in_memory, tree.Base()) == Relative(result, tree.Base()),
		      "listing order " + std::to_string(seed) + " gives the same order");
	}
}

void TestCancel() {
	CancelToken token;
	Check(!token.IsCancelled(), "a new token is not cancelled");
	token.Cancel();
	token.Cancel();
	Check(token.IsCancelled(), "Cancel is one-way and may repeat");

	const Tree tree;
	tree.Game("f/a/Game");
	tree.Game("f/b/Game");
	tree.Archive("f/c.zar", true);
	const auto host   = MakeHostScanFileSystem();
	const auto reader = MakeHostMetadataReader();

	const std::vector<fs::path> folders = {tree.Base() / "f", tree.Base() / "missing"};
	const auto                  before  = ScanGameFolders(folders, *host, *reader, token);
	Check(before.cancelled && before.entries.empty() && before.warnings.empty(),
	      "a cancelled token gives a cancelled, empty result");

	// Cancelled while listing.
	for (size_t stop_after = 1; stop_after <= 4; stop_after++) {
		CancelToken                  cancel;
		FaultInjectingScanFileSystem faults(*host);
		faults.SetBeforeList([&](const fs::path&) {
			if (faults.ListCalls().size() == stop_after) {
				cancel.Cancel();
			}
		});
		const auto part = ScanGameFolders(folders, faults, *reader, cancel);
		Check(part.cancelled && part.entries.empty() && part.warnings.empty(),
		      "cancelled after " + std::to_string(stop_after) + " listings: discarded whole");
		Check(faults.ListCalls().size() == stop_after, "the scan stops at the next check");
	}

	// Cancelled while reading metadata.
	CancelToken          cancel;
	const CountingReader cancelling([&cancel] { cancel.Cancel(); });
	const auto           during = ScanGameFolders(folders, *host, cancelling, cancel);
	Check(during.cancelled && during.entries.empty(), "cancelled during a metadata read");
	Check(cancelling.Reads().size() == 1, "no further metadata is read");

	// Not cancelled: the complete result.
	const CancelToken idle;
	const auto        full = ScanGameFolders(folders, *host, *reader, idle);
	Check(!full.cancelled && full.entries.size() == 3 && full.warnings.size() == 1,
	      "an uncancelled scan is complete");
}

void TestMetadataOncePerEntry() {
	const Tree tree;
	tree.Game("f/a/Game");
	tree.Archive("f/a/one.zar", true);
	tree.Link(tree.Base() / "f", "alias");
	const auto                  host = MakeHostScanFileSystem();
	const CountingReader        reader;
	const std::vector<fs::path> folders = {tree.Base() / "f", tree.Base() / "alias",
	                                       tree.Base() / "f/a", tree.Base() / "f"};
	const auto                  result  = Scan(folders, *host, reader);
	Check(result.entries.size() == 2, "two entries");
	Check(reader.Reads().size() == 2, "metadata is read once per entry");
	Check(std::find(reader.Reads().begin(), reader.Reads().end(), tree.Base() / "f/a/Game") !=
	          reader.Reads().end(),
	      "the reader gets the canonical path");
}

void TestNormalizeGameFolders() {
	const Tree tree;
	const auto d0      = tree.Dir("d0");
	const auto d1      = tree.Dir("d0/d1");
	const auto l0      = tree.Link(d0, "l0");
	const auto missing = tree.Base() / "missing";
	const auto host    = MakeHostScanFileSystem();

	std::error_code ec;
	const auto      cwd = fs::current_path(ec);
	Check(!ec, "current_path");
	const auto relative_d0 = d0.lexically_relative(cwd).string();

	const std::vector<std::string> input  = {"",
	                                         d0.string(),
	                                         d0.string() + "/",
	                                         (tree.Base() / "./d0").string(),
	                                         (tree.Base() / "d0/../d0").string(),
	                                         l0.string(),
	                                         relative_d0,
	                                         d1.string(),
	                                         (l0 / "d1").string(),
	                                         missing.string(),
	                                         missing.string() + "/",
	                                         (tree.Base() / "./missing").string(),
	                                         (d0 / "../missing").string(),
	                                         d1.string() + "/."};
	const auto                     result = NormalizeGameFolders(input, *host);
	Check(result == std::vector<fs::path> {d0, d1, missing},
	      "one absolute path per directory, first occurrences in order");

	const std::vector<std::string> alias_first = {l0.string() + "/", d0.string(), relative_d0};
	const auto                     aliased     = NormalizeGameFolders(alias_first, *host);
	Check(aliased == std::vector<fs::path> {l0.string() + "/"},
	      "the first occurrence keeps its absolute, not canonical, spelling");

	const std::vector<std::string> relative_first = {relative_d0, d0.string()};
	const auto                     made_absolute  = NormalizeGameFolders(relative_first, *host);
	Check(made_absolute.size() == 1 && made_absolute[0] == fs::absolute(relative_d0) &&
	          made_absolute[0].is_absolute(),
	      "a relative folder is made absolute");

	// When Canonical fails for an existing path, the lexical form decides, so an alias is no
	// longer recognized.
	FaultInjectingScanFileSystem failing(*host);
	failing.FailCanonical(l0);
	const std::vector<std::string> lexical = {l0.string(), l0.string() + "/", d0.string()};
	Check(NormalizeGameFolders(lexical, failing) == std::vector<fs::path> {l0, d0},
	      "without a canonical path, spellings compare lexically");
	Check(NormalizeGameFolders(std::vector<std::string> {}, *host).empty(), "no folders");
}

void TestHostList() {
	const Tree tree;
	tree.File("d/file", "x");
	tree.Dir("d/sub");
	tree.Link("sub", "d/link");
	tree.Link("nowhere", "d/dangling");
	Check(::mkfifo((tree.Base() / "d/fifo").c_str(), 0644) == 0, "mkfifo");

	const auto host    = MakeHostScanFileSystem();
	auto       listing = host->List(tree.Base() / "d");
	Check(listing.has_value() && listing->size() == 5, "five entries");
	std::sort(listing->begin(), listing->end(),
	          [](const FsEntry& a, const FsEntry& b) { return a.name < b.name; });
	const std::vector<std::pair<std::string, FsEntry::Type>> expected = {
	    {"dangling", FsEntry::Symlink},
	    {"fifo", FsEntry::Other},
	    {"file", FsEntry::File},
	    {"link", FsEntry::Symlink},
	    {"sub", FsEntry::Directory}};
	for (size_t i = 0; i < expected.size(); i++) {
		Check((*listing)[i].name == expected[i].first && (*listing)[i].type == expected[i].second,
		      "entry " + expected[i].first + " has its own type");
	}
	Check(host->List(tree.Base() / "d/link").has_value(), "a linked directory can be listed");
	Check(host->List(tree.Base() / "d/sub")->empty(), "an empty directory");
	Check(host->List(tree.Base() / "d/file").error() == ErrorText(ENOTDIR), "a file");
	Check(host->List(tree.Base() / "nope").error() == ErrorText(ENOENT), "a missing directory");
	Check(host->Canonical(tree.Base() / "d/link/../file") == tree.Base() / "d/file",
	      "Canonical resolves links before '..'");
	Check(!host->Canonical(tree.Base() / "d/dangling").has_value(), "a dangling link");
	Check(!host->Canonical(tree.Base() / "nope").has_value(), "a missing path");
}

void TestInMemoryFileSystem() {
	InMemoryScanFileSystem memory;
	memory.AddDirectory("/r/a");
	memory.AddFile("/r/a/file");
	memory.AddArchive("/r/a/good.zar", true);
	memory.AddArchive("/r/a/bad.zar", false);
	memory.AddSymlink("/r/link", "a");
	memory.AddSymlink("/r/loop", "loop");
	memory.AddSymlink("/r/up", "..");
	memory.AddOther("/r/fifo");
	memory.SetUnreadable("/r/a");

	Check(memory.Canonical("/r/link/file") == fs::path("/r/a/file"), "through a relative link");
	Check(memory.Canonical("/r/up/r/link/") == fs::path("/r/a"), "'..' as a link target");
	Check(!memory.Canonical("/r/loop").has_value(), "a loop does not resolve");
	Check(memory.List("/r/loop").error() == ErrorText(ELOOP), "a loop is ELOOP");
	Check(!memory.Canonical("/r/a/file/").has_value(), "a file with a trailing separator");
	Check(memory.List("/r/a/file").error() == ErrorText(ENOTDIR), "a file is not a directory");
	Check(memory.List("/r/missing").error() == ErrorText(ENOENT), "a missing directory");
	Check(memory.List("/r/link").error() == "Permission denied", "an unreadable directory");
	Check(memory.ArchiveHasRootEboot("/r/a/good.zar") &&
	          !memory.ArchiveHasRootEboot("/r/a/bad.zar") &&
	          !memory.ArchiveHasRootEboot("/r/a/file") &&
	          !memory.ArchiveHasRootEboot("/r/none.zar"),
	      "archives");
	const auto root = memory.List("/r");
	Check(root.has_value() && root->size() == 5 && (*root)[0].name == "a" &&
	          (*root)[0].type == FsEntry::Directory,
	      "the root listing in name order");
	Check(memory.ListCount() == 5, "List calls are counted");

	InMemoryMetadataReader reader;
	reader.SetParamJson("/r/a/./", R"({"titleId":"PPSA00009"})");
	Check(reader.Read("/r/a", kDirectory).title_id == "PPSA00009", "param.json by normal path");
	Check(reader.Read("/r/b.zar", kArchive) == GameMetadata {"b", "", "", "", false},
	      "no text is the fallback");
	Check(reader.Reads().size() == 2, "reads are recorded");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameScannerTests");
	TestNestedGames();
	TestFolderIsGame();
	TestSymlinks();
	TestArchives();
	TestMissingFolders();
	TestOverlappingFolders();
	TestUnreadable();
	TestResolutionFailures();
	TestSortOrder();
	TestCancel();
	TestMetadataOncePerEntry();
	TestNormalizeGameFolders();
	TestHostList();
	TestInMemoryFileSystem();
	std::printf("GameScannerTests: all tests passed\n");
	return 0;
}
