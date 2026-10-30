// Feature: linux-gtk-modernization, Property 26: Trophy viewer content
//
// Validates Requirement 11.1. Each case draws a trophy package of 0-30 trophies (distinct IDs, any
// grade, hidden or not, names, descriptions, and rewards that are absent, empty, or UTF-8 text with
// quotes, backslashes, and control characters, optional icon bytes), an unlock set (some of the
// package's IDs plus IDs it does not define, with unlock times before, at, and after 1970), and a
// set of revealed IDs. BuildTrophyViewer must list every trophy in package order with its grade
// text and lock state; the name and description are the package values, except for hidden trophies
// that are locked and not revealed, which show "Hidden trophy" for both (and no reward text or
// icon); the counts and the percentage follow the unlocked IDs of the package. In about one case in
// four the package and its unlock file are also written for real
// (tests/support/contentGenerators.h) and read through LoadGameTrophies, which must give the same
// package and unlock data, or one error naming the file when the package has no trophy.

#include "common/trophies.h"
#include "launcher/core/trophyModel.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

using kyty::core::TrophyGrade;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

// 0-12 code points of valid UTF-8, with JSON metacharacters and control characters.
std::string Text() {
	static const std::vector<std::string> kPieces = {"a",
	                                                 "Z",
	                                                 "0",
	                                                 " ",
	                                                 "\"",
	                                                 "\\",
	                                                 "/",
	                                                 "\n",
	                                                 "\t",
	                                                 std::string(1, '\0'),
	                                                 "\x01",
	                                                 "\x1F",
	                                                 "\xC3\xA9",
	                                                 "\xE6\x97\xA5",
	                                                 "\xF0\x9F\x8F\x86",
	                                                 "Trophy",
	                                                 "{",
	                                                 "}"};
	std::string                           text;
	const int                             count = Choose(0, 13);
	for (int i = 0; i < count; i++) {
		text += kPieces[Choose<std::size_t>(0, kPieces.size())];
	}
	return text;
}

std::optional<std::string> OptionalText() {
	switch (Choose(0, 6)) {
		case 0: return std::nullopt;
		case 1: return std::string();
		default: return Text();
	}
}

std::string IconBytes() {
	std::string bytes;
	const int   size = Choose(0, 33);
	for (int i = 0; i < size; i++) {
		bytes += static_cast<char>(Choose(0, 256));
	}
	return bytes;
}

ContentGen::TrophyPackageSpec PackageSpec() {
	ContentGen::TrophyPackageSpec spec;
	spec.title = Text();
	std::set<int> ids;
	const int     count = Choose(0, 31);
	while (static_cast<int>(ids.size()) < count) {
		ids.insert(Choose(0, 200));
	}
	for (const int id: ids) {
		ContentGen::TrophySpec trophy;
		trophy.id         = id;
		trophy.grade      = "PGSB"[Choose(0, 4)];
		trophy.hidden     = OneIn(3);
		trophy.has_reward = OneIn(3);
		trophy.name       = OptionalText();
		trophy.detail     = OptionalText();
		trophy.reward     = OneIn(2) ? OptionalText() : std::nullopt;
		if (!OneIn(3)) {
			trophy.icon = IconBytes();
		}
		spec.trophies.push_back(std::move(trophy));
	}
	// Package files list the trophies in any order; the package itself is in ID order.
	if (OneIn(2)) {
		std::reverse(spec.trophies.begin(), spec.trophies.end());
	}
	return spec;
}

std::uint64_t Tick() {
	constexpr std::uint64_t kEpoch = Common::Trophies::UnixEpochTick;
	switch (Choose(0, 5)) {
		case 0: return kEpoch - Choose<std::uint64_t>(1, 1000000);
		case 1: return kEpoch;
		case 2: return 315537897599999999ULL;
		default: return kEpoch + Choose<std::uint64_t>(0, 4102444800000000ULL);
	}
}

kyty::core::TrophyUnlocks Unlocks(const ContentGen::TrophyPackageSpec& spec) {
	kyty::core::TrophyUnlocks unlocks;
	for (const auto& trophy: spec.trophies) {
		if (OneIn(2)) {
			unlocks.unlocked.insert(trophy.id);
		}
		if (OneIn(3)) {
			unlocks.timestamps[trophy.id] = Tick();
		}
	}
	const int extra = Choose(0, 4);
	for (int i = 0; i < extra; i++) {
		const int id = Choose(201, 400);
		unlocks.unlocked.insert(id);
		if (OneIn(2)) {
			unlocks.timestamps[id] = Tick();
		}
	}
	return unlocks;
}

std::set<int> Revealed(const ContentGen::TrophyPackageSpec& spec) {
	std::set<int> revealed;
	for (const auto& trophy: spec.trophies) {
		if (OneIn(3)) {
			revealed.insert(trophy.id);
		}
	}
	if (OneIn(2)) {
		revealed.insert(Choose(201, 400));
	}
	return revealed;
}

std::string GradeName(TrophyGrade grade) {
	switch (grade) {
		case TrophyGrade::Platinum: return "Platinum";
		case TrophyGrade::Gold: return "Gold";
		case TrophyGrade::Silver: return "Silver";
		case TrophyGrade::Bronze: return "Bronze";
	}
	return "?";
}

void CheckViewer(const kyty::core::TrophyPackage& package, const kyty::core::TrophyUnlocks& unlocks,
                 const std::set<int>& revealed, bool& any_masked) {
	const auto viewer = kyty::core::BuildTrophyViewer(package, unlocks, revealed);
	RC_ASSERT(viewer.title == package.title);
	RC_ASSERT(viewer.trophies.size() == package.trophies.size());
	std::uint32_t earned = 0;
	for (std::size_t i = 0; i < package.trophies.size(); i++) {
		const auto& trophy = package.trophies[i];
		const auto& row    = viewer.trophies[i];
		const bool  locked = !unlocks.unlocked.contains(trophy.id);
		const bool  masked = trophy.hidden && locked && !revealed.contains(trophy.id);
		RC_ASSERT(row.id == trophy.id);
		RC_ASSERT(row.grade == trophy.grade);
		RC_ASSERT(std::string(row.grade_text) == GradeName(trophy.grade));
		RC_ASSERT(row.unlocked == !locked);
		RC_ASSERT(row.hidden == trophy.hidden);
		RC_ASSERT(row.masked == masked);
		RC_ASSERT(row.has_reward == (trophy.has_reward && !trophy.reward.empty()));
		if (masked) {
			any_masked = true;
			RC_ASSERT(row.name == "Hidden trophy");
			RC_ASSERT(row.description == "Hidden trophy");
			RC_ASSERT(row.display_name == "Hidden trophy");
			RC_ASSERT(row.reward.empty());
			RC_ASSERT(row.icon_png.empty());
		} else {
			RC_ASSERT(row.name == trophy.name);
			RC_ASSERT(row.description == trophy.description);
			RC_ASSERT(row.display_name ==
			          (trophy.name.empty() ? "Trophy " + std::to_string(trophy.id) : trophy.name));
			RC_ASSERT(row.reward == (row.has_reward ? trophy.reward : std::string()));
			RC_ASSERT(row.icon_png == trophy.icon_png);
		}
		const auto tick = unlocks.timestamps.find(trophy.id);
		if (!locked && tick != unlocks.timestamps.end() &&
		    tick->second >= Common::Trophies::UnixEpochTick) {
			RC_ASSERT(
			    row.earned_unix_ms ==
			    static_cast<std::int64_t>((tick->second - Common::Trophies::UnixEpochTick) / 1000));
		} else {
			RC_ASSERT(!row.earned_unix_ms.has_value());
		}
		earned += locked ? 0 : 1;
	}
	RC_ASSERT(viewer.earned == earned);
	RC_ASSERT(viewer.total == package.trophies.size());
	RC_ASSERT(viewer.percent ==
	          (package.trophies.empty() ? 0 : static_cast<int>(earned * 100 / viewer.total)));
}

} // namespace

KYTY_PROPERTY(26, "Trophy viewer content") {
	return rc::check(name, []() {
		const auto spec     = PackageSpec();
		const auto unlocks  = Unlocks(spec);
		const auto revealed = Revealed(spec);
		const auto label    = Choose<std::uint32_t>(0, 100);
		char       file_name[32];
		std::snprintf(file_name, sizeof(file_name), "trophy%02u.ucp", label);
		const auto package = ContentGen::ExpectedTrophyPackage(spec, file_name, label);
		RC_LOG() << spec.trophies.size() << " trophies, " << unlocks.unlocked.size()
		         << " unlocked IDs, " << revealed.size() << " revealed\n";

		bool any_masked = false;
		CheckViewer(package, unlocks, revealed, any_masked);
		RC_CLASSIFY(any_masked, "hidden trophies masked");
		RC_CLASSIFY(package.trophies.empty(), "empty package");

		if (OneIn(4)) {
			const KytyTests::TempDir temp("kyty_p26");
			temp.WriteFile(std::string("game/sce_sys/trophy2/") + file_name,
			               ContentGen::TrophyPackageBytes(spec));
			const auto data_dir = temp.Path() / "data";
			RC_ASSERT(ContentGen::WriteUnlocks(data_dir, "PPSA01234", 1000, label, unlocks));
			kyty::core::GameEntry entry;
			entry.path      = temp.Path() / "game";
			entry.title_id  = "PPSA01234";
			const auto game = kyty::core::LoadGameTrophies(entry, data_dir, 1000, 1);
			RC_ASSERT(game.package_files == 1U);
			if (package.trophies.empty()) {
				RC_ASSERT(game.packages.empty());
				RC_ASSERT(game.errors ==
				          std::vector<std::string> {"Could not read trophy package " +
				                                    std::string(file_name) + "."});
				RC_TAG("real file: no trophy");
			} else {
				RC_ASSERT(game.errors.empty());
				RC_ASSERT(game.packages.size() == 1U);
				RC_ASSERT(game.packages[0].package == package);
				RC_ASSERT(game.packages[0].unlocks == ContentGen::StoredUnlocks(unlocks));
				CheckViewer(game.packages[0].package, game.packages[0].unlocks, revealed,
				            any_masked);
				RC_TAG("real file");
			}
		}
	});
}
