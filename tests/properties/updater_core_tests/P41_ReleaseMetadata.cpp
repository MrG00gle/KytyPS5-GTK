// Feature: linux-gtk-modernization, Property 41: Release metadata parsing
//
// Validates Requirements 13.6 and 13.11. Each case builds a GitHub release document member by
// member (KytyTests::UpdaterGen::ReleaseSpec). Half the cases are published stable releases; the
// others break one release field or several: a tag that is empty, a number, null, or missing
// instead of a non-empty string; a published_at that is null, a number, missing, or a broken
// date-time (no zone, a space, an offset without a colon, an hour of 24, a day the month does not
// have) instead of an RFC 3339 date-time drawn from 1970-2099 and written with any offset from
// -23:59 to +23:59, an optional fraction, and either letter case; draft or prerelease true, a
// string, a number, null, or missing instead of false. The "assets" array holds decoys (other
// platforms, other letter case, the bare tag, names that are not strings, elements that are not
// objects) and the update package at any place or (one case in eight) not at all, its digest valid
// in any letter case or broken, its size from 1 to 2^64-1 or broken, and sometimes a second
// package after it; rarely "assets" is missing or not an array, or the document is not a JSON
// object. ParseRelease must return the package's name, SHA-256, and size exactly when the reference
// model says the release is published, not a draft, not a prerelease, and its first package has a
// "sha256:" digest of 64 hexadecimal digits and an integer size above 0; otherwise no asset and the
// reason the model expects. The tag, published_at (in UTC), draft, and prerelease that could be
// read are reported, and parsing the same document twice gives equal results.

#include "support/property.h"
#include "support/updaterGenerators.h"
#include "updater/core/releaseMetadata.h"
#include "updater/core/updatePolicy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace UpdaterGen = KytyTests::UpdaterGen;
using UpdaterGen::JsonString;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

std::string RandomTag() {
	static const std::vector<std::string> kPieces = {"2026", "-",        "10", "30", "8d525f7",
	                                                 "v",    "1",        ".",  "_",  "rc",
	                                                 " ",    "\xC3\xA9", "/",  "+",  "~"};
	std::string                           tag;
	const int                             count = Choose(1, 8);
	for (int i = 0; i < count; i++) {
		tag += Pick(kPieces);
	}
	return tag;
}

std::string Pad(int value, int width) {
	char text[16];
	std::snprintf(text, sizeof(text), "%0*d", width, value);
	return text;
}

struct PublishedAt {
	std::string                             json;     // the member's raw JSON text
	std::optional<std::chrono::sys_seconds> expected; // the UTC time when the text is valid
	bool                                    present = true;
	const char*                             kind    = "";
};

// A valid date-time: the UTC time first, then its local time at a random offset.
PublishedAt ValidPublishedAt() {
	using namespace std::chrono;
	const auto utc = sys_seconds {
	    seconds {Choose<int64_t>(0, int64_t {4102444800})}}; // 1970-01-01 to 2100-01-01
	const int            offset_minutes = OneIn(3) ? 0 : Choose(-(23 * 60 + 59), 23 * 60 + 60);
	const auto           local          = utc + minutes {offset_minutes};
	const auto           day            = floor<days>(local);
	const year_month_day date {day};
	const hh_mm_ss       time {local - day};

	std::string text = Pad(static_cast<int>(date.year()), 4) + "-" +
	                   Pad(static_cast<int>(static_cast<unsigned>(date.month())), 2) + "-" +
	                   Pad(static_cast<int>(static_cast<unsigned>(date.day())), 2) +
	                   (OneIn(5) ? "t" : "T") + Pad(static_cast<int>(time.hours().count()), 2) +
	                   ":" + Pad(static_cast<int>(time.minutes().count()), 2) + ":" +
	                   Pad(static_cast<int>(time.seconds().count()), 2);
	if (OneIn(3)) {
		text += "." + std::to_string(Choose(0, 1000000));
	}
	if (offset_minutes == 0 && OneIn(2)) {
		text += OneIn(5) ? "z" : "Z";
	} else {
		const int magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
		text += (offset_minutes < 0 || (offset_minutes == 0 && OneIn(2)) ? "-" : "+") +
		        Pad(magnitude / 60, 2) + ":" + Pad(magnitude % 60, 2);
	}
	return PublishedAt {JsonString(text), utc, true, "valid"};
}

PublishedAt RandomPublishedAt(bool broken) {
	if (!broken) {
		return ValidPublishedAt();
	}
	switch (Choose(0, 5)) {
		case 0: return PublishedAt {"null", std::nullopt, true, "null"};
		case 1: return PublishedAt {"1761825600", std::nullopt, true, "a number"};
		case 2: return PublishedAt {"", std::nullopt, false, "missing"};
		default: {
			static const std::vector<std::string> kBroken = {"2026-10-30T12:00:00",
			                                                 "2026-10-30 12:00:00Z",
			                                                 "2026-10-30T12:00:00+0200",
			                                                 "2026-10-30T24:00:00Z",
			                                                 "2026-02-30T12:00:00Z",
			                                                 "2026-10-30",
			                                                 "2026-10-30T12:00Z",
			                                                 "2026-13-01T00:00:00Z",
			                                                 "2026-10-30T12:00:60Z",
			                                                 "2026-10-30T12:00:00+24:00",
			                                                 "2026-10-30T12:00:00.Z",
			                                                 "2026-10-30T12:00:00Z ",
			                                                 "",
			                                                 "2023-02-29T00:00:00+01:00",
			                                                 "2026-10-30T12:00:00UTC"};
			return PublishedAt {JsonString(Pick(kBroken)), std::nullopt, true, "broken"};
		}
	}
}

struct BoolMember {
	std::optional<std::string> json;
	bool                       valid_false = false; // the boolean false
	bool                       is_true     = false; // the boolean true
};

BoolMember RandomBool(bool broken) {
	if (!broken) {
		return BoolMember {"false", true, false};
	}
	switch (Choose(0, 5)) {
		case 0: return BoolMember {"\"false\"", false, false};
		case 1: return BoolMember {"0", false, false};
		case 2: return BoolMember {"null", false, false};
		case 3: return BoolMember {std::nullopt, false, false};
		default: return BoolMember {"true", false, true};
	}
}

struct Package {
	UpdaterGen::AssetSpec      spec;
	bool                       valid = false;
	kyty::updater::Sha256Bytes digest {};
	uint64_t                   size = 0;
};

Package RandomPackage(const std::string& tag) {
	Package package;
	for (auto& byte: package.digest) {
		byte = static_cast<uint8_t>(Choose(0, 256));
	}
	std::string hex = UpdaterGen::DigestHex(package.digest);
	if (OneIn(3)) {
		for (auto& c: hex) {
			if (c >= 'a' && OneIn(2)) {
				c = static_cast<char>(c - 'a' + 'A');
			}
		}
	}
	package.spec.name = JsonString(kyty::updater::AssetNameForTag(tag));

	// Each of the digest and the size is broken in about one case in four.
	bool digest_valid = false;
	switch (Choose(0, 32)) {
		case 0: package.spec.digest = JsonString(hex); break;
		case 1: package.spec.digest = JsonString("sha256:" + hex.substr(1)); break;
		case 2: package.spec.digest = JsonString("sha256:" + hex + "0"); break;
		case 3: package.spec.digest = JsonString("sha256:" + hex.substr(1) + "x"); break;
		case 4: package.spec.digest = JsonString("SHA256:" + hex); break;
		case 5: package.spec.digest = JsonString("sha512:" + hex); break;
		case 6: package.spec.digest = std::nullopt; break;
		case 7: package.spec.digest = "null"; break;
		default:
			package.spec.digest = JsonString("sha256:" + hex);
			digest_valid        = true;
			break;
	}

	bool size_valid = true;
	switch (Choose(0, 28)) {
		case 0: package.spec.size = "0"; break;
		case 1: package.spec.size = "-" + std::to_string(Choose(1, 100000)); break;
		case 2: package.spec.size = std::to_string(Choose(1, 1000)) + ".5"; break;
		case 3: package.spec.size = std::to_string(Choose(1, 1000)) + ".0"; break;
		case 4: package.spec.size = JsonString(std::to_string(Choose(1, 1000))); break;
		case 5: package.spec.size = std::nullopt; break;
		case 6: package.spec.size = "true"; break;
		case 7:
			package.size = std::numeric_limits<uint64_t>::max() - Choose<uint64_t>(0, 1000);
			break;
		default: package.size = Choose<uint64_t>(1, uint64_t {1} << 40); break;
	}
	if (package.size == 0) {
		size_valid = false;
	} else {
		package.spec.size = std::to_string(package.size);
	}
	package.valid = digest_valid && size_valid;
	return package;
}

// An element of "assets" that is never the package of tag.
UpdaterGen::AssetSpec Decoy(const std::string& tag) {
	UpdaterGen::AssetSpec decoy;
	decoy.digest = JsonString("sha256:" + std::string(64, 'e'));
	decoy.size   = "99";
	switch (Choose(0, 9)) {
		case 0: decoy.name = JsonString(tag + "-Linux-aarch64.tar.gz"); break;
		case 1: decoy.name = JsonString(tag + "-linux-x86_64.tar.gz"); break;
		case 2: decoy.name = JsonString(tag); break;
		case 3: decoy.name = JsonString("x" + kyty::updater::AssetNameForTag(tag)); break;
		case 4: decoy.name = JsonString(kyty::updater::AssetNameForTag(tag) + ".sha256"); break;
		case 5: decoy.name = "[" + JsonString(kyty::updater::AssetNameForTag(tag)) + "]"; break;
		case 6: decoy.name = std::nullopt; break;
		case 7:
			decoy.element = Pick(std::vector<std::string> {"null", "1", "\"name\"", "[]"});
			break;
		default: decoy.name = JsonString(tag + "-source.tar.gz"); break;
	}
	return decoy;
}

} // namespace

KYTY_PROPERTY(41, "Release metadata parsing") {
	return rc::check(name, []() {
		UpdaterGen::ReleaseSpec spec;

		// Which release fields the case breaks: none (half the cases), exactly one of the tag,
		// published_at, draft, and prerelease, or (one case in eight) each one in a third of them.
		const int  plan   = Choose(0, 16);
		const auto broken = [plan](int field) {
			return plan == 8 + field || (plan >= 12 && OneIn(3));
		};

		// The tag.
		const std::string tag       = RandomTag();
		bool              tag_valid = false;
		switch (broken(0) ? Choose(0, 4) : 4) {
			case 0: spec.tag_name = "\"\""; break;
			case 1: spec.tag_name = "42"; break;
			case 2: spec.tag_name = "null"; break;
			case 3: spec.tag_name = std::nullopt; break;
			default:
				spec.tag_name = JsonString(tag);
				tag_valid     = true;
				break;
		}
		const auto published = RandomPublishedAt(broken(1));
		if (published.present) {
			spec.published_at = published.json;
		}
		const auto draft      = RandomBool(broken(2));
		const auto prerelease = RandomBool(broken(3));
		spec.draft            = draft.json;
		spec.prerelease       = prerelease.json;

		// The assets: decoys, the package at any place or not at all, maybe a second package.
		std::optional<Package> first_package;
		const int              decoys      = Choose(0, 5);
		const bool             has_package = !OneIn(8);
		const int              position    = Choose(0, decoys + 1);
		for (int i = 0; i <= decoys; i++) {
			if (i == position && has_package) {
				first_package = RandomPackage(tag);
				spec.assets.push_back(first_package->spec);
				if (OneIn(4)) {
					spec.assets.push_back(RandomPackage(tag).spec);
				}
			}
			if (i < decoys) {
				spec.assets.push_back(Decoy(tag));
			}
		}
		bool assets_array = true;
		if (OneIn(25)) {
			spec.has_assets = false;
			assets_array    = false;
		} else if (OneIn(25)) {
			spec.assets_json = Pick(std::vector<std::string> {"{}", "null", "\"assets\"", "7"});
			assets_array     = false;
		}

		std::string document  = UpdaterGen::ReleaseDocument(spec);
		bool        is_object = true;
		if (OneIn(20)) {
			is_object = false;
			document  = OneIn(2) ? document.substr(0, Choose<size_t>(0, document.size()))
			                     : "[" + document + "]";
		}

		// The reference model.
		const bool       stable = is_object && tag_valid && published.expected.has_value() &&
		                          draft.valid_false && prerelease.valid_false;
		std::string_view expected_error;
		if (!is_object) {
			expected_error = kyty::updater::kInvalidReleaseMessage;
		} else if (!stable) {
			expected_error = kyty::updater::kUnstableReleaseMessage;
		} else if (!assets_array || !first_package) {
			expected_error = kyty::updater::kMissingAssetMessage;
		} else if (!first_package->valid) {
			expected_error = kyty::updater::kInvalidAssetMessage;
		}

		const auto info  = kyty::updater::ParseRelease(document);
		const auto again = kyty::updater::ParseRelease(document);
		RC_LOG() << document.substr(0, 600) << "\n-> error '" << info.error << "'\n";
		RC_ASSERT(info == again);
		RC_ASSERT(info.error == expected_error);
		RC_ASSERT(info.asset.has_value() == expected_error.empty());
		if (expected_error.empty()) {
			RC_ASSERT(info.asset->name == tag + "-Linux-x86_64.tar.gz");
			RC_ASSERT(info.asset->sha256 == first_package->digest);
			RC_ASSERT(info.asset->size == first_package->size);
		}
		if (is_object) {
			RC_ASSERT(info.tag == (tag_valid ? tag : std::string()));
			RC_ASSERT(info.published_at == published.expected);
			RC_ASSERT(info.draft == draft.is_true);
			RC_ASSERT(info.prerelease == prerelease.is_true);
		}
		RC_TAG(expected_error.empty() ? std::string("asset") : std::string(expected_error));
	});
}
