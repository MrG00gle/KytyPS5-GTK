// Example tests of the update policy (src/updater/core/updatePolicy.h, task 17.1): the repository
// and releases page constants, the URL builders and their percent-encoding of every byte,
// IsAllowedUpdateUrl, Compare, and SelectUpdateUiMode including the "-dirty" check.

#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kyty::updater::BuildFlags;
using kyty::updater::Compare;
using kyty::updater::IsAllowedUpdateUrl;
using kyty::updater::Relation;
using kyty::updater::ReleaseInfo;
using kyty::updater::SelectUpdateUiMode;
using kyty::updater::UpdateUiMode;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "UpdatePolicyTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void TestConstants() {
	Check(kyty::updater::kRepository == "MrG00gle/KytyPS5-GTK", "kRepository is the fork");
	Check(kyty::updater::kReleasesPage == "https://github.com/MrG00gle/KytyPS5-GTK/releases",
	      "kReleasesPage is the fork's releases page");
	Check(kyty::updater::kAssetSuffix == "-Linux-x86_64.tar.gz", "kAssetSuffix");
	Check(kyty::updater::AssetNameForTag("v1") == "v1-Linux-x86_64.tar.gz", "AssetNameForTag");
}

void TestUrls() {
	Check(kyty::updater::LatestReleaseUrl() ==
	          "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest",
	      "LatestReleaseUrl");
	Check(kyty::updater::ReleaseByTagUrl("2026-10-30-8d525f7") ==
	          "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/tags/2026-10-30-8d525f7",
	      "ReleaseByTagUrl");
	Check(kyty::updater::AssetDownloadUrl("v1.0", "v1.0-Linux-x86_64.tar.gz") ==
	          "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1.0/"
	          "v1.0-Linux-x86_64.tar.gz",
	      "AssetDownloadUrl");
	Check(
	    kyty::updater::ReleaseByTagUrl("a b/c?d#e%f") ==
	        "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/tags/a%20b%2Fc%3Fd%23e%25f",
	    "ReleaseByTagUrl encodes reserved characters");
	Check(kyty::updater::AssetDownloadUrl("..", "../x") ==
	          "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/../..%2Fx",
	      "AssetDownloadUrl keeps dots and encodes slashes");

	// Every byte: unreserved ones stay, every other one is %XX with upper-case digits.
	for (int value = 0; value < 256; value++) {
		const char        c          = static_cast<char>(value);
		const std::string encoded    = kyty::updater::PercentEncode(std::string_view(&c, 1));
		const bool        unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                               (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
		                               c == '~';
		char              expected[4];
		std::snprintf(expected, sizeof(expected), "%%%02X", value);
		Check(unreserved ? encoded == std::string(1, c) : encoded == expected,
		      "PercentEncode of byte " + std::to_string(value) + " gives '" + encoded + "'");
	}
	Check(kyty::updater::PercentEncode("\xC3\xA9t\xC3\xA9") == "%C3%A9t%C3%A9", "UTF-8 bytes");
	Check(kyty::updater::PercentEncode("") == "", "empty");
}

void TestAllowedUrls() {
	const std::vector<std::string> allowed = {
	    kyty::updater::LatestReleaseUrl(),
	    kyty::updater::ReleaseByTagUrl("2026-10-30-8d525f7"),
	    kyty::updater::ReleaseByTagUrl("a b/c?d#e%f"),
	    kyty::updater::ReleaseByTagUrl("..."),
	    kyty::updater::AssetDownloadUrl("v1", "v1-Linux-x86_64.tar.gz"),
	    kyty::updater::AssetDownloadUrl("\xC3\xA9t\xC3\xA9", "x"),
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/1",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/a%2e%2e.tar.gz",
	};
	for (const auto& url: allowed) {
		Check(IsAllowedUpdateUrl(url), "allowed: " + url);
	}
	const std::vector<std::string> refused = {
	    "",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/",
	    "https://api.github.com/repos/KytyPS5/KytyPS5/releases/latest",
	    "https://github.com/KytyPS5/KytyPS5/releases/download/v1/v1-Linux-x86_64.tar.gz",
	    "https://api.github.com/repos/mrg00gle/kytyps5-gtk/releases/latest",
	    "http://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest",
	    "HTTPS://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releasesX/latest",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/tag/v1",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases",
	    "https://api.github.com.evil.example/repos/MrG00gle/KytyPS5-GTK/releases/latest",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest?per_page=1",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest#x",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest/",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases//latest",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/../../KytyPS5/KytyPS5/releases/"
	    "latest",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/tags/..",
	    "https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/.",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/%2e%2E/x",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/.%2E/x",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/%2e/x",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/a b",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/a%2",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/a%zz",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/@evil",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/a\\b",
	    "https://github.com/MrG00gle/KytyPS5-GTK/releases/download/v1/\xC3\xA9",
	    " https://api.github.com/repos/MrG00gle/KytyPS5-GTK/releases/latest",
	};
	for (const auto& url: refused) {
		Check(!IsAllowedUpdateUrl(url), "refused: " + url);
	}
	Check(
	    !IsAllowedUpdateUrl(std::string(kyty::updater::kReleaseApiPrefix) + std::string("a\0b", 3)),
	    "refused: a NUL byte");
}

ReleaseInfo Release(std::string tag, std::optional<std::chrono::sys_seconds> published_at) {
	ReleaseInfo info;
	info.tag          = std::move(tag);
	info.published_at = published_at;
	return info;
}

void TestCompare() {
	const auto day = [](int d) {
		return std::chrono::sys_seconds {
		    std::chrono::sys_days {std::chrono::year {2026} / std::chrono::October / d}};
	};
	struct Case {
		ReleaseInfo latest;
		ReleaseInfo installed;
		Relation    expected;
		const char* name;
	};
	const std::vector<Case> cases = {
	    {Release("v2", day(2)), Release("v1", day(1)), Relation::Newer, "later"},
	    {Release("v2", day(2) + std::chrono::seconds {1}), Release("v1", day(2)), Relation::Newer,
	     "one second later"},
	    {Release("v2", day(1)), Release("v1", day(2)), Relation::NotNewer, "earlier"},
	    {Release("v2", day(1)), Release("v1", day(1)), Relation::NotNewer, "at the same time"},
	    {Release("v1", day(2)), Release("v1", day(1)), Relation::NotNewer, "the same tag"},
	    {Release("v1", std::nullopt), Release("v1", std::nullopt), Relation::NotNewer,
	     "the same tag without dates"},
	    {Release("v2", std::nullopt), Release("v1", day(1)), Relation::Unknown,
	     "latest without a date"},
	    {Release("v2", day(2)), Release("v1", std::nullopt), Relation::Unknown,
	     "installed without a date"},
	    {Release("", day(2)), Release("v1", day(1)), Relation::Unknown, "latest without a tag"},
	    {Release("v2", day(2)), Release("", day(1)), Relation::Unknown, "installed without a tag"},
	    {Release("", day(2)), Release("", day(1)), Relation::Unknown, "no tags"},
	};
	for (const auto& test: cases) {
		Check(Compare(test.latest, test.installed) == test.expected,
		      std::string("Compare: ") + test.name);
	}

	// Compare looks only at tags and dates: errors and assets do not matter.
	auto latest  = Release("v2", day(2));
	latest.draft = true;
	latest.error = "The release is not a published stable release.";
	Check(Compare(latest, Release("v1", day(1))) == Relation::Newer, "Compare ignores the error");
}

void TestUiMode() {
	struct Case {
		BuildFlags   flags;
		UpdateUiMode expected;
		const char*  name;
	};
	const std::vector<Case> cases = {
	    {{false, true, "2026-10-30-8d525f7", "8d525f7"}, UpdateUiMode::Official, "official"},
	    {{false, true, "v1", "unknown"}, UpdateUiMode::Official, "official without git"},
	    {{false, true, "v1", "8d525f7-dirty"}, UpdateUiMode::Other, "official but dirty"},
	    {{false, true, "", "8d525f7"}, UpdateUiMode::Other, "official without a tag"},
	    {{false, false, "v1", "8d525f7"}, UpdateUiMode::Other, "not official"},
	    {{false, false, "", "8d525f7-dirty"}, UpdateUiMode::Other, "a source build"},
	    {{true, false, "", "8d525f7"}, UpdateUiMode::Flatpak, "Flatpak"},
	    {{true, true, "v1", "8d525f7"}, UpdateUiMode::Flatpak, "Flatpak wins over official"},
	    {{true, false, "v1", "8d525f7-dirty"}, UpdateUiMode::Flatpak, "a dirty Flatpak"},
	    {{false, true, "v1", "dirty"}, UpdateUiMode::Official, "'dirty' without a dash"},
	    {{false, true, "v1", "8d525f7-dirty "},
	     UpdateUiMode::Official,
	     "only a final -dirty counts"},
	};
	for (const auto& test: cases) {
		Check(SelectUpdateUiMode(test.flags) == test.expected,
		      std::string("SelectUpdateUiMode: ") + test.name);
	}
}

} // namespace

int main() {
	TestConstants();
	TestUrls();
	TestAllowedUrls();
	TestCompare();
	TestUiMode();
	std::printf("UpdatePolicyTests: all tests passed\n");
	return 0;
}
