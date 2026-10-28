#include "launcher/core/gameMetadata.h"

#include "launcher/core/gameContent.h"
#include "launcher/core/text.h"
#include "launcher/core/version.h"

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
