// Example tests of the Launcher_Core text helpers (src/launcher/core/text.h, task 10.4):
// Unicode whitespace trimming, non-ASCII case folding, and invalid UTF-8.

#include "launcher/core/text.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kyty::core::CompareCaseInsensitive;
using kyty::core::ContainsCaseInsensitive;
using kyty::core::SanitizeUtf8;
using kyty::core::TrimWhitespace;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "TextTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Hex(std::string_view bytes) {
	std::string text;
	for (const char c: bytes) {
		char buffer[4];
		std::snprintf(buffer, sizeof(buffer), "%02X", static_cast<unsigned char>(c));
		if (!text.empty()) {
			text += ' ';
		}
		text += buffer;
	}
	return "[" + text + "]";
}

std::string Utf8(char32_t c) {
	std::string text;
	if (c < 0x80) {
		text += static_cast<char>(c);
	} else if (c < 0x800) {
		text += static_cast<char>(0xC0 | (c >> 6));
		text += static_cast<char>(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		text += static_cast<char>(0xE0 | (c >> 12));
		text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
		text += static_cast<char>(0x80 | (c & 0x3F));
	} else {
		text += static_cast<char>(0xF0 | (c >> 18));
		text += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
		text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
		text += static_cast<char>(0x80 | (c & 0x3F));
	}
	return text;
}

const std::string kReplacement = "\xEF\xBF\xBD";

std::string Replacements(int count) {
	std::string text;
	for (int i = 0; i < count; i++) {
		text += kReplacement;
	}
	return text;
}

void CheckTrim(std::string_view input, std::string_view expected, const std::string& name) {
	const auto result = TrimWhitespace(input);
	Check(result == expected, name + ": TrimWhitespace(" + Hex(input) + ") is " + Hex(result) +
	                              ", expected " + Hex(expected));
	Check(result.empty() || (result.data() >= input.data() &&
	                         result.data() + result.size() <= input.data() + input.size()),
	      name + ": the result is a view into the input");
}

void TestTrimEachWhiteSpaceCodePoint() {
	std::vector<char32_t> white_space = {0x0085, 0x00A0, 0x1680, 0x2028, 0x2029,
	                                     0x202F, 0x205F, 0x3000, 0x0020};
	for (char32_t c = 0x09; c <= 0x0D; c++) {
		white_space.push_back(c);
	}
	for (char32_t c = 0x2000; c <= 0x200A; c++) {
		white_space.push_back(c);
	}
	Check(white_space.size() == 25, "the Unicode White_Space property has 25 code points");
	for (const char32_t c: white_space) {
		const auto ws   = Utf8(c);
		const auto name = "U+" + Hex(ws);
		CheckTrim(ws + "a b" + ws, "a b", name + " at both ends");
		CheckTrim(ws + ws + "x", "x", name + " twice at the start");
		CheckTrim("x" + ws + ws, "x", name + " twice at the end");
		CheckTrim(ws, "", name + " alone");
		CheckTrim("a" + ws + "b", "a" + ws + "b", name + " inside is kept");
	}
	std::string all;
	for (const char32_t c: white_space) {
		all += Utf8(c);
	}
	CheckTrim(all + "\xC3\xA9" + all, "\xC3\xA9", "every White_Space code point at both ends");
	CheckTrim(all, "", "only White_Space");
	CheckTrim("", "", "empty text");
}

void TestTrimLookAlikes() {
	// Not White_Space: zero width space, Mongolian vowel separator (not since Unicode 6.3), zero
	// width no-break space (BOM), word joiner, and U+0084 next to U+0085.
	for (const char32_t c: {0x200B, 0x180E, 0xFEFF, 0x2060, 0x0084, 0x001C, 0x200C}) {
		const auto text = Utf8(c);
		CheckTrim(" " + text + " ", text, "look-alike " + Hex(text) + " is kept");
	}
	// Bytes of whitespace code points that are not valid UTF-8 on their own.
	CheckTrim("\x85", "\x85", "a lone 0x85 byte");
	CheckTrim("\xA0x\xA0", "\xA0x\xA0", "lone 0xA0 bytes");
	CheckTrim(" \xC2 ", "\xC2", "a truncated sequence between spaces");
	CheckTrim("\xE2\x80", "\xE2\x80", "a truncated U+2028 at the end");
	CheckTrim("x\xE2\x80", "x\xE2\x80", "a truncated U+2028 after text");
	CheckTrim("\x80\xA8", "\x80\xA8", "the continuation bytes of U+2028");
	CheckTrim("\xC0\xA0", "\xC0\xA0", "an overlong U+0020");
	CheckTrim("\xE0\x80\xA0", "\xE0\x80\xA0", "a three-byte overlong U+0020");
	CheckTrim("\xC2\xC2\x85", "\xC2", "U+0085 after an invalid byte");
	CheckTrim("\xF0\x9F\x98\x80 ", "\xF0\x9F\x98\x80", "a four-byte code point before a space");
	CheckTrim(" \xE0\xA0\x85", "\xE0\xA0\x85", "U+0805 ends in the byte 0x85");
}

void TestContains() {
	Check(ContainsCaseInsensitive("Hello World", "WORLD"), "ASCII case folding");
	Check(ContainsCaseInsensitive("Hello World", "lo w"), "a match across words");
	Check(!ContainsCaseInsensitive("Hello World", "worlds"), "a longer needle does not match");
	Check(ContainsCaseInsensitive("\xC3\x89lan", "\xC3\xA9LAN"), "U+00C9 matches U+00E9");
	Check(ContainsCaseInsensitive("Caf\xC3\xA9", "CAF\xC3\x89"), "U+00E9 matches U+00C9");
	Check(ContainsCaseInsensitive("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91",
	                              "\xCF\x83\xCE\xBF\xCF\x86"),
	      "Greek capitals match small letters");
	Check(ContainsCaseInsensitive("\xD0\x9C\xD0\x98\xD0\xA0", "\xD0\xBC\xD0\xB8\xD1\x80"),
	      "Cyrillic capitals match small letters");
	Check(!ContainsCaseInsensitive("\xC3\x89lan", "elan"), "U+00C9 does not match e");
	// Code points of different UTF-8 lengths, also in the middle of the haystack.
	Check(ContainsCaseInsensitive("Skip it", "S\xE2\x84\xAAIP"), "U+212A KELVIN SIGN matches k");
	Check(ContainsCaseInsensitive("o\xE2\x84\xAAo", "K"), "K matches U+212A");
	Check(ContainsCaseInsensitive("Ma\xC3\x9Fstab", "MA\xE1\xBA\x9ESTAB"), "U+1E9E matches U+00DF");
	// Foldings with several code points match only as a whole code point (Requirement 7.14: the
	// result for needle + x is a subset of the result for needle).
	Check(!ContainsCaseInsensitive("Ma\xC3\x9Fstab", "massstab"), "sss does not find U+00DF s");
	Check(!ContainsCaseInsensitive("Ma\xC3\x9F", "mas"), "s does not find U+00DF");
	Check(ContainsCaseInsensitive("Ma\xC3\x9F", "ma"), "the text before U+00DF");
	Check(!ContainsCaseInsensitive("massstab", "ma\xC3\x9F"), "U+00DF does not find ss");
	Check(!ContainsCaseInsensitive("\xEF\xAC\x81le", "file"), "fi does not find U+FB01");
	Check(ContainsCaseInsensitive("\xEF\xAC\x81le", "\xEF\xAC\x81LE"), "U+FB01 finds itself");
	// Ill-formed bytes match only themselves.
	Check(ContainsCaseInsensitive("x\xFFy", "\xFFY"), "an ill-formed byte matches itself");
	Check(!ContainsCaseInsensitive("x\xFEy", "\xFFy"), "different ill-formed bytes differ");
	Check(!ContainsCaseInsensitive("x\xEF\xBF\xBDy", "\xFF"), "an ill-formed byte is not U+FFFD");
	Check(ContainsCaseInsensitive("abc", ""), "an empty needle matches");
	Check(ContainsCaseInsensitive("", ""), "an empty needle matches an empty haystack");
	Check(!ContainsCaseInsensitive("", "a"), "nothing is found in an empty haystack");
	// The comparison ends at an embedded NUL (documented).
	Check(!ContainsCaseInsensitive(std::string_view("ab\0cd", 5), "cd"),
	      "text after an embedded NUL is not searched");
	// Views that are not NUL-terminated.
	const std::string_view haystack = std::string_view("PPSA01234-extra").substr(0, 9);
	Check(ContainsCaseInsensitive(haystack, "ppsa01234"), "a haystack view");
	Check(!ContainsCaseInsensitive(haystack, "ppsa01234-"), "a haystack view ends at its size");
	Check(ContainsCaseInsensitive("abc", std::string_view("bcd").substr(0, 2)), "a needle view");
}

void TestCompare() {
	Check(CompareCaseInsensitive("abc", "ABC") == 0, "ASCII case folding");
	Check(CompareCaseInsensitive("a", "B") == -1, "a before B");
	Check(CompareCaseInsensitive("B", "a") == 1, "B after a");
	Check(CompareCaseInsensitive("abc", "abd") == -1, "-1 for less");
	Check(CompareCaseInsensitive("abd", "abc") == 1, "1 for greater");
	Check(CompareCaseInsensitive("", "") == 0, "empty texts are equal");
	Check(CompareCaseInsensitive("", "a") == -1, "empty before text");
	Check(CompareCaseInsensitive("a", "") == 1, "text after empty");
	Check(CompareCaseInsensitive("ab", "abc") == -1, "a prefix orders first");
	Check(CompareCaseInsensitive("\xC3\x89", "\xC3\xA9") == 0, "U+00C9 equals U+00E9");
	Check(CompareCaseInsensitive("\xCE\xA3", "\xCF\x83") == 0, "U+03A3 equals U+03C3");
	Check(CompareCaseInsensitive("\xD0\x96", "\xD0\xB6") == 0, "U+0416 equals U+0436");
	Check(CompareCaseInsensitive("\xC3\xA9", "f") == 1, "U+00E9 orders after f");
	Check(CompareCaseInsensitive(std::string_view("abcz").substr(0, 3), "ABC") == 0,
	      "views that are not NUL-terminated");
	Check(CompareCaseInsensitive(std::string_view("a\0b", 3), std::string_view("a\0c", 3)) == 0,
	      "the comparison ends at an embedded NUL");
}

void CheckSanitize(std::string_view input, const std::string& expected, const std::string& name) {
	const auto result = SanitizeUtf8(input);
	Check(result == expected, name + ": SanitizeUtf8(" + Hex(input) + ") is " + Hex(result) +
	                              ", expected " + Hex(expected));
}

void TestSanitize() {
	CheckSanitize("", "", "empty text");
	CheckSanitize("plain ASCII", "plain ASCII", "ASCII");
	const std::string valid = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBD";
	CheckSanitize(valid, valid, "valid two-, three-, and four-byte sequences");
	CheckSanitize(std::string_view("a\0b", 3), std::string("a\0b", 3), "NUL is valid UTF-8");
	CheckSanitize("\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF", "U+10FFFF is valid");
	CheckSanitize("\xED\x9F\xBF\xEE\x80\x80", "\xED\x9F\xBF\xEE\x80\x80",
	              "U+D7FF and U+E000 around the surrogates are valid");
	CheckSanitize("\xE0\xA0\x80\xF0\x90\x80\x80", "\xE0\xA0\x80\xF0\x90\x80\x80",
	              "the smallest three- and four-byte forms are valid");

	CheckSanitize("\xC0\xAF", Replacements(2), "overlong C0 AF");
	CheckSanitize("\xC1\xBF", Replacements(2), "overlong C1 BF");
	CheckSanitize("\xE0\x80\xAF", Replacements(3), "overlong E0 80 AF");
	CheckSanitize("\xF0\x80\x80\xAF", Replacements(4), "overlong F0 80 80 AF");
	CheckSanitize("\xED\xA0\x80", Replacements(3), "surrogate ED A0 80");
	CheckSanitize("\xED\xBF\xBF", Replacements(3), "surrogate ED BF BF");
	CheckSanitize("\xF4\x90\x80\x80", Replacements(4), "above U+10FFFF F4 90 80 80");
	CheckSanitize("\xF5\x80\x80\x80", Replacements(4), "lead byte F5");
	CheckSanitize("\xFF", Replacements(1), "byte FF");
	CheckSanitize("\x80", Replacements(1), "a stray continuation byte");
	CheckSanitize("\xE2\x82", Replacements(1), "truncated E2 82");
	CheckSanitize("a\xE2\x82", "a" + Replacements(1), "truncated at the end");
	CheckSanitize("\xE2\x82"
	              "a",
	              Replacements(1) + "a", "truncated before ASCII");
	CheckSanitize("\xF0\x9F\x98", Replacements(1), "truncated four-byte sequence");
	CheckSanitize("\xF0\x9F\x98\xC3\xA9", Replacements(1) + "\xC3\xA9",
	              "truncated before a valid sequence");
	CheckSanitize("\xC3", Replacements(1), "a lone lead byte");
	// The example of the Unicode Standard, section 3.9 (U+FFFD substitution of maximal subparts).
	CheckSanitize("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64",
	              "a" + Replacements(3) + "b" + Replacements(1) + "c" + Replacements(2) + "d",
	              "Unicode table 3-8");
	CheckSanitize("a\xC3\xA9"
	              "b\xFF"
	              "c\xED\xA0\x80"
	              "d",
	              "a\xC3\xA9"
	              "b" +
	                  Replacements(1) + "c" + Replacements(3) + "d",
	              "mixed valid and invalid");
	const auto twice = SanitizeUtf8(SanitizeUtf8("\xC0\xAF\xE2\x82"));
	Check(twice == SanitizeUtf8("\xC0\xAF\xE2\x82"), "SanitizeUtf8 is idempotent");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("TextTests");
	TestTrimEachWhiteSpaceCodePoint();
	TestTrimLookAlikes();
	TestContains();
	TestCompare();
	TestSanitize();
	std::printf("TextTests: all tests passed\n");
	return 0;
}
