#include "updater/core/releaseMetadata.h"

#include "updater/core/updatePolicy.h"

#include <cstddef>
#include <nlohmann/json.hpp>

namespace kyty::updater {

namespace {

using Json = nlohmann::json;

int HexDigit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// "sha256:" and 64 hexadecimal digits (Qt: \Asha256:[A-Fa-f0-9]{64}\z).
std::optional<Sha256Bytes> ParseDigest(std::string_view text) {
	constexpr std::string_view kPrefix = "sha256:";
	if (text.size() != kPrefix.size() + 64 || !text.starts_with(kPrefix)) {
		return std::nullopt;
	}
	text.remove_prefix(kPrefix.size());
	Sha256Bytes digest {};
	for (size_t i = 0; i < digest.size(); i++) {
		const int high = HexDigit(text[2 * i]);
		const int low  = HexDigit(text[2 * i + 1]);
		if (high < 0 || low < 0) {
			return std::nullopt;
		}
		digest[i] = static_cast<uint8_t>(high * 16 + low);
	}
	return digest;
}

// A positive JSON integer (nlohmann stores parsed non-negative integers as unsigned).
std::optional<uint64_t> PositiveInteger(const Json& value) {
	if (!value.is_number_unsigned()) {
		return std::nullopt;
	}
	const auto number = value.get<uint64_t>();
	if (number == 0) {
		return std::nullopt;
	}
	return number;
}

// The member key of object when it is a string, else nullptr.
const std::string* StringMember(const Json& object, const char* key) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) {
		return nullptr;
	}
	return &found->get_ref<const std::string&>();
}

// The member key of object when it is a boolean, else nullopt.
std::optional<bool> BooleanMember(const Json& object, const char* key) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_boolean()) {
		return std::nullopt;
	}
	return found->get<bool>();
}

// Reads the fixed-width decimal number at text[position, position + count).
bool ReadDigits(std::string_view text, size_t& position, size_t count, int& value) {
	if (text.size() - position < count) {
		return false;
	}
	value = 0;
	for (size_t i = 0; i < count; i++) {
		const char c = text[position + i];
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	position += count;
	return true;
}

bool ReadChar(std::string_view text, size_t& position, char expected) {
	if (position >= text.size() || text[position] != expected) {
		return false;
	}
	position++;
	return true;
}

} // namespace

std::optional<std::chrono::sys_seconds> ParsePublishedAt(std::string_view text) {
	size_t position = 0;
	int    year     = 0;
	int    month    = 0;
	int    day      = 0;
	int    hour     = 0;
	int    minute   = 0;
	int    second   = 0;
	if (!ReadDigits(text, position, 4, year) || !ReadChar(text, position, '-') ||
	    !ReadDigits(text, position, 2, month) || !ReadChar(text, position, '-') ||
	    !ReadDigits(text, position, 2, day)) {
		return std::nullopt;
	}
	if (!ReadChar(text, position, 'T') && !ReadChar(text, position, 't')) {
		return std::nullopt;
	}
	if (!ReadDigits(text, position, 2, hour) || !ReadChar(text, position, ':') ||
	    !ReadDigits(text, position, 2, minute) || !ReadChar(text, position, ':') ||
	    !ReadDigits(text, position, 2, second)) {
		return std::nullopt;
	}
	if (ReadChar(text, position, '.')) {
		const size_t start = position;
		while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
			position++;
		}
		if (position == start) {
			return std::nullopt;
		}
	}
	int offset_minutes = 0;
	if (!ReadChar(text, position, 'Z') && !ReadChar(text, position, 'z')) {
		const bool negative = ReadChar(text, position, '-');
		if (!negative && !ReadChar(text, position, '+')) {
			return std::nullopt;
		}
		int offset_hour   = 0;
		int offset_minute = 0;
		if (!ReadDigits(text, position, 2, offset_hour) || !ReadChar(text, position, ':') ||
		    !ReadDigits(text, position, 2, offset_minute) || offset_hour > 23 ||
		    offset_minute > 59) {
			return std::nullopt;
		}
		offset_minutes = (offset_hour * 60 + offset_minute) * (negative ? -1 : 1);
	}
	if (position != text.size() || hour > 23 || minute > 59 || second > 59) {
		return std::nullopt;
	}

	const std::chrono::year_month_day date {std::chrono::year {year},
	                                        std::chrono::month {static_cast<unsigned>(month)},
	                                        std::chrono::day {static_cast<unsigned>(day)}};
	if (!date.ok()) {
		return std::nullopt;
	}
	// The local time minus its offset from UTC.
	return std::chrono::sys_days {date} + std::chrono::hours {hour} +
	       std::chrono::minutes {minute - offset_minutes} + std::chrono::seconds {second};
}

ReleaseInfo ParseRelease(std::string_view json) {
	ReleaseInfo info;
	const auto  root = Json::parse(json.begin(), json.end(), nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		info.error = kInvalidReleaseMessage;
		return info;
	}

	if (const auto* tag = StringMember(root, "tag_name")) {
		info.tag = *tag;
	}
	if (const auto* published_at = StringMember(root, "published_at")) {
		info.published_at = ParsePublishedAt(*published_at);
	}
	const auto draft      = BooleanMember(root, "draft");
	const auto prerelease = BooleanMember(root, "prerelease");
	info.draft            = draft.value_or(false);
	info.prerelease       = prerelease.value_or(false);
	if (info.tag.empty() || !info.published_at || !draft || !prerelease || info.draft ||
	    info.prerelease) {
		info.error = kUnstableReleaseMessage;
		return info;
	}

	const std::string asset_name = info.tag + std::string(kAssetSuffix);
	const auto        assets     = root.find("assets");
	if (assets != root.end() && assets->is_array()) {
		for (const auto& asset: *assets) {
			if (!asset.is_object()) {
				continue;
			}
			const auto* name = StringMember(asset, "name");
			if (name == nullptr || *name != asset_name) {
				continue;
			}
			const auto* digest_text = StringMember(asset, "digest");
			const auto  digest = digest_text != nullptr ? ParseDigest(*digest_text) : std::nullopt;
			const auto  size_member = asset.find("size");
			const auto  size =
			    size_member != asset.end() ? PositiveInteger(*size_member) : std::nullopt;
			if (!digest || !size) {
				info.error = kInvalidAssetMessage;
				return info;
			}
			info.asset = ReleaseAsset {asset_name, *digest, *size};
			return info;
		}
	}
	info.error = kMissingAssetMessage;
	return info;
}

} // namespace kyty::updater
