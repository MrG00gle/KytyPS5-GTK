#include "launcher/core/cheats.h"

#include "launcher/core/dataPaths.h"
#include "launcher/core/titleId.h"

#include <cstddef>
#include <string>

namespace kyty::core {

namespace {

constexpr std::string_view kCheatTitlePrefix = "PPSA";
constexpr std::size_t      kCheatTitleDigits = 5;

} // namespace

bool IsCheatTitleId(std::string_view title_id) {
	const auto title = NormalizeTitleId(title_id);
	if (title.size() != kCheatTitlePrefix.size() + kCheatTitleDigits ||
	    !title.starts_with(kCheatTitlePrefix)) {
		return false;
	}
	for (std::size_t i = kCheatTitlePrefix.size(); i < title.size(); i++) {
		if (title[i] < '0' || title[i] > '9') {
			return false;
		}
	}
	return true;
}

std::filesystem::path CheatFilePath(const std::filesystem::path& data_dir,
                                    std::string_view             title_id) {
	if (!IsCheatTitleId(title_id)) {
		return {};
	}
	return CheatDirectory(data_dir) / (NormalizeTitleId(title_id) + ".json");
}

} // namespace kyty::core
