#pragma once

// RapidCheck generators and reference helpers for the library properties (Properties 7 and 8,
// task 12.2). Header only, and only for property TUs: RapidCheck needs exceptions, which the test
// support library is built without. The generator functions draw values with operator* on
// generators, so call them only inside a property.
//
// Text is built from a fixed alphabet of units (Units()): ASCII letters of both cases, letters
// whose case folding is not one-to-one (U+00DF and U+1E9E fold to "ss", U+0130 to "i" U+0307,
// U+FB01 to "fi", U+212A KELVIN SIGN to "k"), Latin, Greek, and Cyrillic case pairs, CJK, an emoji,
// digits, punctuation, the White_Space code points U+0009, U+000A, U+0020, U+0085, U+00A0, U+2003,
// U+2028, and U+3000, and one ill-formed byte (0xFF). No unit starts with a continuation byte, so a
// concatenation of units decodes back into the same units. ReferenceFold() is the full case
// folding (Unicode CaseFolding.txt, statuses C and F) of every code point the alphabet uses.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <rapidcheck.h>
#include <string>
#include <string_view>
#include <vector>

namespace KytyTests::LibraryGen {

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

inline bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& PickOf(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

// ---- Alphabet and reference folding

struct Unit {
	std::string bytes;
	bool        whitespace = false; // a White_Space code point (TrimWhitespace removes it)
	bool        valid      = true;  // false for the ill-formed byte
};

inline const std::vector<Unit>& Units() {
	static const std::vector<Unit> units = {
	    {"a"},
	    {"A"},
	    {"b"},
	    {"B"},
	    {"f"},
	    {"F"},
	    {"i"},
	    {"I"},
	    {"k"},
	    {"K"},
	    {"s"},
	    {"S"},
	    {"\xC3\x9F"},         // U+00DF LATIN SMALL LETTER SHARP S
	    {"\xE1\xBA\x9E"},     // U+1E9E LATIN CAPITAL LETTER SHARP S
	    {"\xE2\x84\xAA"},     // U+212A KELVIN SIGN
	    {"\xC4\xB0"},         // U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE
	    {"\xC4\xB1"},         // U+0131 LATIN SMALL LETTER DOTLESS I
	    {"\xEF\xAC\x81"},     // U+FB01 LATIN SMALL LIGATURE FI
	    {"\xC3\xA9"},         // U+00E9
	    {"\xC3\x89"},         // U+00C9
	    {"\xCF\x83"},         // U+03C3 GREEK SMALL LETTER SIGMA
	    {"\xCE\xA3"},         // U+03A3 GREEK CAPITAL LETTER SIGMA
	    {"\xCF\x82"},         // U+03C2 GREEK SMALL LETTER FINAL SIGMA
	    {"\xD0\xB6"},         // U+0436
	    {"\xD0\x96"},         // U+0416
	    {"\xE4\xB8\xAD"},     // U+4E2D
	    {"\xF0\x9F\x8E\xAE"}, // U+1F3AE
	    {"0"},
	    {"1"},
	    {"9"},
	    {"-"},
	    {"."},
	    {"_"},
	    {":"},
	    {" ", true},
	    {"\t", true},
	    {"\n", true},
	    {"\xC2\x85", true},     // U+0085
	    {"\xC2\xA0", true},     // U+00A0
	    {"\xE2\x80\x83", true}, // U+2003
	    {"\xE2\x80\xA8", true}, // U+2028
	    {"\xE3\x80\x80", true}, // U+3000
	    {"\xFF", false, false}, // ill-formed
	};
	return units;
}

// The code points of text (well-formed UTF-8 apart from 0xFF bytes, which decode as U+FFFD).
inline std::vector<char32_t> DecodeCodePoints(std::string_view text) {
	std::vector<char32_t> code_points;
	size_t                i = 0;
	while (i < text.size()) {
		const auto lead = static_cast<uint8_t>(text[i]);
		size_t     length;
		char32_t   code_point;
		if (lead < 0x80) {
			length     = 1;
			code_point = lead;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			length     = 2;
			code_point = lead & 0x1Fu;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length     = 3;
			code_point = lead & 0x0Fu;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			length     = 4;
			code_point = lead & 0x07u;
		} else {
			code_points.push_back(0xFFFD);
			i++;
			continue;
		}
		for (size_t k = 1; k < length; k++) {
			code_point = (code_point << 6u) | (static_cast<uint8_t>(text[i + k]) & 0x3Fu);
		}
		code_points.push_back(code_point);
		i += length;
	}
	return code_points;
}

// The full case folding of text: ASCII capitals to small letters, the alphabet's non-ASCII
// capitals and special letters per CaseFolding.txt, every other code point unchanged.
inline std::vector<char32_t> ReferenceFold(std::string_view text) {
	std::vector<char32_t> folded;
	for (const char32_t c: DecodeCodePoints(text)) {
		switch (c) {
			case 0x00DF:
			case 0x1E9E:
				folded.push_back('s');
				folded.push_back('s');
				break;
			case 0x212A: folded.push_back('k'); break;
			case 0x0130:
				folded.push_back('i');
				folded.push_back(0x0307);
				break;
			case 0xFB01:
				folded.push_back('f');
				folded.push_back('i');
				break;
			case 0x00C9: folded.push_back(0x00E9); break;
			case 0x03A3:
			case 0x03C2: folded.push_back(0x03C3); break;
			case 0x0416: folded.push_back(0x0436); break;
			default: folded.push_back(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); break;
		}
	}
	return folded;
}

// -1, 0, or 1: the folded code point sequences compared lexicographically (a prefix first).
inline int ReferenceCompareText(std::string_view a, std::string_view b) {
	const auto fa = ReferenceFold(a);
	const auto fb = ReferenceFold(b);
	if (fa == fb) {
		return 0;
	}
	return fa < fb ? -1 : 1;
}

// Two units match ignoring case when they are the same unit, or both are well-formed and fold to
// the same sequence (U+00DF matches U+1E9E, "s" matches neither).
inline bool UnitsMatch(size_t a, size_t b) {
	const auto& units = Units();
	return a == b || (units[a].valid && units[b].valid &&
	                  ReferenceFold(units[a].bytes) == ReferenceFold(units[b].bytes));
}

// ---- Text

struct Text {
	std::vector<size_t> units; // indices into Units()
	std::string         bytes;

	void Append(size_t unit) {
		units.push_back(unit);
		bytes += Units()[unit].bytes;
	}
};

inline Text TextOf(const std::vector<size_t>& units) {
	Text text;
	for (const size_t unit: units) {
		text.Append(unit);
	}
	return text;
}

// A unit; letters most of the time (a small pool half of the time, so that searches and sort
// keys collide often), whitespace only when allowed.
inline size_t GenerateUnit(bool allow_whitespace) {
	const auto& units = Units();
	for (;;) {
		const size_t unit = OneIn(2) ? Choose<size_t>(0, 12) : Choose<size_t>(0, units.size());
		if (allow_whitespace || !units[unit].whitespace) {
			return unit;
		}
	}
}

inline Text GenerateText(int min_units, int max_units, bool allow_whitespace = true) {
	Text      text;
	const int count = Choose(min_units, max_units + 1);
	for (int i = 0; i < count; i++) {
		text.Append(GenerateUnit(allow_whitespace));
	}
	return text;
}

inline Text GenerateWhitespace(int max_units) {
	static const std::vector<size_t> kWhitespace = [] {
		std::vector<size_t> indices;
		for (size_t i = 0; i < Units().size(); i++) {
			if (Units()[i].whitespace) {
				indices.push_back(i);
			}
		}
		return indices;
	}();
	Text      text;
	const int count = Choose(0, max_units + 1);
	for (int i = 0; i < count; i++) {
		text.Append(PickOf(kWhitespace));
	}
	return text;
}

// The same text with some units replaced by another unit that matches it ignoring case.
inline Text VaryCase(const Text& text) {
	Text varied;
	for (const size_t unit: text.units) {
		std::vector<size_t> matches;
		for (size_t other = 0; other < Units().size(); other++) {
			if (UnitsMatch(unit, other)) {
				matches.push_back(other);
			}
		}
		varied.Append(OneIn(2) ? unit : PickOf(matches));
	}
	return varied;
}

// The units without leading and trailing whitespace units.
inline std::vector<size_t> ReferenceTrim(const std::vector<size_t>& units) {
	size_t begin = 0;
	size_t end   = units.size();
	while (begin < end && Units()[units[begin]].whitespace) {
		begin++;
	}
	while (end > begin && Units()[units[end - 1]].whitespace) {
		end--;
	}
	return {units.begin() + static_cast<ptrdiff_t>(begin),
	        units.begin() + static_cast<ptrdiff_t>(end)};
}

// True when needle occurs in haystack, unit by unit with UnitsMatch; an empty needle matches.
inline bool ReferenceContains(const std::vector<size_t>& haystack,
                              const std::vector<size_t>& needle) {
	if (needle.size() > haystack.size()) {
		return false;
	}
	for (size_t start = 0; start + needle.size() <= haystack.size(); start++) {
		bool match = true;
		for (size_t i = 0; i < needle.size() && match; i++) {
			match = UnitsMatch(haystack[start + i], needle[i]);
		}
		if (match) {
			return true;
		}
	}
	return false;
}

// ---- Search cases (Property 7)

struct SearchEntry {
	Text title;
	Text title_id;
};

// 0-24 entries; titles of 0-8 units with inner whitespace, title IDs empty or 1-9 units without
// whitespace. A third of the entries repeat an earlier title with a different case.
inline std::vector<SearchEntry> GenerateSearchEntries() {
	std::vector<SearchEntry> entries;
	const int                count = Choose(0, 25);
	for (int i = 0; i < count; i++) {
		SearchEntry entry;
		if (!entries.empty() && OneIn(3)) {
			entry.title = VaryCase(PickOf(entries).title);
		} else {
			entry.title = GenerateText(0, 8);
		}
		if (!OneIn(4)) {
			entry.title_id = GenerateText(1, 9, false);
		}
		entries.push_back(std::move(entry));
	}
	return entries;
}

// A search text: in 6 of 10 cases a case-varied piece of an entry's title or title ID, in 2 random
// units, in 2 empty or whitespace only; with 0-2 whitespace units on either side half of the time.
inline Text GenerateQuery(const std::vector<SearchEntry>& entries) {
	Text core;
	switch (Choose(0, 10)) {
		case 0: break;
		case 1: core = GenerateWhitespace(3); break;
		case 2:
		case 3: core = GenerateText(1, 4); break;
		default:
			if (!entries.empty()) {
				const auto& entry  = PickOf(entries);
				const auto& source = OneIn(2) ? entry.title : entry.title_id;
				if (!source.units.empty()) {
					const size_t begin = Choose<size_t>(0, source.units.size());
					const size_t end   = Choose<size_t>(begin + 1, source.units.size() + 1);
					core = VaryCase(TextOf({source.units.begin() + static_cast<ptrdiff_t>(begin),
					                        source.units.begin() + static_cast<ptrdiff_t>(end)}));
				}
			}
			if (ReferenceTrim(core.units).empty()) {
				// No entry, an empty source, or a whitespace-only piece: blank queries come from
				// cases 0 and 1.
				core = GenerateText(1, 4, false);
			}
			break;
	}
	if (!OneIn(2)) {
		return core;
	}
	Text query = GenerateWhitespace(2);
	for (const size_t unit: core.units) {
		query.Append(unit);
	}
	for (const size_t unit: GenerateWhitespace(2).units) {
		query.Append(unit);
	}
	return query;
}

// ---- Rows (Property 8)

struct RowModel {
	Text                     title;
	Text                     title_id;
	std::vector<std::string> game_version;     // components; none for an empty version
	std::vector<std::string> firmware_version; // components; none for an empty version
	std::optional<uint64_t>  size_bytes;
	Text                     path;
	std::string              status;
	Text                     comment;
};

inline std::string JoinVersion(const std::vector<std::string>& components) {
	std::string text;
	for (size_t i = 0; i < components.size(); i++) {
		text += (i == 0 ? "" : ".") + components[i];
	}
	return text;
}

// 0-3 components: numbers 0-20 with up to two leading zeros (1.9, 1.10, 01.000), now and then a
// word ("a", "B", "rc", "Beta").
inline std::vector<std::string> GenerateVersion() {
	std::vector<std::string> components;
	if (OneIn(5)) {
		return components;
	}
	const int count = Choose(1, 4);
	for (int i = 0; i < count; i++) {
		if (OneIn(8)) {
			components.push_back(PickOf(std::vector<std::string> {"a", "B", "rc", "Beta", "beta"}));
		} else {
			components.push_back(std::string(static_cast<size_t>(Choose(0, 3)), '0') +
			                     std::to_string(Choose(0, 21)));
		}
	}
	return components;
}

// 0-30 rows. Values repeat often (titles with another case, equal versions spelled differently,
// equal sizes, the same path), so ties and tie-breaking are exercised.
inline std::vector<RowModel> GenerateRows() {
	static const std::vector<std::string> kStatuses = {
	    "", "Playable", "playable", "PLAYABLE", "Ingame", "Menus", "Boots", "Nothing", "Unknown"};
	std::vector<RowModel> rows;
	const int             count = Choose(0, 31);
	for (int i = 0; i < count; i++) {
		RowModel        row;
		const RowModel* earlier = rows.empty() || !OneIn(3) ? nullptr : &PickOf(rows);
		row.title    = earlier != nullptr ? VaryCase(earlier->title) : GenerateText(0, 5);
		row.title_id = OneIn(3) ? Text {} : GenerateText(1, 4, false);
		row.game_version =
		    earlier != nullptr && OneIn(2) ? earlier->game_version : GenerateVersion();
		row.firmware_version = GenerateVersion();
		if (!OneIn(4)) {
			row.size_bytes = OneIn(2) ? Choose<uint64_t>(0, 4) : *rc::gen::arbitrary<uint64_t>();
		}
		row.path    = earlier != nullptr && OneIn(2) ? earlier->path : GenerateText(0, 4, false);
		row.status  = PickOf(kStatuses);
		row.comment = OneIn(2) ? Text {} : GenerateText(0, 4);
		rows.push_back(std::move(row));
	}
	return rows;
}

} // namespace KytyTests::LibraryGen
