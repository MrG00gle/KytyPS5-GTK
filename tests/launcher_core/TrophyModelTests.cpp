// Example tests of the trophy views (src/launcher/core/trophyModel.h, task 16.1; Requirements 11.1
// and 11.2): grade texts, progress percentages, the viewer rows with the hidden-trophy placeholder,
// the overview rows, and the loader over real trophy packages built at test time
// (tests/support/contentGenerators.h) in a game directory and in a .zar packed with
// Common::PackArchive, with unlock files written through Common::Trophies::SaveUnlockData.

#include "common/archive.h"
#include "common/trophies.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/trophyModel.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace ContentGen = KytyTests::ContentGen;

using kyty::core::GameEntry;
using kyty::core::TrophyGrade;
using kyty::core::TrophyUnlocks;

constexpr int           kUserId    = 1000;
constexpr int           kEnglish   = 1; // Common::Localization: en-US
constexpr int           kFrench    = 2; // fr-FR
constexpr int           kJapanese  = 0; // ja-JP
constexpr char          kTitleId[] = "PPSA01234";
constexpr std::uint64_t kTick      = Common::Trophies::UnixEpochTick + 1791288000123456ULL;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "TrophyModelTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

ContentGen::TrophySpec Trophy(int id, char grade, bool hidden, std::optional<std::string> name,
                              std::optional<std::string> detail = std::nullopt) {
	ContentGen::TrophySpec trophy;
	trophy.id     = id;
	trophy.grade  = grade;
	trophy.hidden = hidden;
	trophy.name   = std::move(name);
	trophy.detail = std::move(detail);
	return trophy;
}

// Six trophies: every grade, hidden ones, one without texts, rewards, icons.
ContentGen::TrophyPackageSpec MainSpec() {
	ContentGen::TrophyPackageSpec spec;
	spec.title = "Test game";
	spec.trophies.push_back(Trophy(0, 'P', false, "Platinum Cup", "Earn every trophy."));
	spec.trophies.back().icon = std::string("PNG0\0\x89", 6);
	spec.trophies.push_back(Trophy(1, 'G', true, "Secret Gold", "Found the secret."));
	spec.trophies.back().has_reward = true;
	spec.trophies.back().reward     = "A hat";
	spec.trophies.back().icon       = "PNG1";
	spec.trophies.push_back(Trophy(2, 'S', true, "Secret Silver", "Second secret."));
	spec.trophies.push_back(Trophy(3, 'B', false, std::nullopt));
	spec.trophies.push_back(Trophy(4, 'B', false, "Bronze \"quoted\"", "Line\nbreak \\ and tab\t"));
	spec.trophies.back().has_reward = true; // without a reward text
	spec.trophies.push_back(Trophy(12, 'B', true, "Late", "Late one"));
	spec.trophies.back().has_reward = true;
	spec.trophies.back().reward     = "R";
	return spec;
}

ContentGen::TrophyPackageSpec DlcSpec() {
	ContentGen::TrophyPackageSpec spec;
	spec.title = "Test game DLC \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC";
	spec.trophies.push_back(Trophy(1, 'B', false, "DLC Bronze"));
	spec.trophies.push_back(Trophy(0, 'G', false, "DLC Gold"));
	return spec;
}

ContentGen::TrophyPackageSpec SmallSpec(const std::string& title) {
	ContentGen::TrophyPackageSpec spec;
	spec.title = title;
	spec.trophies.push_back(Trophy(0, 'S', false, title + " silver"));
	return spec;
}

TrophyUnlocks MainUnlocks() {
	TrophyUnlocks unlocks;
	unlocks.unlocked   = {0, 2, 999};
	unlocks.timestamps = {{0, kTick}};
	return unlocks;
}

void TestGradeTexts() {
	Check(kyty::core::TrophyGradeText(TrophyGrade::Platinum) == "Platinum", "Platinum");
	Check(kyty::core::TrophyGradeText(TrophyGrade::Gold) == "Gold", "Gold");
	Check(kyty::core::TrophyGradeText(TrophyGrade::Silver) == "Silver", "Silver");
	Check(kyty::core::TrophyGradeText(TrophyGrade::Bronze) == "Bronze", "Bronze");
	Check(kyty::core::TrophyGradeIndex(TrophyGrade::Platinum) == 0 &&
	          kyty::core::TrophyGradeIndex(TrophyGrade::Bronze) == 3,
	      "grade indexes");
}

void TestProgressPercent() {
	constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
	struct Case {
		std::uint64_t earned;
		std::uint64_t total;
		int           percent;
	};
	for (const Case& c: {Case {0, 0, 0}, Case {5, 0, 0}, Case {0, 1, 0}, Case {1, 3, 33},
	                     Case {2, 3, 66}, Case {1, 1, 100}, Case {3, 2, 100}, Case {1, 100, 1},
	                     Case {199, 200, 99}, Case {kMax - 1, kMax, 99}, Case {1, kMax, 0},
	                     Case {kMax / 2, kMax, 49}, Case {kMax, kMax, 100}}) {
		Check(kyty::core::ProgressPercent(c.earned, c.total) == c.percent,
		      "ProgressPercent(" + std::to_string(c.earned) + ", " + std::to_string(c.total) +
		          ") == " + std::to_string(c.percent));
	}
}

void TestViewer() {
	const auto package = ContentGen::ExpectedTrophyPackage(MainSpec(), "trophy00.ucp", 0);
	auto       unlocks = MainUnlocks();
	// A time for a locked trophy and one before 1970 are not earned dates.
	unlocks.timestamps[3] = kTick;
	unlocks.timestamps[2] = Common::Trophies::UnixEpochTick - 1;
	const auto viewer     = kyty::core::BuildTrophyViewer(package, unlocks, {12});

	Check(viewer.file_name == "trophy00.ucp" && viewer.title == "Test game", "viewer title");
	Check(viewer.total == 6 && viewer.earned == 2 && viewer.percent == 33,
	      "2 of 6 earned (unlock ID 999 is not in the package), 33 %");
	Check(viewer.total_by_grade == kyty::core::TrophyGradeCounts {1, 1, 1, 3} &&
	          viewer.earned_by_grade == kyty::core::TrophyGradeCounts {1, 0, 1, 0},
	      "counts per grade");
	Check(viewer.trophies.size() == 6, "every trophy listed");

	const auto& platinum = viewer.trophies[0];
	Check(platinum.id == 0 && platinum.grade == TrophyGrade::Platinum &&
	          platinum.grade_text == "Platinum" && platinum.unlocked && !platinum.hidden &&
	          !platinum.masked && platinum.name == "Platinum Cup" &&
	          platinum.display_name == "Platinum Cup" &&
	          platinum.description == "Earn every trophy." && !platinum.has_reward &&
	          platinum.reward.empty() && platinum.earned_unix_ms == 1791288000123 &&
	          platinum.icon_png == std::string("PNG0\0\x89", 6),
	      "an unlocked trophy with its earned date and icon");

	const auto& gold = viewer.trophies[1];
	Check(gold.id == 1 && gold.hidden && !gold.unlocked && gold.masked &&
	          gold.name == kyty::core::kHiddenTrophyPlaceholder &&
	          gold.description == kyty::core::kHiddenTrophyPlaceholder &&
	          gold.display_name == "Hidden trophy" && gold.grade_text == "Gold" &&
	          gold.has_reward && gold.reward.empty() && gold.icon_png.empty() &&
	          !gold.earned_unix_ms,
	      "a locked hidden trophy shows the placeholder, its grade, and no reward text or icon");

	const auto& silver = viewer.trophies[2];
	Check(silver.hidden && silver.unlocked && !silver.masked && silver.name == "Secret Silver" &&
	          silver.description == "Second secret." && !silver.earned_unix_ms,
	      "an unlocked hidden trophy is shown; a time before 1970 is no earned date");

	const auto& unnamed = viewer.trophies[3];
	Check(unnamed.name.empty() && unnamed.display_name == "Trophy 3" &&
	          unnamed.description.empty() && !unnamed.unlocked && !unnamed.earned_unix_ms,
	      "a trophy without a name is 'Trophy <id>'; a locked trophy has no earned date");

	const auto& bronze = viewer.trophies[4];
	Check(bronze.name == "Bronze \"quoted\"" && bronze.description == "Line\nbreak \\ and tab\t" &&
	          !bronze.has_reward && bronze.reward.empty(),
	      "has_reward without a reward text shows no reward");

	const auto& revealed = viewer.trophies[5];
	Check(revealed.id == 12 && revealed.hidden && !revealed.unlocked && !revealed.masked &&
	          revealed.name == "Late" && revealed.description == "Late one" &&
	          revealed.has_reward && revealed.reward == "R",
	      "a revealed hidden trophy shows its texts and reward");

	const auto masked_again = kyty::core::BuildTrophyViewer(package, unlocks, {});
	Check(masked_again.trophies[5].masked && masked_again.trophies[5].name == "Hidden trophy",
	      "without the reveal the trophy is masked");
	const auto all_revealed = kyty::core::BuildTrophyViewer(package, unlocks, {1, 12, 77});
	Check(!all_revealed.trophies[1].masked && all_revealed.trophies[1].name == "Secret Gold" &&
	          all_revealed.trophies[1].reward == "A hat" &&
	          all_revealed.trophies[1].icon_png == "PNG1",
	      "revealing shows the name, reward, and icon");

	const auto empty = kyty::core::BuildTrophyViewer({}, {}, {});
	Check(empty.trophies.empty() && empty.total == 0 && empty.percent == 0, "an empty package");
}

void TestOverview() {
	const auto    main = ContentGen::ExpectedTrophyPackage(MainSpec(), "trophy00.ucp", 0);
	const auto    dlc  = ContentGen::ExpectedTrophyPackage(DlcSpec(), "trophy01.ucp", 1);
	const auto    one  = ContentGen::ExpectedTrophyPackage(SmallSpec("One"), "trophy00.ucp", 0);
	TrophyUnlocks dlc_unlocks;
	dlc_unlocks.unlocked = {1};
	TrophyUnlocks one_unlocks;
	one_unlocks.unlocked = {0};

	std::vector<kyty::core::TrophyOverviewEntry> entries(5);
	entries[0]          = {"Alpha", "PPSA00001", {{main, MainUnlocks()}, {dlc, dlc_unlocks}}};
	entries[1]          = {"Beta", "PPSA00002", {}};
	entries[2]          = {"", "PPSA00003", {{one, one_unlocks}}};
	entries[3]          = {"Gamma", "", {{kyty::core::TrophyPackage {}, TrophyUnlocks {}}}};
	entries[4]          = {"Delta", "CUSA00004", {{main, TrophyUnlocks {}}}};
	const auto overview = kyty::core::BuildTrophyOverview(entries);

	Check(overview.rows.size() == 3, "rows only for entries with trophies");
	const auto& alpha = overview.rows[0];
	Check(alpha.entry_index == 0 && alpha.name == "Alpha" && alpha.title_id == "PPSA00001" &&
	          alpha.earned == 3 && alpha.total == 8 && alpha.percent == 37,
	      "the packages of an entry add up: 3 of 8, 37 %");
	Check(alpha.total_by_grade == kyty::core::TrophyGradeCounts {1, 2, 1, 4} &&
	          alpha.earned_by_grade == kyty::core::TrophyGradeCounts {1, 0, 1, 1},
	      "grade counts over both packages");
	const auto& unnamed = overview.rows[1];
	Check(unnamed.entry_index == 2 && unnamed.name == "PPSA00003" &&
	          unnamed.title_id == "PPSA00003" && unnamed.earned == 1 && unnamed.total == 1 &&
	          unnamed.percent == 100,
	      "an entry without a name shows its title ID");
	const auto& delta = overview.rows[2];
	Check(delta.entry_index == 4 && delta.name == "Delta" && delta.earned == 0 &&
	          delta.total == 6 && delta.percent == 0,
	      "nothing earned is 0 %");
	Check(overview.earned == 4 &&
	          overview.earned_by_grade == kyty::core::TrophyGradeCounts {1, 0, 2, 1},
	      "overview totals");
	Check(kyty::core::BuildTrophyOverview({}).rows.empty(), "an empty library");
	Check(kyty::core::kNoTrophyGamesMessage == "No games with trophies found in the game folders.",
	      "the empty overview text");
}

// A game directory with every kind of name in sce_sys/trophy2.
void MakeGameDirectory(const KytyTests::TempDir& temp, const fs::path& game) {
	const auto dir = game / Common::Trophies::PackageDirectory;
	temp.WriteFile(dir / "trophy00.ucp", ContentGen::TrophyPackageBytes(MainSpec()));
	temp.WriteFile(dir / "trophy01.ucp", ContentGen::TrophyPackageBytes(DlcSpec()));
	temp.WriteFile(dir / "trophy10.ucp", ContentGen::TrophyPackageBytes(SmallSpec("Ten")));
	temp.WriteFile(dir / "trophy99999999999.ucp",
	               ContentGen::TrophyPackageBytes(SmallSpec("Overflow")));
	temp.WriteFile(dir / "trophy05.ucp", "not a trophy package");
	ContentGen::TrophyPackageSpec no_trophies;
	temp.WriteFile(dir / "trophy06.ucp", ContentGen::TrophyPackageBytes(no_trophies));
	for (const char* name: {"trophy.ucp", "trophyA.ucp", "Trophy00.ucp", "trophy00.UCP",
	                        "trophy00.ucp.bak", "uds00.ucp", "trophy-1.ucp", "trophy0x1.ucp"}) {
		temp.WriteFile(dir / name, ContentGen::TrophyPackageBytes(SmallSpec(name)));
	}
	temp.MakeSymlink("trophy01.ucp", dir / "trophy02.ucp");
	temp.MakeDirectories(dir / "trophy03.ucp");
	Check(::mkfifo((temp.Path() / dir / "trophy07.ucp").c_str(), 0600) == 0, "make a FIFO");
}

void TestFindFiles() {
	const KytyTests::TempDir temp("kyty_trophy_model");
	MakeGameDirectory(temp, "game");
	GameEntry entry;
	entry.path                     = temp.Path() / "game";
	entry.title_id                 = kTitleId;
	const auto               files = kyty::core::FindTrophyPackageFiles(entry);
	std::vector<std::string> names;
	for (const auto& file: files) {
		names.push_back(file.file_name);
		Check(file.path == entry.path / "sce_sys/trophy2" / file.file_name, "host path");
	}
	Check(names == std::vector<std::string> {"trophy00.ucp", "trophy01.ucp", "trophy05.ucp",
	                                         "trophy06.ucp", "trophy10.ucp",
	                                         "trophy99999999999.ucp"},
	      "trophy<digits>.ucp regular files in name order; links, directories, FIFOs, and other "
	      "names skipped");
	Check(files[0].service_label == 0 && files[1].service_label == 1 &&
	          files[4].service_label == 10 && files[5].service_label == 0,
	      "service labels; a number above 32 bits is 0, as QString::toUInt");

	GameEntry missing;
	missing.path = temp.Path() / "missing";
	Check(kyty::core::FindTrophyPackageFiles(missing).empty(), "a missing directory");
}

void TestLoadDirectory() {
	const KytyTests::TempDir temp("kyty_trophy_model");
	MakeGameDirectory(temp, "game");
	const auto    data_dir = temp.Path() / "data";
	TrophyUnlocks dlc_unlocks;
	dlc_unlocks.unlocked = {1};
	TrophyUnlocks ten_unlocks;
	ten_unlocks.unlocked = {0};
	Check(ContentGen::WriteUnlocks(data_dir, kTitleId, kUserId, 0, MainUnlocks()) &&
	          ContentGen::WriteUnlocks(data_dir, kTitleId, kUserId, 1, dlc_unlocks) &&
	          ContentGen::WriteUnlocks(data_dir, kTitleId, 2000, 10, ten_unlocks),
	      "write unlock files");
	Check(fs::exists(kyty::core::TrophyDataDirectory(data_dir) / kTitleId / "trophies_1000_0.json"),
	      "unlock files live in TrophyDataDirectory/<title>");

	GameEntry entry;
	entry.path      = temp.Path() / "game";
	entry.title_id  = kTitleId;
	const auto game = kyty::core::LoadGameTrophies(entry, data_dir, kUserId, kEnglish);
	Check(game.package_files == 6, "six package files");
	Check(game.errors == std::vector<std::string> {"Could not read trophy package trophy05.ucp.",
	                                               "Could not read trophy package trophy06.ucp."},
	      "one error per unreadable package, naming it; a package without trophies is unreadable");
	Check(game.packages.size() == 4, "four readable packages");
	Check(game.packages[0].package ==
	          ContentGen::ExpectedTrophyPackage(MainSpec(), "trophy00.ucp", 0),
	      "trophy00.ucp read in full");
	Check(game.packages[0].unlocks == ContentGen::StoredUnlocks(MainUnlocks()),
	      "trophy00.ucp unlocks");
	Check(game.packages[1].package ==
	              ContentGen::ExpectedTrophyPackage(DlcSpec(), "trophy01.ucp", 1) &&
	          game.packages[1].unlocks == dlc_unlocks,
	      "trophy01.ucp with its own unlock file; trophies in ID order");
	Check(game.packages[2].package.file_name == "trophy10.ucp" &&
	          game.packages[2].package.service_label == 10 &&
	          game.packages[2].unlocks.unlocked.empty(),
	      "another user's unlock file is not read");
	Check(game.packages[3].package.title == "Overflow" &&
	          game.packages[3].unlocks == ContentGen::StoredUnlocks(MainUnlocks()),
	      "label 0 for an oversized number reads the unlock file of label 0 (Qt parity)");

	const auto other_user = kyty::core::LoadGameTrophies(entry, data_dir, 2000, kEnglish);
	Check(other_user.packages[0].unlocks.unlocked.empty() &&
	          other_user.packages[2].unlocks.unlocked == std::set<int> {0},
	      "the user ID selects the unlock files");

	// The viewer over the loaded data.
	const auto viewer =
	    kyty::core::BuildTrophyViewer(game.packages[0].package, game.packages[0].unlocks, {});
	Check(viewer.earned == 2 && viewer.trophies[0].earned_unix_ms == 1791288000123,
	      "the earned date survives the unlock file");

	GameEntry unsafe       = entry;
	unsafe.title_id        = "../PPSA01234";
	const auto unsafe_game = kyty::core::LoadGameTrophies(unsafe, data_dir, kUserId, kEnglish);
	Check(unsafe_game.packages.size() == 4 && unsafe_game.packages[0].unlocks.unlocked.empty(),
	      "a title ID that Common::Trophies::UnlocksPath refuses reads no unlock file");
}

void TestLoadArchive() {
	const KytyTests::TempDir temp("kyty_trophy_model");
	const fs::path           dir = fs::path("source") / Common::Trophies::PackageDirectory;
	temp.WriteFile(dir / "trophy00.ucp", ContentGen::TrophyPackageBytes(MainSpec()));
	temp.WriteFile(dir / "trophy01.ucp", ContentGen::TrophyPackageBytes(DlcSpec()));
	temp.WriteFile(dir / "trophy05.ucp", "garbage");
	temp.WriteFile(dir / "trophy.ucp", ContentGen::TrophyPackageBytes(SmallSpec("x")));
	temp.WriteFile(dir / "notes.txt", "notes");
	temp.MakeDirectories(dir / "trophy03.ucp");
	temp.WriteFile(dir / "trophy03.ucp/inner", "inner");
	temp.WriteFile("source/eboot.bin", "ELF");
	const auto archive = temp.Path() / "Game.zar";
	Check(Common::PackArchive(temp.Path() / "source", archive,
	                          [](std::uint64_t, std::uint64_t) { return true; }),
	      "pack Game.zar");
	const auto data_dir = temp.Path() / "data";
	Check(ContentGen::WriteUnlocks(data_dir, kTitleId, kUserId, 0, MainUnlocks()),
	      "write the unlock file");

	GameEntry entry;
	entry.path       = archive;
	entry.kind       = GameEntry::Kind::Archive;
	entry.title_id   = kTitleId;
	const auto files = kyty::core::FindTrophyPackageFiles(entry);
	Check(files.size() == 3 && files[0].file_name == "trophy00.ucp" &&
	          files[0].path == Common::MakeArchivePath(archive, "sce_sys/trophy2/trophy00.ucp") &&
	          files[2].file_name == "trophy05.ucp",
	      "archive members: files only, by name");
	const auto game = kyty::core::LoadGameTrophies(entry, data_dir, kUserId, kEnglish);
	Check(game.package_files == 3 && game.packages.size() == 2 &&
	          game.errors ==
	              std::vector<std::string> {"Could not read trophy package trophy05.ucp."},
	      "two readable packages and one error");
	Check(game.packages[0].package ==
	              ContentGen::ExpectedTrophyPackage(MainSpec(), "trophy00.ucp", 0) &&
	          game.packages[0].unlocks == ContentGen::StoredUnlocks(MainUnlocks()),
	      "an archive package reads like a directory package");
	Check(game.packages[1].package ==
	              ContentGen::ExpectedTrophyPackage(DlcSpec(), "trophy01.ucp", 1) &&
	          game.packages[1].unlocks.unlocked.empty(),
	      "no unlock file means nothing unlocked");

	GameEntry missing = entry;
	missing.path      = temp.Path() / "Missing.zar";
	const auto none   = kyty::core::LoadGameTrophies(missing, data_dir, kUserId, kEnglish);
	Check(none.package_files == 0 && none.packages.empty() && none.errors.empty(),
	      "a missing archive has no trophy data");
}

void TestConsoleLanguage() {
	const KytyTests::TempDir temp("kyty_trophy_model");
	auto                     spec   = SmallSpec("English");
	auto                     french = spec.trophies;
	french[0].name                  = "Argent";
	// "Français" (split so that the 'a' does not extend the \xA7 escape).
	const std::string french_title          = "Fran\xC3\xA7"
	                                          "ais";
	spec.extra_files["tropmeta_fr-FR.json"] = ContentGen::TropmetaJson(french_title, french);
	temp.WriteFile("game/sce_sys/trophy2/trophy00.ucp", ContentGen::TrophyPackageBytes(spec));
	GameEntry entry;
	entry.path          = temp.Path() / "game";
	entry.title_id      = kTitleId;
	const auto data_dir = temp.Path() / "data";

	const auto french_game = kyty::core::LoadGameTrophies(entry, data_dir, kUserId, kFrench);
	Check(french_game.packages.size() == 1 &&
	          french_game.packages[0].package.title == french_title &&
	          french_game.packages[0].package.trophies[0].name == "Argent",
	      "the console language selects tropmeta_fr-FR.json");
	const auto english = kyty::core::LoadGameTrophies(entry, data_dir, kUserId, kEnglish);
	Check(english.packages[0].package.title == "English" &&
	          english.packages[0].package.trophies[0].name == "English silver",
	      "tropmeta_en-US.json for English");
	const auto japanese = kyty::core::LoadGameTrophies(entry, data_dir, kUserId, kJapanese);
	Check(japanese.packages[0].package.title == "English",
	      "a missing language falls back to the default language");
}

void TestNoTrophyData() {
	const KytyTests::TempDir temp("kyty_trophy_model");
	temp.WriteFile("game/eboot.bin", "ELF");
	temp.MakeDirectories("empty/sce_sys/trophy2");
	for (const char* name: {"game", "empty"}) {
		GameEntry entry;
		entry.path      = temp.Path() / name;
		const auto game = kyty::core::LoadGameTrophies(entry, temp.Path(), kUserId, kEnglish);
		Check(game.package_files == 0 && game.packages.empty() && game.errors.empty(),
		      std::string(name) + ": no trophy data");
	}
	Check(kyty::core::kNoTrophyPackageMessage == "No trophy package found in sce_sys/trophy2.",
	      "the no-package text");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("TrophyModelTests");
	TestGradeTexts();
	TestProgressPercent();
	TestViewer();
	TestOverview();
	TestFindFiles();
	TestLoadDirectory();
	TestLoadArchive();
	TestConsoleLanguage();
	TestNoTrophyData();
	std::printf("TrophyModelTests: all tests passed\n");
	return 0;
}
