#include "updater/core/updatePolicy.h"

#include <cstddef>

namespace kyty::updater {

namespace {

bool IsUnreserved(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
	       c == '.' || c == '_' || c == '~';
}

bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// A non-empty path segment of unreserved characters and %XX escapes that is not a dot segment.
bool IsSafeSegment(std::string_view segment) {
	if (segment.empty()) {
		return false;
	}
	// The segment with %2E decoded, which RFC 3986 normalization treats as ".".
	std::string decoded_dots;
	for (size_t i = 0; i < segment.size(); i++) {
		const char c = segment[i];
		if (c == '%') {
			if (segment.size() - i < 3 || !IsHexDigit(segment[i + 1]) ||
			    !IsHexDigit(segment[i + 2])) {
				return false;
			}
			const bool dot =
			    segment[i + 1] == '2' && (segment[i + 2] == 'E' || segment[i + 2] == 'e');
			decoded_dots += dot ? std::string(".") : std::string(segment.substr(i, 3));
			i += 2;
		} else if (IsUnreserved(c)) {
			decoded_dots += c;
		} else {
			return false;
		}
	}
	return decoded_dots != "." && decoded_dots != "..";
}

} // namespace

std::string PercentEncode(std::string_view text) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string           encoded;
	encoded.reserve(text.size());
	for (const char c: text) {
		if (IsUnreserved(c)) {
			encoded += c;
		} else {
			const auto byte = static_cast<unsigned char>(c);
			encoded += '%';
			encoded += kHex[byte >> 4u];
			encoded += kHex[byte & 0xFu];
		}
	}
	return encoded;
}

std::string AssetNameForTag(std::string_view tag) {
	return std::string(tag) + std::string(kAssetSuffix);
}

std::string LatestReleaseUrl() {
	return std::string(kReleaseApiPrefix) + "latest";
}

std::string ReleaseByTagUrl(std::string_view tag) {
	return std::string(kReleaseApiPrefix) + "tags/" + PercentEncode(tag);
}

std::string AssetDownloadUrl(std::string_view tag, std::string_view asset_name) {
	return std::string(kReleaseDownloadPrefix) + PercentEncode(tag) + "/" +
	       PercentEncode(asset_name);
}

bool IsAllowedUpdateUrl(std::string_view url) {
	std::string_view rest;
	if (url.starts_with(kReleaseApiPrefix)) {
		rest = url.substr(kReleaseApiPrefix.size());
	} else if (url.starts_with(kReleaseDownloadPrefix)) {
		rest = url.substr(kReleaseDownloadPrefix.size());
	} else {
		return false;
	}
	for (;;) {
		const size_t slash = rest.find('/');
		if (!IsSafeSegment(rest.substr(0, slash))) {
			return false;
		}
		if (slash == std::string_view::npos) {
			return true;
		}
		rest.remove_prefix(slash + 1);
	}
}

Relation Compare(const ReleaseInfo& latest, const ReleaseInfo& installed) {
	if (latest.tag.empty() || installed.tag.empty()) {
		return Relation::Unknown;
	}
	if (latest.tag == installed.tag) {
		return Relation::NotNewer;
	}
	if (!latest.published_at || !installed.published_at) {
		return Relation::Unknown;
	}
	return *latest.published_at > *installed.published_at ? Relation::Newer : Relation::NotNewer;
}

UpdateUiMode SelectUpdateUiMode(const BuildFlags& flags) {
	if (flags.flatpak) {
		return UpdateUiMode::Flatpak;
	}
	if (flags.official && !flags.release_tag.empty() && !flags.git_hash.ends_with("-dirty")) {
		return UpdateUiMode::Official;
	}
	return UpdateUiMode::Other;
}

} // namespace kyty::updater
