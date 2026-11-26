#include "launcher/core/gameMetadata.h"

#include "launcher/core/gameContent.h"
#include "launcher/core/text.h"
#include "launcher/core/version.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace kyty::core {

namespace {

using Json = nlohmann::json;

// The trimmed string value of object[key]; "" when object is not an object, the key is missing, or
// the value is not a string (QJsonValue::toString().trimmed() in the Qt launcher).
std::string GetString(const Json& object, const std::string& key) {
	if (!object.is_object()) {
		return {};
	}
	const auto it = object.find(key);
	if (it == object.end()) {
		return {};
	}
	const auto* value = it->get_ptr<const std::string*>();
	return value != nullptr ? std::string(TrimWhitespace(*value)) : std::string();
}

std::string GetLocalizedTitleName(const Json& root) {
	const auto localized = root.find("localizedParameters");
	if (localized == root.end() || !localized->is_object()) {
		return {};
	}

	const auto default_language = GetString(*localized, "defaultLanguage");
	if (!default_language.empty()) {
		const auto it = localized->find(default_language);
		if (it != localized->end()) {
			auto title = GetString(*it, "titleName");
			if (!title.empty()) {
				return title;
			}
		}
	}

	const auto english = localized->find("en-US");
	if (english != localized->end()) {
		auto title = GetString(*english, "titleName");
		if (!title.empty()) {
			return title;
		}
	}

	// Members in key order, as QJsonObject iterates them.
	for (const auto& [language, value]: localized->items()) {
		auto title = GetString(value, "titleName");
		if (!title.empty()) {
			return title;
		}
	}
	return {};
}

std::string GetTitleId(const Json& root) {
	auto title_id = GetString(root, "titleId");
	if (!title_id.empty()) {
		return title_id;
	}
	// "UP0000-PPSA01234_00-0123456789ABCDEF": the title ID is bytes [7, 16).
	const auto content_id = GetString(root, "contentId");
	if (content_id.size() == 36 && content_id[6] == '-' && content_id[16] == '_') {
		return content_id.substr(7, 9);
	}
	return {};
}

// attribute3 bit 22: the title supports PS5 Pro mode (upstream KytyPS5's launcher, commit
// bd28a152).
constexpr std::uint64_t kProModeAttribute = std::uint64_t {1} << 22u;

bool SupportsProMode(const Json& root) {
	const auto it = root.find("attribute3");
	if (it == root.end()) {
		return false;
	}
	std::uint64_t bits = 0;
	if (it->is_number_unsigned()) {
		// Above the qint64 range Qt reads a double that toInteger() does not convert.
		bits = it->get<std::uint64_t>();
		if (bits > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
			return false;
		}
	} else if (it->is_number_integer()) {
		bits = static_cast<std::uint64_t>(it->get<std::int64_t>());
	} else if (it->is_number_float()) {
		// QJsonValue::toInteger(): a whole number that fits in a qint64, else 0.
		const double value = it->get<double>();
		if (!std::isfinite(value) || std::trunc(value) != value || value < -0x1p63 ||
		    value >= 0x1p63) {
			return false;
		}
		bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
	} else {
		return false;
	}
	return (bits & kProModeAttribute) != 0;
}

// A launchActivity intent among gameIntent.permittedIntents (upstream KytyPS5's launcher, commit
// 2638b1b0).
bool HasActivityIntent(const Json& root) {
	const auto intent = root.find("gameIntent");
	if (intent == root.end() || !intent->is_object()) {
		return false;
	}
	const auto permitted = intent->find("permittedIntents");
	if (permitted == intent->end() || !permitted->is_array()) {
		return false;
	}
	return std::any_of(permitted->begin(), permitted->end(), [](const Json& item) {
		if (!item.is_object()) {
			return false;
		}
		const auto type = item.find("intentType");
		return type != item.end() && type->is_string() &&
		       type->get_ref<const std::string&>() == "launchActivity";
	});
}

} // namespace

std::string FallbackTitle(const std::filesystem::path& entry_path, GameEntry::Kind kind) {
	auto path = entry_path;
	if (!path.has_filename() && path.has_relative_path()) {
		path = path.parent_path();
	}
	if (!path.has_filename()) {
		return path.generic_string();
	}
	return kind == GameEntry::Kind::Archive ? path.stem().string() : path.filename().string();
}

GameMetadata ParseParamJson(std::string_view bytes, std::string_view fallback_title) {
	GameMetadata metadata;
	metadata.title_name = std::string(fallback_title);
	if (bytes.size() > kMaxParamJsonBytes) {
		return metadata;
	}

	// No exceptions: a parse error gives a discarded value.
	const auto root = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		return metadata;
	}

	metadata.metadata_available = true;
	auto title                  = GetLocalizedTitleName(root);
	if (!title.empty()) {
		metadata.title_name = std::move(title);
	}
	metadata.title_id     = GetTitleId(root);
	metadata.game_version = GetString(root, "appVersion");
	if (metadata.game_version.empty()) {
		metadata.game_version = GetString(root, "contentVersion");
	}
	metadata.firmware_version =
	    FormatSystemSoftwareVersion(GetString(root, "requiredSystemSoftwareVersion"));
	metadata.pro_supported   = SupportsProMode(root);
	metadata.activity_intent = HasActivityIntent(root);
	return metadata;
}

namespace {

class HostMetadataReader final: public MetadataReader {
public:
	[[nodiscard]] GameMetadata Read(const std::filesystem::path& entry_path,
	                                GameEntry::Kind              kind) const override {
		const auto fallback = FallbackTitle(entry_path, kind);
		const auto bytes    = ReadParamJson(entry_path, kind);
		if (!bytes) {
			GameMetadata metadata;
			metadata.title_name = fallback;
			return metadata;
		}
		return ParseParamJson(*bytes, fallback);
	}
};

} // namespace

std::unique_ptr<MetadataReader> MakeHostMetadataReader() {
	return std::make_unique<HostMetadataReader>();
}

} // namespace kyty::core
