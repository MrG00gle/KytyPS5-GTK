// Feature: linux-gtk-modernization, Property 27: Trophy overview rows
//
// Validates Requirement 11.2. Each case draws a library of 0-12 entries, each with an empty or
// non-empty name, a title ID (possibly empty), and 0-3 trophy packages of 0-20 trophies (any
// grades) with arbitrary unlocks (some of the package's IDs and IDs it does not define).
// BuildTrophyOverview must give exactly one row per entry with at least one trophy, in entry order,
// and none for the others; each row shows the name (or the title ID when the name is empty), the
// title ID, the earned and total counts over the entry's packages, the counts per grade, and the
// grade-weighted percentage of Property 27 as amended on 2026-10-30, from 0 to 100:
// floor(100 * earned_points / points) with gold 6, silver 2, bronze 1, and platinum 0 points, and 0
// when points is 0 (the reference computes it here, independently of
// Common::Trophies::Progress::Percentage()); the totals add up over the rows.

#include "launcher/core/trophyModel.h"
#include "support/property.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using kyty::core::TrophyGrade;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

kyty::core::LoadedTrophyPackage Package() {
	kyty::core::LoadedTrophyPackage loaded;
	std::set<int>                   ids;
	const int                       count = OneIn(5) ? 0 : Choose(1, 21);
	while (static_cast<int>(ids.size()) < count) {
		ids.insert(Choose(0, 100));
	}
	for (const int id: ids) {
		kyty::core::TrophyDefinition trophy;
		trophy.id    = id;
		trophy.grade = static_cast<TrophyGrade>(Choose(1, 5));
		loaded.package.trophies.push_back(trophy);
		if (OneIn(2)) {
			loaded.unlocks.unlocked.insert(id);
		}
	}
	if (OneIn(3)) {
		loaded.unlocks.unlocked.insert(Choose(100, 200));
	}
	return loaded;
}

std::vector<kyty::core::TrophyOverviewEntry> Library() {
	static const std::vector<std::string> kNames    = {"", "Alpha", "Beta (EU)", "\xE6\x97\xA5"};
	static const std::vector<std::string> kTitleIds = {"", "PPSA01234", "CUSA00001", "ppsa09999"};
	std::vector<kyty::core::TrophyOverviewEntry> entries;
	const int                                    count = Choose(0, 13);
	for (int i = 0; i < count; i++) {
		kyty::core::TrophyOverviewEntry entry;
		entry.name         = kNames[Choose<std::size_t>(0, kNames.size())];
		entry.title_id     = kTitleIds[Choose<std::size_t>(0, kTitleIds.size())];
		const int packages = Choose(0, 4);
		for (int p = 0; p < packages; p++) {
			entry.packages.push_back(Package());
		}
		entries.push_back(std::move(entry));
	}
	return entries;
}

} // namespace

KYTY_PROPERTY(27, "Trophy overview rows") {
	return rc::check(name, []() {
		const auto entries  = Library();
		const auto overview = kyty::core::BuildTrophyOverview(entries);
		RC_LOG() << entries.size() << " entries, " << overview.rows.size() << " rows\n";

		std::size_t                   row_index  = 0;
		std::uint32_t                 all_earned = 0;
		kyty::core::TrophyGradeCounts all_by_grade {};
		bool                          platinum_only    = false;
		bool                          weighted_differs = false;
		for (std::size_t index = 0; index < entries.size(); index++) {
			const auto&                   entry  = entries[index];
			std::uint32_t                 earned = 0, total = 0;
			kyty::core::TrophyGradeCounts earned_by_grade {}, total_by_grade {};
			for (const auto& [package, unlocks]: entry.packages) {
				for (const auto& trophy: package.trophies) {
					const auto grade = static_cast<std::size_t>(trophy.grade) - 1;
					total++;
					total_by_grade[grade]++;
					if (unlocks.unlocked.contains(trophy.id)) {
						earned++;
						earned_by_grade[grade]++;
					}
				}
			}
			if (total == 0) {
				// No row for this entry: the next row belongs to a later entry. (RC_ASSERT does not
				// short-circuit || and &&, so the bounds check stays outside it.)
				if (row_index < overview.rows.size()) {
					RC_ASSERT(overview.rows[row_index].entry_index != index);
				}
				continue;
			}
			RC_ASSERT(row_index < overview.rows.size());
			const auto& row = overview.rows[row_index++];
			RC_ASSERT(row.entry_index == index);
			RC_ASSERT(row.name == (entry.name.empty() ? entry.title_id : entry.name));
			RC_ASSERT(row.title_id == entry.title_id);
			RC_ASSERT(row.earned == earned);
			RC_ASSERT(row.total == total);
			RC_ASSERT(row.earned_by_grade == earned_by_grade);
			RC_ASSERT(row.total_by_grade == total_by_grade);
			// Counts are {platinum, gold, silver, bronze}.
			const std::uint64_t points =
			    6 * std::uint64_t {total_by_grade[1]} + 2 * total_by_grade[2] + total_by_grade[3];
			const std::uint64_t earned_points = 6 * std::uint64_t {earned_by_grade[1]} +
			                                    2 * earned_by_grade[2] + earned_by_grade[3];
			const int percent = points == 0 ? 0 : static_cast<int>(earned_points * 100 / points);
			RC_ASSERT(row.percent == percent);
			RC_ASSERT(row.percent >= 0);
			RC_ASSERT(row.percent <= 100);
			if (points == 0) {
				platinum_only = true;
			} else if (percent != static_cast<int>(std::uint64_t {earned} * 100 / total)) {
				weighted_differs = true;
			}
			all_earned += earned;
			for (std::size_t g = 0; g < all_by_grade.size(); g++) {
				all_by_grade[g] += earned_by_grade[g];
			}
		}
		RC_ASSERT(row_index == overview.rows.size());
		RC_ASSERT(overview.earned == all_earned);
		RC_ASSERT(overview.earned_by_grade == all_by_grade);

		RC_CLASSIFY(overview.rows.size() < entries.size(), "entries without trophy data");
		RC_CLASSIFY(!overview.rows.empty(), "rows");
		RC_CLASSIFY(platinum_only, "a row without points (platinum only)");
		RC_CLASSIFY(weighted_differs, "the weighted percentage differs from the unweighted one");
	});
}
