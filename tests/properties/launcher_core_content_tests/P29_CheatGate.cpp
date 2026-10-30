// Feature: linux-gtk-modernization, Property 29: Cheat availability gate
//
// Validates Requirement 11.4. Each case builds a string from Unicode whitespace (and look-alikes
// that are not White_Space: U+200B, U+FEFF), a "PPSA" prefix in any letter case or a near miss
// (another letter, a fullwidth P, U+017F, whose Unicode capital is S), 3-7 digits mixing ASCII,
// fullwidth, and Arabic-Indic digits, and an optional suffix, or, in one case in eight, arbitrary
// bytes. IsCheatTitleId(s) must be true exactly when TrimWhitespace(s) with its ASCII letters
// upper-cased (NormalizeTitleId, plan D18: only ASCII letters, so U+017F stays) is "PPSA" followed
// by five ASCII digits; CheatFilePath must name <data>/_Patches/<that ID>.json exactly then, and be
// empty otherwise.

#include "launcher/core/cheats.h"
#include "launcher/core/text.h"
#include "support/property.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
	return values[Choose<std::size_t>(0, values.size())];
}

std::string Spaces() {
	static const std::vector<std::string> kSpaces = {" ", "\t", "\n", "\r", "\v", "\f", "\xC2\x85",
	                                                 "\xC2\xA0", "\xE3\x80\x80", "\xE2\x80\x80",
	                                                 "\xE2\x80\xA8", "\xE2\x80\xAF",
	                                                 // Not White_Space: they stay after trimming.
	                                                 "\xE2\x80\x8B", "\xEF\xBB\xBF"};
	std::string                           text;
	const int                             count = OneIn(2) ? 0 : Choose(1, 4);
	for (int i = 0; i < count; i++) {
		text += Pick(kSpaces);
	}
	return text;
}

std::string Prefix() {
	if (OneIn(5)) {
		// U+017F (long s) upper-cases to S in Unicode, but NormalizeTitleId keeps it.
		static const std::vector<std::string> kNearMisses = {
		    "PPS", "PPSB", "CUSA", "PPSAA", "\xEF\xBC\xB0PSA", "PP\u017FA", "", "PPSA "};
		return Pick(kNearMisses);
	}
	std::string prefix = "PPSA";
	for (auto& c: prefix) {
		if (OneIn(2)) {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return prefix;
}

std::string Digits() {
	static const std::vector<std::string> kOddDigits = {
	    "\xEF\xBC\x91", "\xD9\xA3", "a", "+", "-", " ", std::string(1, '\0')};
	std::string digits;
	const int   count = OneIn(3) ? Choose(3, 8) : 5;
	for (int i = 0; i < count; i++) {
		digits +=
		    OneIn(12) ? Pick(kOddDigits) : std::string(1, static_cast<char>('0' + Choose(0, 10)));
	}
	return digits;
}

// The property's own rule.
bool IsReferenceCheatId(std::string_view text) {
	std::string id(kyty::core::TrimWhitespace(text));
	for (auto& c: id) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	if (id.size() != 9 || !id.starts_with("PPSA")) {
		return false;
	}
	for (std::size_t i = 4; i < 9; i++) {
		if (id[i] < '0' || id[i] > '9') {
			return false;
		}
	}
	return true;
}

} // namespace

KYTY_PROPERTY(29, "Cheat availability gate") {
	return rc::check(name, []() {
		std::string text;
		if (OneIn(8)) {
			text = *rc::gen::arbitrary<std::string>();
		} else {
			text = Spaces() + Prefix() + Digits() + (OneIn(8) ? "_00" : "") + Spaces();
		}
		const bool expected = IsReferenceCheatId(text);
		RC_ASSERT(kyty::core::IsCheatTitleId(text) == expected);

		const std::filesystem::path data = "/data/kyty";
		const auto                  path = kyty::core::CheatFilePath(data, text);
		if (expected) {
			std::string id(kyty::core::TrimWhitespace(text));
			for (auto& c: id) {
				if (c >= 'a' && c <= 'z') {
					c = static_cast<char>(c - 'a' + 'A');
				}
			}
			RC_ASSERT(path == data / "_Patches" / (id + ".json"));
		} else {
			RC_ASSERT(path.empty());
		}
		RC_TAG(expected ? "cheat title ID" : "no cheats");
	});
}
