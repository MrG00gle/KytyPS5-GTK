#pragma once

// Update policy of the fork (task 17.1; design.md "Updater interfaces" and "Updater flow";
// Requirements 13.1, 13.3, 13.5, 13.10, 13.13): the repository, the release URLs and the only URL
// prefixes that the Updater requests, the release comparison, and the update UI mode of a build.

#include "updater/core/releaseMetadata.h"

#include <string>
#include <string_view>

namespace kyty::updater {

inline constexpr std::string_view kRepository = "MrG00gle/KytyPS5-GTK";
inline constexpr std::string_view kReleasesPage =
    "https://github.com/MrG00gle/KytyPS5-GTK/releases";

// The update package of a release is "<tag>" followed by this suffix.
inline constexpr std::string_view kAssetSuffix = "-Linux-x86_64.tar.gz";

// Every release metadata request starts with the first prefix, every asset download with the
// second (Requirement 13.1).
inline constexpr std::string_view kReleaseApiPrefix =
    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/";
inline constexpr std::string_view kReleaseDownloadPrefix =
    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/";

// text with every byte outside the RFC 3986 unreserved set (ASCII letters and digits, "-", ".",
// "_", "~") written as "%" and two upper-case hexadecimal digits, as QUrl::toPercentEncoding does.
std::string PercentEncode(std::string_view text);

// tag + kAssetSuffix.
std::string AssetNameForTag(std::string_view tag);

// <kReleaseApiPrefix>latest: the most recently published release that is neither a draft nor a
// prerelease.
std::string LatestReleaseUrl();
// <kReleaseApiPrefix>tags/<percent-encoded tag>.
std::string ReleaseByTagUrl(std::string_view tag);
// <kReleaseDownloadPrefix><percent-encoded tag>/<percent-encoded asset name>.
std::string AssetDownloadUrl(std::string_view tag, std::string_view asset_name);

// True when url starts with kReleaseApiPrefix or kReleaseDownloadPrefix and the rest is one or
// more "/"-separated non-empty path segments of unreserved characters and %XX escapes, none of
// them a dot segment ("." or "..", also spelled with %2E). So no query, fragment, or path that a
// client normalizes out of the prefix passes. Every URL of the builders above passes.
bool IsAllowedUpdateUrl(std::string_view url);

enum class Relation { Newer, NotNewer, Unknown };

// Whether latest was published after installed (Requirements 13.5, 13.13). Unknown when a tag is
// empty; NotNewer when the tags are equal; else Unknown when a published_at is unknown; else Newer
// exactly when latest.published_at is later (an equal time is NotNewer: "not published after").
Relation Compare(const ReleaseInfo& latest, const ReleaseInfo& installed);

// What the GTK launcher offers (Requirements 13.3, 13.10): Official builds check for and install
// updates, Flatpak builds show no update control and send no request, Other builds only link to
// kReleasesPage.
enum class UpdateUiMode { Official, Flatpak, Other };

// The compile-time classification of a build (buildInfo.h gives the running binary's flags).
struct BuildFlags {
	bool             flatpak  = false; // KYTY_FLATPAK (KYTY_FLATPAK_BUILD=ON)
	bool             official = false; // KYTY_OFFICIAL_BUILD (cmake/KytyBuildInfo.cmake)
	std::string_view release_tag;      // KYTY_RELEASE_TAG
	std::string_view git_hash;         // KYTY_GIT_HASH, "-dirty" when built from a modified tree
};

// Flatpak when flags.flatpak; else Official when flags.official, the release tag is not empty,
// and the git hash does not end in "-dirty" (the Qt launcher's check); else Other.
UpdateUiMode SelectUpdateUiMode(const BuildFlags& flags);

} // namespace kyty::updater
