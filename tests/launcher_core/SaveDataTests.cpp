// Example tests of save data listing and removal (src/launcher/core/saveData.h, task 16.3;
// Requirement 11.13; plan D21): the title ID used as given (no trimming, no upper-casing, as the
// Emulator writes it) and its safety rule, the directories listed for the confirmation dialog,
// removal confined to _SaveData/<TITLE_ID> with links removed and never followed (a case variant
// of the title ID is another directory), and every path that could not be deleted, with failures
// injected through KytyTests::ContentGen::FailingRemover (never through permissions).

#include "launcher/core/dataPaths.h"
#include "launcher/core/saveData.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "SaveDataTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// The snapshot of data_dir without the entries at or below "_SaveData/<title>".
std::map<std::string, std::string> Without(std::map<std::string, std::string> tree,
                                           const std::string&                 title) {
	const std::string prefix = "_SaveData/" + title;
	std::erase_if(tree, [&](const auto& item) {
		return item.first == prefix || item.first.starts_with(prefix + "/");
	});
	return tree;
}

// A Data_Directory with the save data of several titles, trophies, and look-alike names.
void MakeDataDirectory(const KytyTests::TempDir& temp) {
	temp.WriteFile("data/_SaveData/PPSA01234/0/savedata0/sce_sys/param.sfo", "sfo");
	temp.WriteFile("data/_SaveData/PPSA01234/0/savedata0/data.bin", std::string("\0save\xFF", 6));
	temp.MakeDirectories("data/_SaveData/PPSA01234/1/empty_dir");
	temp.MakeSymlink("../PPSA05678", "data/_SaveData/PPSA01234/link_out");
	temp.MakeSymlink("../PPSA05678/keep.bin", "data/_SaveData/PPSA01234/file_link");
	temp.MakeSymlink("/nonexistent/kyty", "data/_SaveData/PPSA01234/dangling");
	temp.WriteFile("data/_SaveData/PPSA05678/keep.bin", "other title");
	temp.WriteFile("data/_SaveData/PPSA05678/sub/more.bin", "more");
	temp.WriteFile("data/_SaveData/ppsa01234/lower.bin", "lower-case sibling");
	temp.WriteFile("data/_SaveData/PPSA012345/x", "longer title");
	temp.WriteFile("data/_SaveData/PPSA01234.bak", "a file next to it");
	temp.WriteFile("data/_Trophies/PPSA01234/trophies_1000_0.json", "{}");
	temp.WriteFile("data/Kyty.ini", "[Launcher]\n");
}

void TestSaveDataPath() {
	const auto path = kyty::core::SaveDataPath("/data", "PPSA01234");
	Check(path && path->native() == "/data/_SaveData/PPSA01234", "<data_dir>/_SaveData/<TITLE_ID>");
	Check(*path == kyty::core::SaveDataDirectory("/data") / "PPSA01234", "below SaveDataDirectory");
	// The title ID as given, as the Emulator writes it (src/libs/libSaveData.cpp): no trimming, no
	// upper-casing.
	for (const std::string_view title:
	     {"ppsa01234", "PpSa01234", " PPSA01234", "PPSA01234 ", " ppsa01234\t", "   ", " .. ",
	      "...", "CUSA00001_00", "a.b@c-d", "\xC3\xA9t\xC3\xA9"}) {
		const auto as_given = kyty::core::SaveDataPath("/data", title);
		Check(as_given && as_given->native() == "/data/_SaveData/" + std::string(title),
		      "'" + std::string(title) + "' is used as given");
	}
	for (const std::string_view title:
	     {std::string_view(""), std::string_view("."), std::string_view(".."),
	      std::string_view("a/b"), std::string_view("/"), std::string_view("../PPSA01234"),
	      std::string_view("PPSA01234/"), std::string_view(" ../x"), std::string_view("PPSA\0x", 6),
	      std::string_view("\0", 1)}) {
		const auto refused = kyty::core::SaveDataPath("/data", title);
		Check(!refused &&
		          refused.error().find("does not name a save data directory") != std::string::npos,
		      "'" + std::string(title) + "' is refused");
	}
	Check(kyty::core::SaveDataPath("/data", "ppsa/b").error() ==
	          "The title ID \"ppsa/b\" does not name a save data directory.",
	      "the refusal names the title ID as given");
}

void TestList() {
	const KytyTests::TempDir temp("kyty_save_data");
	MakeDataDirectory(temp);
	const auto data   = temp.Path() / "data";
	const auto listed = kyty::core::ListSaveData(data, "PPSA01234");
	Check(listed && *listed == std::vector<fs::path> {data / "_SaveData/PPSA01234"},
	      "an existing save data directory is listed");
	const auto lower = kyty::core::ListSaveData(data, "ppsa01234");
	Check(lower && *lower == std::vector<fs::path> {data / "_SaveData/ppsa01234"},
	      "the lower-case title ID lists its own directory");
	Check(kyty::core::ListSaveData(data, "ppsa01234 ")->empty() &&
	          kyty::core::ListSaveData(data, " PPSA01234")->empty() &&
	          kyty::core::ListSaveData(data, "Ppsa01234")->empty(),
	      "other spellings name directories that do not exist");
	Check(kyty::core::ListSaveData(data, "PPSA00000")->empty(), "a title without save data");
	Check(kyty::core::ListSaveData(data, "PPSA01234.bak")->empty(), "a file is not listed");
	temp.MakeSymlink("PPSA05678", "data/_SaveData/PPSA00001");
	temp.MakeSymlink("PPSA99999", "data/_SaveData/PPSA00002");
	Check(kyty::core::ListSaveData(data, "PPSA00001")->size() == 1, "a link to a directory");
	Check(kyty::core::ListSaveData(data, "PPSA00002")->empty(), "a dangling link");
	Check(!kyty::core::ListSaveData(data, "..") && !kyty::core::ListSaveData(data, ""),
	      "unsafe title IDs are refused");
	Check(kyty::core::ListSaveData(temp.Path() / "missing", "PPSA01234")->empty(),
	      "a missing Data_Directory");
}

bool IsBelow(const fs::path& path, const fs::path& directory) {
	const auto relative = path.lexically_relative(directory);
	return !relative.empty() && *relative.begin() != "..";
}

void TestRemove() {
	const KytyTests::TempDir temp("kyty_save_data");
	MakeDataDirectory(temp);
	const auto data   = temp.Path() / "data";
	const auto title  = data / "_SaveData/PPSA01234";
	const auto before = ContentGen::SnapshotTree(data);

	ContentGen::FailingRemover remover;
	const auto                 removal = kyty::core::RemoveSaveData(data, "PPSA01234", remover);
	Check(removal && removal->directory == title && removal->failures.empty(),
	      "everything removed without failures");
	Check(!fs::exists(fs::symlink_status(title)), "_SaveData/PPSA01234 is gone");
	Check(ContentGen::SnapshotTree(data) == Without(before, "PPSA01234"),
	      "every other path below the Data_Directory is unchanged (link targets included)");
	const auto& calls = remover.Calls();
	Check(calls.size() == 11 && calls.back() == title, "eleven removals, the directory last");
	for (std::size_t i = 0; i < calls.size(); i++) {
		Check(calls[i] == title || IsBelow(calls[i], title),
		      "only paths in the title directory: " + calls[i].string());
		for (std::size_t j = i + 1; j < calls.size(); j++) {
			Check(calls[i] != calls[j] && !IsBelow(calls[j], calls[i]),
			      "deepest first, each once: " + calls[j].string() + " after " + calls[i].string());
		}
	}

	const auto again = kyty::core::RemoveSaveData(data, "PPSA01234", remover);
	Check(again && again->failures.empty() && remover.Calls().size() == 11,
	      "removing missing save data succeeds without calling the remover");
}

// The title ID is used as given: a lower-case ID removes only its own directory, and a title ID
// with a space names yet another directory.
void TestRemoveCaseVariants() {
	const KytyTests::TempDir temp("kyty_save_data");
	MakeDataDirectory(temp);
	temp.WriteFile("data/_SaveData/ PPSA01234/space.bin", "leading space");
	const auto data   = temp.Path() / "data";
	const auto before = ContentGen::SnapshotTree(data);

	ContentGen::FailingRemover remover;
	const auto                 lower = kyty::core::RemoveSaveData(data, "ppsa01234", remover);
	Check(lower && lower->directory == data / "_SaveData/ppsa01234" && lower->failures.empty(),
	      "the lower-case title ID removes _SaveData/ppsa01234");
	Check(remover.Calls() == std::vector<fs::path> {data / "_SaveData/ppsa01234/lower.bin",
	                                                data / "_SaveData/ppsa01234"},
	      "only the lower-case directory is touched");
	Check(ContentGen::SnapshotTree(data) == Without(before, "ppsa01234"),
	      "_SaveData/PPSA01234 and every other path stay byte-identical");

	const auto after_lower = ContentGen::SnapshotTree(data);
	const auto spaced      = kyty::core::RemoveSaveData(data, " PPSA01234", remover);
	Check(spaced && spaced->directory == data / "_SaveData/ PPSA01234" && spaced->failures.empty(),
	      "' PPSA01234' with a space is a different directory");
	Check(ContentGen::SnapshotTree(data) == Without(after_lower, " PPSA01234") &&
	          fs::exists(data / "_SaveData/PPSA01234/0/savedata0/data.bin"),
	      "removing it leaves _SaveData/PPSA01234 alone");

	const auto mixed = kyty::core::RemoveSaveData(data, "PpSa01234", remover);
	Check(mixed && mixed->directory == data / "_SaveData/PpSa01234" && mixed->failures.empty() &&
	          ContentGen::SnapshotTree(data) == Without(after_lower, " PPSA01234"),
	      "a mixed-case title ID without save data removes nothing");
}

void TestRemoveFailures() {
	const KytyTests::TempDir temp("kyty_save_data");
	MakeDataDirectory(temp);
	const auto data   = temp.Path() / "data";
	const auto title  = data / "_SaveData/PPSA01234";
	const auto before = ContentGen::SnapshotTree(data);

	ContentGen::FailingRemover remover;
	remover.FailOn(title / "0/savedata0/data.bin", std::errc::permission_denied);
	remover.FailOn(title / "1/empty_dir", std::errc::device_or_resource_busy);
	remover.FailOn(title / "link_out", std::errc::operation_not_permitted);
	const auto removal = kyty::core::RemoveSaveData(data, "PPSA01234", remover);
	Check(removal.has_value(), "the removal runs");
	const std::vector<fs::path> expected_calls = {title / "0/savedata0/data.bin",
	                                              title / "0/savedata0/sce_sys/param.sfo",
	                                              title / "0/savedata0/sce_sys",
	                                              title / "0/savedata0",
	                                              title / "0",
	                                              title / "1/empty_dir",
	                                              title / "1",
	                                              title / "dangling",
	                                              title / "file_link",
	                                              title / "link_out",
	                                              title};
	Check(remover.Calls() == expected_calls,
	      "the entries of each directory in name order, then it");

	const auto& failures = removal->failures;
	Check(failures.size() == 7, "seven paths not deleted");
	const auto failed = [&](std::size_t index, const fs::path& path, std::errc error) {
		Check(failures[index].path == path && failures[index].error == error,
		      "failure " + std::to_string(index) + ": " + failures[index].path.string() + ": " +
		          failures[index].error.message());
	};
	failed(0, title / "0/savedata0/data.bin", std::errc::permission_denied);
	failed(1, title / "0/savedata0", std::errc::directory_not_empty);
	failed(2, title / "0", std::errc::directory_not_empty);
	failed(3, title / "1/empty_dir", std::errc::device_or_resource_busy);
	failed(4, title / "1", std::errc::directory_not_empty);
	failed(5, title / "link_out", std::errc::operation_not_permitted);
	failed(6, title, std::errc::directory_not_empty);

	std::map<std::string, std::string> remaining;
	for (const auto& [path, kind]: ContentGen::SnapshotTree(title)) {
		remaining[path] = kind.substr(0, 1);
	}
	Check(remaining == std::map<std::string, std::string> {{"0", "D"},
	                                                       {"0/savedata0", "D"},
	                                                       {"0/savedata0/data.bin", "F"},
	                                                       {"1", "D"},
	                                                       {"1/empty_dir", "D"},
	                                                       {"link_out", "L"}},
	      "exactly the failed paths and their directories remain");
	Check(Without(ContentGen::SnapshotTree(data), "PPSA01234") == Without(before, "PPSA01234"),
	      "other titles unchanged");
}

void TestRemoveSpecialCases() {
	const KytyTests::TempDir temp("kyty_save_data");
	MakeDataDirectory(temp);
	const auto data = temp.Path() / "data";

	// The title directory is a link: only the link goes.
	temp.MakeSymlink("PPSA05678", "data/_SaveData/PPSA00001");
	auto                       before = ContentGen::SnapshotTree(data);
	ContentGen::FailingRemover link_remover;
	const auto                 link = kyty::core::RemoveSaveData(data, "PPSA00001", link_remover);
	Check(link && link->failures.empty() &&
	          link_remover.Calls() == std::vector<fs::path> {data / "_SaveData/PPSA00001"} &&
	          ContentGen::SnapshotTree(data) == Without(before, "PPSA00001"),
	      "a linked title directory: the link is removed, its target kept");

	// A regular file in its place is removed; the title ID is used as given, so the upper-case
	// "PPSA01234.BAK" is a different path from the file "PPSA01234.bak".
	temp.WriteFile("data/_SaveData/PPSA00003.BAK", "file");
	const auto file = kyty::core::RemoveSaveData(data, "PPSA00003.BAK");
	Check(file && file->failures.empty() && !fs::exists(data / "_SaveData/PPSA00003.BAK"),
	      "a file at the save data path is removed");
	const auto upper = kyty::core::RemoveSaveData(data, "PPSA01234.BAK");
	Check(upper && upper->directory == data / "_SaveData/PPSA01234.BAK" &&
	          upper->failures.empty() && fs::exists(data / "_SaveData/PPSA01234.bak"),
	      "save data paths use the title ID as given");

	// Refused title IDs touch nothing.
	before = ContentGen::SnapshotTree(data);
	ContentGen::FailingRemover refused_remover;
	for (const std::string_view title: {"", "..", ".", "../_Trophies", "PPSA05678/sub"}) {
		const auto refused = kyty::core::RemoveSaveData(data, title, refused_remover);
		Check(!refused, "'" + std::string(title) + "' is refused");
	}
	Check(refused_remover.Calls().empty() && ContentGen::SnapshotTree(data) == before,
	      "a refused title ID removes nothing");

	// A path that cannot even be checked (a name longer than NAME_MAX).
	const std::string long_title(300, 'A');
	const auto        too_long = kyty::core::RemoveSaveData(data, long_title, refused_remover);
	Check(too_long && too_long->failures.size() == 1 &&
	          too_long->failures[0].path == data / "_SaveData" / long_title &&
	          too_long->failures[0].error == std::errc::filename_too_long &&
	          refused_remover.Calls().empty(),
	      "an unusable save data path is reported, nothing removed");

	// A directory that vanishes before it is listed is not a failure.
	temp.WriteFile("data/_SaveData/PPSA00010/a", "a");
	temp.WriteFile("data/_SaveData/PPSA00010/b/c", "c");
	ContentGen::FailingRemover vanishing;
	vanishing.before_remove = [&](const fs::path& path) {
		if (path.filename() == "a") {
			std::error_code ec;
			fs::remove_all(path.parent_path() / "b", ec);
		}
	};
	const auto vanished = kyty::core::RemoveSaveData(data, "PPSA00010", vanishing);
	Check(vanished && vanished->failures.empty() && !fs::exists(data / "_SaveData/PPSA00010") &&
	          vanishing.Calls().size() == 2,
	      "a directory removed meanwhile is skipped");

	// A directory that cannot be listed is reported and kept.
	temp.WriteFile("data/_SaveData/PPSA00011/a", "a");
	temp.WriteFile("data/_SaveData/PPSA00011/b/c", "c");
	ContentGen::FailingRemover replacing;
	replacing.before_remove = [&](const fs::path& path) {
		if (path.filename() == "a") {
			std::error_code ec;
			fs::remove_all(path.parent_path() / "b", ec);
			Check(ContentGen::WriteHostFile(path.parent_path() / "b", "now a file"), "replace b");
		}
	};
	const auto title    = data / "_SaveData/PPSA00011";
	const auto unlisted = kyty::core::RemoveSaveData(data, "PPSA00011", replacing);
	Check(unlisted && unlisted->failures.size() == 2 && unlisted->failures[0].path == title / "b" &&
	          unlisted->failures[0].error == std::errc::not_a_directory &&
	          unlisted->failures[1].path == title &&
	          unlisted->failures[1].error == std::errc::directory_not_empty &&
	          fs::exists(title / "b") && !fs::exists(title / "a"),
	      "an unlistable directory is reported and not removed; its parent then is not empty");
}

void TestHostRemover() {
	const KytyTests::TempDir temp("kyty_save_data");
	auto&                    remover = kyty::core::HostPathRemover();
	const auto               file    = temp.WriteFile("file", "x");
	Check(!remover.Remove(file) && !fs::exists(file), "a file is removed");
	Check(!remover.Remove(file), "a missing path counts as removed");
	const auto dir = temp.MakeDirectories("dir/sub");
	Check(remover.Remove(dir.parent_path()) == std::errc::directory_not_empty && fs::exists(dir),
	      "a directory that is not empty is kept");
	Check(!remover.Remove(dir) && !remover.Remove(dir.parent_path()) &&
	          !fs::exists(dir.parent_path()),
	      "empty directories are removed");
	const auto link = temp.MakeSymlink(temp.Path(), "self");
	Check(!remover.Remove(link) && fs::exists(temp.Path()), "a link is removed, not followed");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("SaveDataTests");
	TestSaveDataPath();
	TestList();
	TestRemove();
	TestRemoveCaseVariants();
	TestRemoveFailures();
	TestRemoveSpecialCases();
	TestHostRemover();
	std::printf("SaveDataTests: all tests passed\n");
	return 0;
}
