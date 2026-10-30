#include "launcher/core/trophyModel.h"

#include "common/archive.h"
#include "common/file.h"
#include "common/trophies.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTrophyDirectory = Common::Trophies::PackageDirectory;
constexpr std::string_view kFilePrefix      = "trophy";
constexpr std::string_view kFileSuffix      = ".ucp";

// The N of "trophy<N>.ucp" (one or more ASCII digits), or nullopt for any other name.
std::optional<std::uint32_t> ServiceLabel(std::string_view name) {
	if (name.size() <= kFilePrefix.size() + kFileSuffix.size() || !name.starts_with(kFilePrefix) ||
	    !name.ends_with(kFileSuffix)) {
		return std::nullopt;
	}
	const auto digits =
	    name.substr(kFilePrefix.size(), name.size() - kFilePrefix.size() - kFileSuffix.size());
	if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		return std::nullopt;
	}
	std::uint32_t label     = 0;
	const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), label);
	// QString::toUInt gives 0 for a number that does not fit.
	return error == std::errc {} && end == digits.data() + digits.size() ? label : 0;
}

// Earned and total trophies, also per grade; the one counting rule of the viewer and the overview.
struct Progress {
	std::uint32_t     earned = 0;
	std::uint32_t     total  = 0;
	TrophyGradeCounts earned_by_grade {};
	TrophyGradeCounts total_by_grade {};

	void Add(TrophyGrade grade, bool unlocked) {
		const auto index = TrophyGradeIndex(grade);
		total++;
		total_by_grade[index]++;
		if (unlocked) {
			earned++;
			earned_by_grade[index]++;
		}
	}

	void Add(const TrophyPackage& package, const TrophyUnlocks& unlocks) {
		for (const auto& trophy: package.trophies) {
			Add(trophy.grade, unlocks.unlocked.contains(trophy.id));
		}
	}
};

void AddCounts(TrophyGradeCounts& sum, const TrophyGradeCounts& add) {
	for (std::size_t i = 0; i < sum.size(); i++) {
		sum[i] += add[i];
	}
}

TrophyGrade GradeFromCommon(int grade) {
	// Common::Trophies::LoadPackage keeps only the grades "PGSB", numbered 1-4.
	return static_cast<TrophyGrade>(std::clamp(grade, 1, 4));
}

TrophyPackage ConvertPackage(Common::Trophies::Package&& source, const TrophyPackageFile& file) {
	TrophyPackage package;
	package.file_name     = file.file_name;
	package.service_label = file.service_label;
	package.title         = std::move(source.title);
	package.trophies.reserve(source.trophies.size());
	for (auto& [id, trophy]: source.trophies) {
		TrophyDefinition definition;
		definition.id          = id;
		definition.grade       = GradeFromCommon(trophy.grade);
		definition.hidden      = trophy.hidden;
		definition.has_reward  = trophy.has_reward;
		definition.name        = std::move(trophy.name);
		definition.description = std::move(trophy.description);
		definition.reward      = std::move(trophy.reward);
		definition.icon_png.assign(reinterpret_cast<const char*>(trophy.icon_png.data()),
		                           trophy.icon_png.size());
		package.trophies.push_back(std::move(definition));
	}
	return package;
}

} // namespace

std::string_view TrophyGradeText(TrophyGrade grade) {
	switch (grade) {
		case TrophyGrade::Platinum: return "Platinum";
		case TrophyGrade::Gold: return "Gold";
		case TrophyGrade::Silver: return "Silver";
		case TrophyGrade::Bronze: return "Bronze";
	}
	return {};
}

int ProgressPercent(std::uint64_t earned, std::uint64_t total) {
	if (total == 0) {
		return 0;
	}
	if (earned >= total) {
		return 100;
	}
	return static_cast<int>(static_cast<unsigned __int128>(earned) * 100 / total);
}

TrophyViewer BuildTrophyViewer(const TrophyPackage& package, const TrophyUnlocks& unlocks,
                               const std::set<int>& revealed) {
	TrophyViewer viewer;
	viewer.file_name = package.file_name;
	viewer.title     = package.title;
	viewer.trophies.reserve(package.trophies.size());
	for (const auto& trophy: package.trophies) {
		TrophyRow row;
		row.id         = trophy.id;
		row.grade      = trophy.grade;
		row.grade_text = TrophyGradeText(trophy.grade);
		row.unlocked   = unlocks.unlocked.contains(trophy.id);
		row.hidden     = trophy.hidden;
		row.masked     = trophy.hidden && !row.unlocked && !revealed.contains(trophy.id);
		row.has_reward = trophy.has_reward && !trophy.reward.empty();
		if (row.masked) {
			row.name         = kHiddenTrophyPlaceholder;
			row.description  = kHiddenTrophyPlaceholder;
			row.display_name = kHiddenTrophyPlaceholder;
		} else {
			row.name        = trophy.name;
			row.description = trophy.description;
			row.display_name =
			    trophy.name.empty() ? "Trophy " + std::to_string(trophy.id) : trophy.name;
			row.reward   = row.has_reward ? trophy.reward : std::string {};
			row.icon_png = trophy.icon_png;
		}
		if (const auto tick = unlocks.timestamps.find(trophy.id);
		    row.unlocked && tick != unlocks.timestamps.end() &&
		    tick->second >= Common::Trophies::UnixEpochTick) {
			row.earned_unix_ms =
			    static_cast<std::int64_t>((tick->second - Common::Trophies::UnixEpochTick) / 1000);
		}

		viewer.trophies.push_back(std::move(row));
	}
	Progress progress;
	progress.Add(package, unlocks);
	viewer.earned          = progress.earned;
	viewer.total           = progress.total;
	viewer.earned_by_grade = progress.earned_by_grade;
	viewer.total_by_grade  = progress.total_by_grade;
	viewer.percent         = ProgressPercent(progress.earned, progress.total);
	return viewer;
}

TrophyOverview BuildTrophyOverview(std::span<const TrophyOverviewEntry> entries) {
	TrophyOverview overview;
	for (std::size_t index = 0; index < entries.size(); index++) {
		const auto&       entry = entries[index];
		TrophyOverviewRow row;
		row.entry_index = index;
		row.name        = entry.name.empty() ? entry.title_id : entry.name;
		row.title_id    = entry.title_id;
		Progress progress;
		for (const auto& [package, unlocks]: entry.packages) {
			progress.Add(package, unlocks);
		}
		if (progress.total == 0) {
			continue;
		}
		row.earned          = progress.earned;
		row.total           = progress.total;
		row.earned_by_grade = progress.earned_by_grade;
		row.total_by_grade  = progress.total_by_grade;
		row.percent         = ProgressPercent(progress.earned, progress.total);
		overview.earned += row.earned;
		AddCounts(overview.earned_by_grade, row.earned_by_grade);
		overview.rows.push_back(std::move(row));
	}
	return overview;
}

std::vector<TrophyPackageFile> FindTrophyPackageFiles(const GameEntry& entry) {
	std::vector<TrophyPackageFile> files;
	if (entry.kind == GameEntry::Kind::Archive) {
		const auto directory = Common::MakeArchivePath(entry.path, fs::path(kTrophyDirectory));
		for (const auto& item: Common::File::GetDirEntries(directory)) {
			if (const auto label = ServiceLabel(item.name); label && item.is_file) {
				files.push_back({directory / item.name, item.name, *label});
			}
		}
	} else {
		const auto      directory = entry.path / fs::path(kTrophyDirectory);
		std::error_code ec;
		for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
			auto            name  = it->path().filename().string();
			const auto      label = ServiceLabel(name);
			std::error_code status_ec;
			if (label && it->symlink_status(status_ec).type() == fs::file_type::regular) {
				files.push_back({it->path(), std::move(name), *label});
			}
		}
	}
	std::sort(files.begin(), files.end(),
	          [](const auto& a, const auto& b) { return a.file_name < b.file_name; });
	return files;
}

GameTrophies LoadGameTrophies(const GameEntry& entry, const fs::path& data_dir, int user_id,
                              int console_language) {
	// Keep the archive index and block cache alive across the package reads, as the Qt launcher.
	const auto reader =
	    entry.kind == GameEntry::Kind::Archive ? Common::OpenArchive(entry.path) : nullptr;

	GameTrophies result;
	const auto   files   = FindTrophyPackageFiles(entry);
	result.package_files = files.size();
	for (const auto& file: files) {
		auto source = Common::Trophies::LoadPackage(file.path, console_language);
		if (source.trophies.empty()) {
			result.errors.push_back("Could not read trophy package " + file.file_name + ".");
			continue;
		}
		auto unlock_data = Common::Trophies::LoadUnlockData(
		    Common::Trophies::UnlocksPath(data_dir, entry.title_id, user_id, file.service_label));
		LoadedTrophyPackage loaded;
		loaded.package            = ConvertPackage(std::move(source), file);
		loaded.unlocks.unlocked   = std::move(unlock_data.unlocked);
		loaded.unlocks.timestamps = std::move(unlock_data.timestamps);
		result.packages.push_back(std::move(loaded));
	}
	return result;
}

} // namespace kyty::core
