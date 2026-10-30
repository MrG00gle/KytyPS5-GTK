// Example tests of kyty::updater::ParseRelease and ParsePublishedAt
// (src/updater/core/releaseMetadata.h, task 17.1), with release documents that follow the rules of
// the Qt launcher's UpdateRelease::Parse (src/launcher/src/updateRelease.cpp): a valid release
// among other assets, drafts and prereleases, missing or invalid published_at, a missing asset,
// wrong names, bad digests, sizes of 0 or of the wrong type, wrong JSON types, documents that are
// not objects, and the same results for every document parsed twice.

#include "support/updaterGenerators.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::ParsePublishedAt;
using kyty::updater::ParseRelease;
using kyty::updater::ReleaseInfo;
using UpdaterGen::AssetSpec;
using UpdaterGen::JsonString;
using UpdaterGen::ReleaseSpec;

constexpr std::string_view kTag       = "2026-10-30-8d525f7";
constexpr std::string_view kPublished = "2026-10-30T12:00:00Z";
constexpr uint64_t         kSize      = 12345;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ReleaseMetadataTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::chrono::sys_seconds Utc(int year, unsigned month, unsigned day, int hour, int minute,
                             int second) {
	return std::chrono::sys_days {std::chrono::year {year} / std::chrono::month {month} /
	                              std::chrono::day {day}} +
	       std::chrono::hours {hour} + std::chrono::minutes {minute} +
	       std::chrono::seconds {second};
}

kyty::updater::Sha256Bytes Digest() {
	return UpdaterGen::DigestOf("the update package");
}

ReleaseSpec Valid() {
	return UpdaterGen::ValidRelease(kTag, kPublished, Digest(), kSize);
}

// Parses the document twice and checks that both results are equal.
ReleaseInfo Parse(const std::string& document, const std::string& name) {
	const auto first  = ParseRelease(document);
	const auto second = ParseRelease(document);
	Check(first == second, name + ": the same document parses to the same result twice");
	Check(first.asset.has_value() == first.error.empty(),
	      name + ": an asset exactly when there is no error ('" + first.error + "')");
	return first;
}

ReleaseInfo Parse(const ReleaseSpec& spec, const std::string& name) {
	return Parse(UpdaterGen::ReleaseDocument(spec), name);
}

void CheckError(const ReleaseInfo& info, std::string_view expected, const std::string& name) {
	Check(!info.asset.has_value(), name + ": no asset");
	Check(info.error == expected,
	      name + ": error '" + info.error + "', expected '" + std::string(expected) + "'");
}

void TestValidRelease() {
	const auto info = Parse(Valid(), "valid");
	Check(info.error.empty(), "valid: no error, got '" + info.error + "'");
	Check(info.tag == kTag, "valid: tag");
	Check(info.published_at == Utc(2026, 10, 30, 12, 0, 0), "valid: published_at");
	Check(!info.draft && !info.prerelease, "valid: not a draft or prerelease");
	Check(info.asset.has_value() && info.asset->name == std::string(kTag) + "-Linux-x86_64.tar.gz",
	      "valid: asset name");
	Check(info.asset->sha256 == Digest(), "valid: digest");
	Check(info.asset->size == kSize, "valid: size");

	// Upper-case digits, the largest size, and other platforms' packages around it.
	auto spec                 = Valid();
	spec.assets.back().digest = JsonString("sha256:" + UpdaterGen::DigestHex(Digest(), true));
	spec.assets.back().size   = "18446744073709551615";
	AssetSpec other;
	other.name   = JsonString(std::string(kTag) + "-Linux-aarch64.tar.gz");
	other.digest = JsonString("sha256:" + std::string(64, 'a'));
	other.size   = "1";
	spec.assets.insert(spec.assets.begin(), other);
	spec.assets.push_back(other);
	const auto upper = Parse(spec, "upper-case digest");
	Check(upper.asset && upper.asset->sha256 == Digest() &&
	          upper.asset->size == 18446744073709551615ULL,
	      "upper-case digest and the largest size");

	// Elements that are not objects are skipped.
	spec = Valid();
	for (const char* element: {"null", "\"text\"", "[]", "42", "true"}) {
		AssetSpec value;
		value.element = element;
		spec.assets.insert(spec.assets.begin(), value);
	}
	Check(Parse(spec, "non-object elements").asset.has_value(), "non-object elements are skipped");

	// The tag is used as it is: any non-empty string.
	for (const std::string tag: {"v1.0", "release candidate", "\xC3\xA9t\xC3\xA9", "a/b"}) {
		const auto info_for_tag =
		    Parse(UpdaterGen::ValidRelease(tag, kPublished, Digest(), 1), "tag " + tag);
		Check(info_for_tag.asset && info_for_tag.asset->name == tag + "-Linux-x86_64.tar.gz",
		      "tag '" + tag + "' gives its asset");
	}
}

void TestUnstableReleases() {
	struct Case {
		std::string                name;
		std::optional<std::string> ReleaseSpec::* member;
		std::optional<std::string>                value;
	};
	const std::vector<Case> cases = {
	    {"draft", &ReleaseSpec::draft, "true"},
	    {"prerelease", &ReleaseSpec::prerelease, "true"},
	    {"draft missing", &ReleaseSpec::draft, std::nullopt},
	    {"prerelease missing", &ReleaseSpec::prerelease, std::nullopt},
	    {"draft as a string", &ReleaseSpec::draft, "\"false\""},
	    {"draft as a number", &ReleaseSpec::draft, "0"},
	    {"draft null", &ReleaseSpec::draft, "null"},
	    {"prerelease as a string", &ReleaseSpec::prerelease, "\"false\""},
	    {"tag missing", &ReleaseSpec::tag_name, std::nullopt},
	    {"tag empty", &ReleaseSpec::tag_name, "\"\""},
	    {"tag a number", &ReleaseSpec::tag_name, "5"},
	    {"tag null", &ReleaseSpec::tag_name, "null"},
	    {"published_at missing", &ReleaseSpec::published_at, std::nullopt},
	    {"published_at null (a draft)", &ReleaseSpec::published_at, "null"},
	    {"published_at a number", &ReleaseSpec::published_at, "1761825600"},
	    {"published_at without a zone", &ReleaseSpec::published_at, "\"2026-10-30T12:00:00\""},
	    {"published_at a date", &ReleaseSpec::published_at, "\"2026-10-30\""},
	    {"published_at February 30", &ReleaseSpec::published_at, "\"2026-02-30T12:00:00Z\""},
	};
	for (const auto& test: cases) {
		auto spec         = Valid();
		spec.*test.member = test.value;
		const auto info   = Parse(spec, test.name);
		CheckError(info, kyty::updater::kUnstableReleaseMessage, test.name);
	}

	// The fields that could be read are kept.
	auto spec       = Valid();
	spec.draft      = "true";
	spec.prerelease = "true";
	const auto info = Parse(spec, "draft prerelease");
	Check(info.tag == kTag && info.published_at == Utc(2026, 10, 30, 12, 0, 0) && info.draft &&
	          info.prerelease,
	      "a draft prerelease keeps its fields");
}

void TestPublishedAt() {
	const auto noon = Utc(2026, 10, 30, 12, 0, 0);
	const std::vector<std::pair<std::string, std::chrono::sys_seconds>> valid = {
	    {"2026-10-30T12:00:00Z", noon},
	    {"2026-10-30T12:00:00.5Z", noon},
	    {"2026-10-30T12:00:00.123456789Z", noon},
	    {"2026-10-30t12:00:00z", noon},
	    {"2026-10-30T12:00:00+00:00", noon},
	    {"2026-10-30T12:00:00-00:00", noon},
	    {"2026-10-30T14:30:00+02:30", noon},
	    {"2026-10-30T00:00:00-12:00", noon},
	    {"2026-10-31T11:59:00+23:59", noon},
	    {"2026-12-31T23:30:00-01:00", Utc(2027, 1, 1, 0, 30, 0)},
	    {"2027-01-01T00:30:00+01:00", Utc(2026, 12, 31, 23, 30, 0)},
	    {"2024-02-29T23:59:59Z", Utc(2024, 2, 29, 23, 59, 59)},
	    {"2000-02-29T00:00:00Z", Utc(2000, 2, 29, 0, 0, 0)},
	    {"1970-01-01T00:00:00Z", Utc(1970, 1, 1, 0, 0, 0)},
	    {"0000-01-01T00:00:00Z", Utc(0, 1, 1, 0, 0, 0)},
	    {"9999-12-31T23:59:59Z", Utc(9999, 12, 31, 23, 59, 59)},
	};
	for (const auto& [text, expected]: valid) {
		const auto parsed = ParsePublishedAt(text);
		Check(parsed == expected, "'" + text + "' is a valid date-time with the expected value");
	}

	const std::vector<std::string> invalid = {
	    "",
	    "2026-10-30",
	    "2026-10-30T12:00:00",
	    "2026-10-30T12:00Z",
	    "2026-10-30 12:00:00Z",
	    "2026-10-30T12:00:00 Z",
	    " 2026-10-30T12:00:00Z",
	    "2026-10-30T12:00:00Z ",
	    "2026-10-30T12:00:00Zx",
	    "2026-10-30T12:00:00.Z",
	    "2026-10-30T12:00:00,5Z",
	    "2026-10-30T12:00:00+0200",
	    "2026-10-30T12:00:00+02",
	    "2026-10-30T12:00:00+24:00",
	    "2026-10-30T12:00:00+02:60",
	    "2026-10-30T12:00:00UTC",
	    "2026-10-30T24:00:00Z",
	    "2026-10-30T12:60:00Z",
	    "2026-10-30T12:00:60Z",
	    "2026-13-01T12:00:00Z",
	    "2026-00-10T12:00:00Z",
	    "2026-10-00T12:00:00Z",
	    "2026-04-31T12:00:00Z",
	    "2023-02-29T12:00:00Z",
	    "1900-02-29T12:00:00Z",
	    "26-10-30T12:00:00Z",
	    "+2026-10-30T12:00:00Z",
	    "2026/10/30T12:00:00Z",
	    "2026-1-30T12:00:00Z",
	    "2026-10-30T1:00:00Z",
	    "2026-10-30T12:00:00.123+",
	    "\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x92\xEF\xBC\x96-10-30T12:00:00Z", // fullwidth digits
	};
	for (const auto& text: invalid) {
		Check(!ParsePublishedAt(text).has_value(), "'" + text + "' is not an RFC 3339 date-time");
	}
}

void TestAssets() {
	// No asset with the package name.
	{
		auto spec       = Valid();
		spec.has_assets = false;
		CheckError(Parse(spec, "no assets member"), kyty::updater::kMissingAssetMessage,
		           "no assets member");
	}
	for (const std::string assets: {"{}", "\"assets\"", "null", "[]", "[null, 1]"}) {
		auto spec        = Valid();
		spec.assets_json = assets;
		CheckError(Parse(spec, "assets " + assets), kyty::updater::kMissingAssetMessage,
		           "assets " + assets);
	}
	const std::string              tag(kTag);
	const std::vector<std::string> names = {
	    JsonString(tag + "-Linux-aarch64.tar.gz"),
	    JsonString(tag + "-linux-x86_64.tar.gz"),
	    JsonString(tag + "-Linux-x86_64.tar.gz "),
	    JsonString(" " + tag + "-Linux-x86_64.tar.gz"),
	    JsonString(tag + "-Linux-x86_64.zip"),
	    JsonString("-Linux-x86_64.tar.gz"),
	    JsonString(tag),
	    JsonString("x" + tag + "-Linux-x86_64.tar.gz"),
	    "42",
	    "null",
	    "[" + JsonString(tag + "-Linux-x86_64.tar.gz") + "]",
	};
	for (const auto& name: names) {
		auto spec               = Valid();
		spec.assets.back().name = name;
		CheckError(Parse(spec, "asset name " + name), kyty::updater::kMissingAssetMessage,
		           "asset name " + name);
	}
	{
		auto spec = Valid();
		spec.assets.back().name.reset();
		CheckError(Parse(spec, "asset without a name"), kyty::updater::kMissingAssetMessage,
		           "asset without a name");
	}

	// The package is there, but its digest or size is unusable.
	const std::string                             hex         = UpdaterGen::DigestHex(Digest());
	const std::vector<std::optional<std::string>> bad_digests = {
	    std::nullopt,
	    "null",
	    "42",
	    JsonString(hex),
	    JsonString("sha1:" + hex),
	    JsonString("SHA256:" + hex),
	    JsonString("sha256:" + hex.substr(1)),
	    JsonString("sha256:" + hex + "0"),
	    JsonString("sha256:" + hex.substr(1) + "g"),
	    JsonString(" sha256:" + hex),
	    JsonString("sha256:" + hex + " "),
	    JsonString("sha256: " + hex.substr(1)),
	    JsonString("sha256:"),
	};
	for (const auto& digest: bad_digests) {
		auto spec                 = Valid();
		spec.assets.back().digest = digest;
		const std::string name    = "digest " + digest.value_or("missing");
		CheckError(Parse(spec, name), kyty::updater::kInvalidAssetMessage, name);
	}
	const std::vector<std::optional<std::string>> bad_sizes = {
	    std::nullopt, "0", "-1", "-12345", "1.5", "5.0", "1e3", "\"12\"", "null", "true", "[]"};
	for (const auto& size: bad_sizes) {
		auto spec               = Valid();
		spec.assets.back().size = size;
		const std::string name  = "size " + size.value_or("missing");
		CheckError(Parse(spec, name), kyty::updater::kInvalidAssetMessage, name);
	}

	// The first element with the package name decides.
	{
		auto spec = Valid();
		auto bad  = spec.assets.back();
		bad.size  = "0";
		spec.assets.insert(spec.assets.begin(), bad);
		CheckError(Parse(spec, "an invalid package before a valid one"),
		           kyty::updater::kInvalidAssetMessage, "an invalid package before a valid one");
	}
	{
		auto spec  = Valid();
		auto later = UpdaterGen::ValidAsset(kTag, UpdaterGen::DigestOf("other"), 7);
		spec.assets.push_back(later);
		const auto info = Parse(spec, "a valid package before another");
		Check(info.asset && info.asset->sha256 == Digest() && info.asset->size == kSize,
		      "the first valid package is used");
	}
}

void TestInvalidDocuments() {
	const std::vector<std::string> documents = {
	    "",
	    "null",
	    "[]",
	    "[{\"tag_name\":\"v1\"}]",
	    "\"text\"",
	    "42",
	    "{",
	    "{} {}",
	    "{\"tag_name\": \"v1\",}",
	    "{\"tag_name\": \"\xFF\"}",
	    "<html>rate limited</html>",
	};
	for (const auto& document: documents) {
		const auto info = Parse(document, "document '" + document + "'");
		CheckError(info, kyty::updater::kInvalidReleaseMessage, "document '" + document + "'");
		Check(info.tag.empty() && !info.published_at, "document '" + document + "': no fields");
	}

	// A valid document with whitespace around it and members in another order.
	const std::string document =
	    "\n { \"assets\": [" +
	    UpdaterGen::AssetText(UpdaterGen::ValidAsset(kTag, Digest(), kSize)) +
	    "], \"prerelease\": false, \"draft\": false, \"published_at\": " + JsonString(kPublished) +
	    ", \"tag_name\": " + JsonString(kTag) + " } \n";
	Check(Parse(document, "reordered").asset.has_value(),
	      "member order and whitespace do not matter");
}

} // namespace

int main() {
	TestValidRelease();
	TestUnstableReleases();
	TestPublishedAt();
	TestAssets();
	TestInvalidDocuments();
	std::printf("ReleaseMetadataTests: all tests passed\n");
	return 0;
}
