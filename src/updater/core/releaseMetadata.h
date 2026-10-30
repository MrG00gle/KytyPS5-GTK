#pragma once

// Release metadata from the GitHub REST API (task 17.1; design.md "Release metadata (GitHub REST
// subset)"; Requirements 13.5, 13.6, 13.11). ParseRelease is ported from UpdateRelease::Parse of
// the Qt launcher (src/launcher/src/updateRelease.cpp) and keeps its error messages.

#include "updater/core/sha256.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::updater {

// The update package of a release: "<tag_name>-Linux-x86_64.tar.gz" (kAssetSuffix in
// updatePolicy.h) with its published SHA-256 and size.
struct ReleaseAsset {
	std::string name;
	Sha256Bytes sha256 {};
	uint64_t    size = 0;

	bool operator==(const ReleaseAsset&) const = default;
};

struct ReleaseInfo {
	std::string tag; // tag_name; empty when it is missing or not a string
	// published_at in UTC, to the second (a fraction is dropped); nullopt when it is missing or not
	// an RFC 3339 date-time. The design's sys_seconds, optional so that Compare can tell an unknown
	// date from a known one.
	std::optional<std::chrono::sys_seconds> published_at;
	bool                                    draft      = false; // false unless "draft" is true
	bool                                    prerelease = false; // false unless "prerelease" is true
	// The compatible update package: set exactly when error is empty.
	std::optional<ReleaseAsset> asset;
	std::string                 error; // why there is no compatible update package

	bool operator==(const ReleaseInfo&) const = default;
};

inline constexpr std::string_view kInvalidReleaseMessage = "Invalid GitHub release response.";
inline constexpr std::string_view kUnstableReleaseMessage =
    "The release is not a published stable release.";
inline constexpr std::string_view kInvalidAssetMessage =
    "The update package size or SHA-256 checksum is invalid.";
inline constexpr std::string_view kMissingAssetMessage =
    "This release has no update package for your platform.";

// Parses one release object of the GitHub REST API. The document must be a JSON object; otherwise
// error is kInvalidReleaseMessage. The release must have a non-empty string "tag_name", a string
// "published_at" that ParsePublishedAt accepts, and the booleans "draft" and "prerelease", both
// false; otherwise error is kUnstableReleaseMessage. Then the first element of the "assets" array
// that is an object whose "name" is the string <tag_name>-Linux-x86_64.tar.gz decides: its "digest"
// must be a string matching ^sha256:[0-9A-Fa-f]{64}$ and its "size" an integer greater than 0 (else
// kInvalidAssetMessage); without such an element, error is kMissingAssetMessage. The fields that
// could be read are filled in whatever the error. Parsing the same document twice gives equal
// results.
ReleaseInfo ParseRelease(std::string_view json);

// An RFC 3339 date-time, YYYY-MM-DDTHH:MM:SS with an optional fraction and Z or +HH:MM / -HH:MM (T
// and Z also in lower case, RFC 3339 section 5.6), converted to UTC. A day that does not exist in
// its month, an hour above 23, a minute or second above 59 (no leap seconds, as Qt), or an offset
// above 23:59 gives nullopt.
std::optional<std::chrono::sys_seconds> ParsePublishedAt(std::string_view text);

} // namespace kyty::updater
