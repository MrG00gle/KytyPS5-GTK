#pragma once

// Trophy views (task 16.1; design.md, "Trophy views over core::TrophyModel"; Requirements 11.1 and
// 11.2), ported from the Qt launcher's trophy viewer and trophy overview
// (src/launcher/src/trophyViewerDialog.cpp). BuildTrophyViewer and BuildTrophyOverview are pure:
// they turn trophy packages and unlock data into what the trophy window and the trophy overview
// show. LoadGameTrophies reads both for a Game_Entry through Common::Trophies (plan D20).

#include "launcher/core/gameMetadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

enum class TrophyGrade { Platinum = 1, Gold = 2, Silver = 3, Bronze = 4 };

// "Platinum", "Gold", "Silver", or "Bronze".
std::string_view TrophyGradeText(TrophyGrade grade);

// A count per grade, indexed by TrophyGradeIndex: Platinum, Gold, Silver, Bronze.
using TrophyGradeCounts = std::array<std::uint32_t, 4>;

constexpr std::size_t TrophyGradeIndex(TrophyGrade grade) {
	return static_cast<std::size_t>(grade) - 1;
}

// The grade-weighted progress of Property 27 (amended on 2026-10-30 by the user's decision):
// floor(100 * earned_points / points), where points = 6 * gold + 2 * silver + bronze over
// total_by_grade, earned_points is the same sum over earned_by_grade, platinum trophies count 0,
// and the result is 0 when points is 0. It fills a Common::Trophies::Progress and returns its
// Percentage() (uint32 arithmetic, as the Emulator and the Qt launcher), so the trophy viewer
// header and the trophy overview rows show what the PS5 and the Qt launcher show. The counts must
// satisfy earned_by_grade[g] <= total_by_grade[g], as BuildTrophyViewer and BuildTrophyOverview
// count them; the result is then 0-100.
int ProgressPercent(const TrophyGradeCounts& earned_by_grade,
                    const TrophyGradeCounts& total_by_grade);

// The placeholder that replaces the name and the description of a hidden trophy while it is locked
// and the user has not revealed it (Requirement 11.1); the Qt launcher's "Hidden trophy".
inline constexpr std::string_view kHiddenTrophyPlaceholder = "Hidden trophy";

// The message when a Game_Entry has no trophy package (the Qt launcher's text).
inline constexpr std::string_view kNoTrophyPackageMessage =
    "No trophy package found in sce_sys/trophy2.";

// The trophy overview's text when no Game_Entry has trophy data (the Qt launcher's text).
inline constexpr std::string_view kNoTrophyGamesMessage =
    "No games with trophies found in the game folders.";

// One trophy as its package defines it (the fields of Common::Trophies::Trophy that the views use).
struct TrophyDefinition {
	int         id         = 0;
	TrophyGrade grade      = TrophyGrade::Bronze;
	bool        hidden     = false;
	bool        has_reward = false;
	std::string name;
	std::string description;
	std::string reward;
	std::string icon_png; // the bytes of the package's trop<NNNN>.png; empty when it has none

	friend bool operator==(const TrophyDefinition&, const TrophyDefinition&) = default;
};

// A readable trophy package, sce_sys/trophy2/trophy<N>.ucp of a Game_Entry.
struct TrophyPackage {
	std::string                   file_name;         // "trophy00.ucp"
	std::uint32_t                 service_label = 0; // N, which names the unlock file
	std::string                   title;             // the package's title name
	std::vector<TrophyDefinition> trophies;          // in trophy ID order

	friend bool operator==(const TrophyPackage&, const TrophyPackage&) = default;
};

// The unlock data of one package (Common::Trophies::UnlockData): the unlocked trophy IDs and the
// unlock time of some of them, in microseconds since 0001-01-01 00:00 UTC (the Emulator's ticks).
struct TrophyUnlocks {
	std::set<int>                unlocked;
	std::map<int, std::uint64_t> timestamps;

	friend bool operator==(const TrophyUnlocks&, const TrophyUnlocks&) = default;
};

struct LoadedTrophyPackage {
	TrophyPackage package;
	TrophyUnlocks unlocks;

	friend bool operator==(const LoadedTrophyPackage&, const LoadedTrophyPackage&) = default;
};

// One row of the trophy viewer.
struct TrophyRow {
	int              id    = 0;
	TrophyGrade      grade = TrophyGrade::Bronze;
	std::string_view grade_text; // TrophyGradeText(grade), shown for every trophy
	bool             unlocked = false;
	bool             hidden   = false; // the package marks the trophy hidden
	// Hidden, locked, and not revealed: name, description, and display_name are
	// kHiddenTrophyPlaceholder, reward and icon_png are empty (the view shows its hidden-trophy
	// icon and a reveal action).
	bool        masked = false;
	std::string name;         // the package value, or the placeholder while masked
	std::string description;  // the package value, or the placeholder while masked
	std::string display_name; // name, or "Trophy <id>" when the package has no name (as Qt)
	// The package has a reward text (the Qt launcher's "Reward" row); reward is empty while masked.
	bool        has_reward = false;
	std::string reward;
	// Unlocked with a recorded unlock time at or after 1970-01-01: milliseconds since then (UTC).
	std::optional<std::int64_t> earned_unix_ms;
	std::string icon_png; // the package's icon bytes (the view grays it while locked)

	friend bool operator==(const TrophyRow&, const TrophyRow&) = default;
};

// One package page of the trophy viewer.
struct TrophyViewer {
	std::string            file_name;
	std::string            title;
	std::uint32_t          earned  = 0; // trophies of the package whose ID is unlocked
	std::uint32_t          total   = 0; // trophies of the package
	int                    percent = 0; // ProgressPercent(earned_by_grade, total_by_grade)
	TrophyGradeCounts      earned_by_grade {};
	TrophyGradeCounts      total_by_grade {};
	std::vector<TrophyRow> trophies; // every trophy of the package, in package order
};

// The viewer page of a package with its unlock data. A trophy ID in revealed shows its real name
// and description even while it is hidden and locked; unlock IDs that the package does not define
// are ignored.
TrophyViewer BuildTrophyViewer(const TrophyPackage& package, const TrophyUnlocks& unlocks,
                               const std::set<int>& revealed);

// The trophy data of a Game_Entry.
struct GameTrophies {
	// The trophy<N>.ucp files found in sce_sys/trophy2 (kNoTrophyPackageMessage when 0).
	std::size_t package_files = 0;
	// The readable packages with their unlock data, in file name order.
	std::vector<LoadedTrophyPackage> packages;
	// One "Could not read trophy package <file name>." per package that cannot be read or defines
	// no trophy (as the Qt launcher), in file name order (Requirement 11.1).
	std::vector<std::string> errors;
};

// One Game_Entry for the trophy overview.
struct TrophyOverviewEntry {
	std::string                      name;     // GameEntry::title_name
	std::string                      title_id; // GameEntry::title_id
	std::vector<LoadedTrophyPackage> packages; // GameTrophies::packages
};

struct TrophyOverviewRow {
	std::size_t       entry_index = 0; // the entry's index in the BuildTrophyOverview input
	std::string       name;            // the entry's name, or its title ID when the name is empty
	std::string       title_id;
	std::uint32_t     earned  = 0; // over all of the entry's packages
	std::uint32_t     total   = 0;
	int               percent = 0; // ProgressPercent(earned_by_grade, total_by_grade)
	TrophyGradeCounts earned_by_grade {};
	TrophyGradeCounts total_by_grade {};

	friend bool operator==(const TrophyOverviewRow&, const TrophyOverviewRow&) = default;
};

struct TrophyOverview {
	// One row per entry with trophy data (at least one trophy in its packages), in entry order; no
	// row for the others (Requirement 11.2).
	std::vector<TrophyOverviewRow> rows;
	std::uint32_t                  earned = 0; // over all rows (the Qt launcher's "Total")
	TrophyGradeCounts              earned_by_grade {};
};

TrophyOverview BuildTrophyOverview(std::span<const TrophyOverviewEntry> entries);

// A trophy package file of a Game_Entry.
struct TrophyPackageFile {
	std::filesystem::path path;      // a host path, or "<entry>.zar!/sce_sys/trophy2/<file name>"
	std::string           file_name; // "trophy00.ucp"
	std::uint32_t         service_label = 0;
};

// The files of sce_sys/trophy2 named "trophy" + one or more ASCII digits + ".ucp" (letter case
// matters), sorted by name, as the Qt launcher found them. For a directory entry only regular files
// count (symbolic links are skipped, as Qt did, and so are FIFOs and devices, which Qt would have
// opened); for an archive entry the archive's files. service_label is the decimal number, or 0 when
// it does not fit in 32 bits (Qt's QString::toUInt). Empty when the directory is missing.
std::vector<TrophyPackageFile> FindTrophyPackageFiles(const GameEntry& entry);

// Reads every package of FindTrophyPackageFiles(entry) with Common::Trophies::LoadPackage in
// console_language (tropmeta_<locale>.json, else the package's default language) and its unlock
// data with Common::Trophies::LoadUnlockData from <data_dir>/_Trophies/<entry.title_id>/
// trophies_<user_id>_<N>.json (Common::Trophies::UnlocksPath; a missing or unreadable file means
// nothing is unlocked).
GameTrophies LoadGameTrophies(const GameEntry& entry, const std::filesystem::path& data_dir,
                              int user_id, int console_language);

} // namespace kyty::core
