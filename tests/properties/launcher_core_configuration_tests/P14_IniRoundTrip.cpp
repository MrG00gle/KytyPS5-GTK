// Feature: linux-gtk-modernization, Property 14: INI codec round trip
//
// Validates Requirements 9.6 and 9.7. Documents of 0-24 keys: keys in [General], in sections, and
// array keys "GameConfigurations/<n>/<name>" with "GameConfigurations/size" (n from 1 to 12, so
// "10" sorts between "1" and "2"), whose segments mix ASCII letters and digits with characters
// that need %XX or %UXXXX (space, '%', '\', '=', ';', '"', brackets, U+0000, tab, Latin-1,
// CJK, non-BMP, U+FEFF), and the section names "General", "GameConfigurations", and "Launcher".
// Values: strings, string lists of 0-4 elements (an empty list is @Invalid() and a one-element
// list a string, as QSettings stores them), @Invalid(), and opaque @ByteArray(...) values of
// random bytes. Text is Unicode with commas, quotes, '=', ';', backslashes, every C0 control
// character, U+007F-U+00A0, hex digits after control characters, non-characters, non-BMP
// characters, a leading "@" or "@@", @Name(...) look-alikes, and spaces at either end. Writing
// with Serialize() and reading with Parse() must give an equal document without a format error,
// the same typed values, and the same bytes when serialized again; the output is UTF-8 without a
// byte order mark or CR.
//
// Left out of the generator because QSettings (Qt 6.11.2) cannot round-trip them either; the
// unit tests in tests/launcher_core/IniFileTests.cpp pin the Qt behavior instead:
// - U+FEFF in values: Qt's UTF-8 decoder drops it when it is the first character that a value's
//   decoder sees (QStringDecoder skips an initial byte order mark);
// - a first key segment "general" in another letter case than "General": QSettings writes it as
//   [%General], which reads back as "General".

#include "launcher/core/iniFile.h"
#include "support/property.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <rapidcheck.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using kyty::core::IniFile;
using kyty::core::IniValue;
using kyty::core::IniValueKind;

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& PickOf(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

std::string Utf8(char32_t c) {
	std::string out;
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xC0 | (c >> 6u));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else if (c < 0x10000) {
		out += static_cast<char>(0xE0 | (c >> 12u));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else {
		out += static_cast<char>(0xF0 | (c >> 18u));
		out += static_cast<char>(0x80 | ((c >> 12u) & 0x3Fu));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	}
	return out;
}

// Characters of value text: every C0 control character, the characters the dialect quotes or
// escapes, hex digits (escaped after \x and \0 escapes) and other ASCII, U+007F-U+00A0, Latin-1,
// Cyrillic, CJK, line and paragraph separators, a private-use character, U+FFFD, the
// non-characters U+FFFE and U+FFFF, and non-BMP characters (an emoji and U+10FFFF).
const std::vector<char32_t>& ValueAlphabet() {
	static const std::vector<char32_t> alphabet = [] {
		std::vector<char32_t> chars = {
		    U'a',   U'B',   U'z',   U'0',   U'1',   U'7',   U'9',   U'f',    U'F',
		    U'g',   U'x',   U' ',   U' ',   U'\t',  U',',   U'"',   U'=',    U';',
		    U'\\',  U'@',   U'(',   U')',   U'#',   U'[',   U']',   U'%',    U'\'',
		    U'?',   U'/',   0x007F, 0x0080, 0x009F, 0x00A0, 0x00E9, 0x00FF,  0x0416,
		    0x30B2, 0x2028, 0x2029, 0xE000, 0xFFFD, 0xFFFE, 0xFFFF, 0x1F600, 0x10FFFF};
		for (char32_t c = 0; c < 0x20; c++) {
			chars.push_back(c);
		}
		return chars;
	}();
	return alphabet;
}

// Characters of key segments ('/' separates segments, so it is not one of them).
const std::vector<char32_t>& KeyAlphabet() {
	static const std::vector<char32_t> alphabet = {
	    U'a',  U'b',  U'Z',   U'0',   U'9',   U'_',   U'-',    U'.',   U' ',  U'%',
	    U'\\', U'=',  U';',   U'"',   U'[',   U']',   U'@',    U',',   U'#',  0x00,
	    U'\t', U'\n', 0x00E9, 0x00FF, 0x0100, 0x30B2, 0x1F600, 0xFEFF, 0xFFFF};
	return alphabet;
}

std::string GenerateText(const std::vector<char32_t>& alphabet, int min_length, int max_length) {
	std::string text;
	const int   length = Choose(min_length, max_length + 1);
	for (int i = 0; i < length; i++) {
		text += Utf8(PickOf(alphabet));
	}
	return text;
}

// Value text: alphabet characters, often with a leading "@" or "@@", a @Name(...) look-alike, or
// spaces at either end.
std::string GenerateValueText() {
	static const std::vector<std::string> kLookAlikes = {
	    "@ByteArray(x)", "@Invalid()", "@String(a)", "@Variant(", "@Foo(b)", "@@", "@", "@)"};
	std::string text = GenerateText(ValueAlphabet(), 0, 10);
	switch (Choose(0, 8)) {
		case 0: text = "@" + text; break;
		case 1: text = "@@" + text; break;
		case 2: text = PickOf(kLookAlikes) + (OneIn(2) ? std::string() : text); break;
		default: break;
	}
	if (OneIn(4)) {
		text = (OneIn(2) ? " " : "  ") + text;
	}
	if (OneIn(4)) {
		text += OneIn(2) ? " " : " \t ";
	}
	return text;
}

std::string GenerateSegment() {
	return GenerateText(KeyAlphabet(), 1, 6);
}

bool IsGeneralInOtherCase(const std::string& segment) {
	if (segment.size() != 7 || segment == "General") {
		return false;
	}
	for (size_t i = 0; i < 7; i++) {
		const char c = segment[i];
		if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != "general"[i]) {
			return false;
		}
	}
	return true;
}

// A key: one segment ([General]), a section and one or two more segments, or an array element.
std::string GenerateKey() {
	switch (Choose(0, 6)) {
		case 0: return GenerateSegment();
		case 1:
		case 2:
			return IniFile::ArrayKey("GameConfigurations", Choose(0, 12),
			                         OneIn(3) ? std::string("name") : GenerateSegment());
		default: {
			static const std::vector<std::string> kSections = {"General", "GameConfigurations",
			                                                   "Launcher"};
			std::string section = OneIn(2) ? PickOf(kSections) : GenerateSegment();
			if (IsGeneralInOtherCase(section)) {
				section = "General";
			}
			std::string key = section + "/" + GenerateSegment();
			if (OneIn(3)) {
				key += "/" + GenerateSegment();
			}
			return key;
		}
	}
}

// What the document must read back for a key.
struct Wanted {
	IniValueKind                            kind = IniValueKind::Invalid;
	std::optional<std::string>              string;
	std::optional<std::vector<std::string>> list;
	std::optional<std::string>              raw; // checked for opaque values only
};

} // namespace

KYTY_PROPERTY(14, "INI codec round trip") {
	return rc::check(name, []() {
		IniFile                       file;
		std::map<std::string, Wanted> expected;
		std::map<std::string, int>    kinds;
		const int                     count = Choose(0, 25);
		for (int i = 0; i < count; i++) {
			const std::string key = GenerateKey();
			Wanted            want;
			switch (Choose(0, 8)) {
				case 0:
				case 1:
				case 2: {
					const std::string text = GenerateValueText();
					RC_ASSERT(file.SetString(key, text));
					want.kind   = IniValueKind::String;
					want.string = text;
					want.list   = std::vector<std::string> {text};
					kinds["string"]++;
					break;
				}
				case 3:
				case 4:
				case 5: {
					std::vector<std::string> elements;
					const int                size = Choose(0, 5);
					for (int e = 0; e < size; e++) {
						elements.push_back(GenerateValueText());
					}
					RC_ASSERT(file.SetStringList(key, elements));
					want.kind   = size == 0   ? IniValueKind::Invalid
					              : size == 1 ? IniValueKind::String
					                          : IniValueKind::StringList;
					want.list   = elements;
					want.string = size == 0   ? std::optional<std::string>("")
					              : size == 1 ? std::optional<std::string>(elements.front())
					                          : std::nullopt;
					kinds[size == 0 ? "empty list" : size == 1 ? "one-element list" : "list"]++;
					break;
				}
				case 6:
					RC_ASSERT(file.Set(key, IniValue::Invalid()));
					want.kind   = IniValueKind::Invalid;
					want.string = "";
					want.list   = std::vector<std::string> {};
					kinds["invalid"]++;
					break;
				default: {
					std::string bytes;
					const int   size = Choose(0, 17);
					for (int b = 0; b < size; b++) {
						bytes += static_cast<char>(Choose(0, 256));
					}
					const IniValue value = IniValue::FromByteArray(bytes);
					RC_ASSERT(value.kind == IniValueKind::Opaque);
					RC_ASSERT(file.Set(key, value));
					want.kind = IniValueKind::Opaque;
					want.raw  = value.text;
					kinds["byte array"]++;
					break;
				}
			}
			expected.insert_or_assign(key, std::move(want));
		}
		const bool sized = OneIn(3);
		if (sized) {
			RC_ASSERT(file.SetArraySize("GameConfigurations", count));
			Wanted want;
			want.kind                           = IniValueKind::String;
			want.string                         = std::to_string(count);
			want.list                           = std::vector<std::string> {*want.string};
			expected["GameConfigurations/size"] = want;
		}

		const std::string serialized = file.Serialize();
		RC_LOG() << serialized << "\n";
		RC_ASSERT(!serialized.starts_with("\xEF\xBB\xBF"));
		RC_ASSERT(serialized.find('\r') == std::string::npos);
		RC_ASSERT(file.Entries().size() == expected.size());

		const IniFile parsed = IniFile::Parse(serialized);
		RC_ASSERT(!parsed.HasFormatError());
		RC_ASSERT(parsed == file);
		RC_ASSERT(parsed.Serialize() == serialized);
		for (const auto& [key, want]: expected) {
			RC_LOG() << "key " << key << "\n";
			const IniValue* value = parsed.Find(key);
			RC_ASSERT(value != nullptr);
			RC_ASSERT(value->kind == want.kind);
			RC_ASSERT(parsed.GetString(key) == want.string);
			RC_ASSERT(parsed.GetStringList(key) == want.list);
			if (want.raw) {
				RC_ASSERT(parsed.GetRaw(key) == want.raw);
			}
		}
		if (sized) {
			RC_ASSERT(parsed.ArraySize("GameConfigurations") == count);
		}

		RC_TAG(count == 0 ? "empty document" : count < 8 ? "1-7 keys" : "8-24 keys");
		RC_TAG(kinds.size() >= 3 ? "3+ value kinds" : "fewer value kinds");
	});
}
