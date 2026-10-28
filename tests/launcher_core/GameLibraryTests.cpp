// Example tests of the Launcher_Core library search and sorting (src/launcher/core/gameLibrary.h,
// task 12.2): every column in both directions, empty values and absent sizes, mixed-case and
// non-ASCII text, numeric versions, tie-breaking, and blank, whitespace-padded, and title-ID
// searches.

#include "launcher/core/gameLibrary.h"
#include "support/noDisplay.h"

#include <algorithm>
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

using kyty::core::Column;
using kyty::core::CompareForColumn;
using kyty::core::FilterIndices;
using kyty::core::GameEntry;
using kyty::core::GameRow;
using kyty::core::MatchesSearch;
using kyty::core::RowFromEntry;
using kyty::core::SortRows;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameLibraryTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Join(const std::vector<size_t>& indices) {
	std::string text;
	for (const size_t index: indices) {
		text += (text.empty() ? "" : " ") + std::to_string(index);
	}
	return "[" + text + "]";
}

GameEntry Entry(std::string title, std::string title_id) {
	GameEntry entry;
	entry.path       = "/games/" + title_id;
	entry.title_name = std::move(title);
	entry.title_id   = std::move(title_id);
	return entry;
}

GameRow Row(std::string title, std::string path = "/games/x") {
	GameRow row;
	row.title = std::move(title);
	row.path  = std::move(path);
	return row;
}

// Sorts rows by column in both directions and checks the ascending order and that descending is
// its exact reverse.
void CheckSort(const std::vector<GameRow>& rows, Column column, const std::vector<size_t>& expected,
               const std::string& name) {
	const auto ascending = SortRows(rows, column, false);
	Check(ascending == expected,
	      name + ": ascending is " + Join(ascending) + ", expected " + Join(expected));
	const std::vector<size_t> reversed(expected.rbegin(), expected.rend());
	const auto                descending = SortRows(rows, column, true);
	Check(descending == reversed,
	      name + ": descending is " + Join(descending) + ", expected " + Join(reversed));
	for (size_t i = 1; i < expected.size(); i++) {
		const auto& a = rows[expected[i - 1]];
		const auto& b = rows[expected[i]];
		Check(CompareForColumn(a, b, column) <= 0, name + ": adjacent pair " + std::to_string(i));
		Check(CompareForColumn(a, b, column) == -CompareForColumn(b, a, column),
		      name + ": antisymmetry at " + std::to_string(i));
	}
}

void TestRowFromEntry() {
	GameEntry entry;
	entry.path               = "/games/Some Game";
	entry.kind               = GameEntry::Kind::Directory;
	entry.title_name         = "Some Game";
	entry.title_id           = "PPSA01234";
	entry.game_version       = "01.002";
	entry.firmware_version   = "1.50";
	entry.metadata_available = true;
	const auto row           = RowFromEntry(entry);
	Check(row.title == "Some Game", "title from title_name");
	Check(row.title_id == "PPSA01234", "title ID");
	Check(row.game_version == "01.002", "game version");
	Check(row.firmware_version == "1.50", "firmware version");
	Check(row.path == "/games/Some Game", "path");
	Check(!row.size_bytes.has_value(), "no size");
	Check(row.status.empty() && row.comment.empty(), "no status or comment");
	Check(RowFromEntry(GameEntry {}) == GameRow {}, "an empty entry gives an empty row");
}

void TestTitleSort() {
	// Mixed case and non-ASCII: U+00E9 and U+00C9 fold alike and order after ASCII z; equal folded
	// titles order by bytes (U+00C9 before U+00E9, "APPLE" before "Apple").
	const std::vector<GameRow> rows = {Row("zelda"),  Row("Apple"),        Row("\xC3\xA9tude"),
	                                   Row("banana"), Row("\xC3\x89tude"), Row("APPLE"),
	                                   Row(""),       Row("\xCE\xA9mega"), Row("\xD0\x96uk")};
	CheckSort(rows, Column::Title, {6, 5, 1, 3, 0, 4, 2, 7, 8}, "title, mixed case and non-ASCII");
	// Requirement 7.3: the startup order is ascending by title.
	const auto order = SortRows(rows, Column::Title, false);
	Check(rows[order.front()].title.empty(), "an empty title orders first");
	Check(rows[order.back()].title == "\xD0\x96uk", "Cyrillic orders after Greek");
}

void TestTextColumns() {
	// TitleId, Path, Status, and Comment compare as case-insensitive text with empty values first.
	struct Case {
		Column      column;
		const char* name;
		std::string GameRow::* member;
	};
	const Case cases[] = {
	    {Column::TitleId, "title ID", &GameRow::title_id},
	    {Column::Path, "path", &GameRow::path},
	    {Column::Status, "status", &GameRow::status},
	    {Column::Comment, "comment", &GameRow::comment},
	};
	const std::vector<std::string> values = {"beta", "", "Alpha", "ALPHA2", "\xC3\x89t\xC3\xA9",
	                                         "gamma"};
	for (const auto& c: cases) {
		std::vector<GameRow> rows;
		for (size_t i = 0; i < values.size(); i++) {
			auto row = Row("same title", "/p");
			// Path is the column itself in one case; otherwise keep the path equal so the order
			// comes from the column alone.
			row.*c.member = values[i];
			rows.push_back(row);
		}
		CheckSort(rows, c.column, {1, 2, 3, 0, 5, 4}, c.name);
	}
	// Status is the display text the UI supplies.
	const std::vector<GameRow> statuses = [] {
		std::vector<GameRow> rows;
		for (const char* status: {"Playable", "boots", "", "Ingame", "nothing", "Menus"}) {
			auto row   = Row("t");
			row.status = status;
			rows.push_back(row);
		}
		return rows;
	}();
	CheckSort(statuses, Column::Status, {2, 1, 3, 5, 4, 0}, "status texts");
}

void TestVersionColumns() {
	for (const Column column: {Column::GameVersion, Column::FirmwareVersion}) {
		const char* name = column == Column::GameVersion ? "game version" : "firmware version";
		std::vector<GameRow> rows;
		// Titles break the tie between equal versions: 01.000 equals 1.0 and 1.
		const std::vector<std::pair<std::string, std::string>> values = {
		    {"1.10", "a"}, {"1.9", "b"}, {"01.000", "c"}, {"", "d"},  {"1.0", "e"},
		    {"2", "f"},    {"10", "g"},  {"1", "h"},      {"0", "i"}, {"1.9a", "j"}};
		for (const auto& [version, title]: values) {
			auto row                                                                  = Row(title);
			(column == Column::GameVersion ? row.game_version : row.firmware_version) = version;
			rows.push_back(row);
		}
		// "" first; 0; 01.000 = 1.0 = 1 by title (c, e, h); 1.9; 1.10; 1.9a (its second component
		// is a word, and a number orders before a word); 2; 10.
		CheckSort(rows, column, {3, 8, 2, 4, 7, 1, 0, 9, 5, 6}, name);
		Check(CompareForColumn(rows[1], rows[0], column) < 0, std::string(name) + ": 1.9 < 1.10");
		Check(CompareForColumn(Row("x"), rows[8], column) < 0,
		      std::string(name) + ": an empty version orders before 0");
	}
}

void TestSizeColumn() {
	std::vector<GameRow>                       rows;
	const std::vector<std::optional<uint64_t>> sizes = {uint64_t {10}, std::nullopt, uint64_t {0},
	                                                    uint64_t {9},  UINT64_MAX,   uint64_t {100},
	                                                    std::nullopt};
	const char*                                titles[] = {"a", "b", "c", "d", "e", "f", "a"};
	for (size_t i = 0; i < sizes.size(); i++) {
		auto row       = Row(titles[i], "/p" + std::to_string(i));
		row.size_bytes = sizes[i];
		rows.push_back(row);
	}
	// Absent sizes first (by title: a, b), then by value: 0, 9, 10, 100, UINT64_MAX (not as text:
	// "9" would follow "100").
	CheckSort(rows, Column::Size, {6, 1, 2, 3, 0, 5, 4}, "size");
}

void TestTieBreaking() {
	// Equal column values: by title case-insensitively, then by title bytes, then by path bytes.
	std::vector<GameRow> rows = {Row("beta", "/b"),  Row("Alpha", "/z"), Row("alpha", "/a"),
	                             Row("Alpha", "/a"), Row("ALPHA", "/m"), Row("alpha", "/a")};
	for (auto& row: rows) {
		row.title_id = "PPSA00001";
	}
	// ALPHA < Alpha (/a) < Alpha (/z) < alpha (/a) = alpha (/a, kept in input order) < beta.
	CheckSort(rows, Column::TitleId, {4, 3, 1, 2, 5, 0}, "ties");
	Check(CompareForColumn(rows[2], rows[5], Column::TitleId) == 0, "identical rows compare 0");
	Check(CompareForColumn(rows[3], rows[1], Column::TitleId) < 0, "path bytes break a title tie");
	// Path bytes, not case-insensitive text, break the last tie.
	const std::vector<GameRow> paths = {Row("t", "/b"), Row("t", "/B"), Row("t", "/a")};
	CheckSort(paths, Column::Title, {1, 2, 0}, "path bytes");
	// Every column in both directions on rows that differ only in title and path.
	for (const Column column:
	     {Column::Title, Column::TitleId, Column::GameVersion, Column::FirmwareVersion,
	      Column::Size, Column::Path, Column::Status, Column::Comment}) {
		const std::vector<GameRow> same = {Row("b", "/1"), Row("a", "/2"), Row("a", "/1")};
		const auto                 expected =
		    column == Column::Path ? std::vector<size_t> {2, 0, 1} : std::vector<size_t> {2, 1, 0};
		CheckSort(same, column, expected, "column " + std::to_string(static_cast<int>(column)));
	}
}

void TestEmptyInput() {
	Check(SortRows({}, Column::Title, false).empty(), "sorting no rows");
	Check(SortRows({}, Column::Size, true).empty(), "sorting no rows, descending");
	const std::vector<GameRow> one = {Row("x")};
	Check(SortRows(one, Column::Comment, true) == std::vector<size_t> {0}, "sorting one row");
}

void TestSearch() {
	const std::vector<GameEntry> entries = {
	    Entry("The Legend of Zelda", "PPSA01234"),
	    Entry("\xC3\x89lan Vital", "CUSA00042"),                        // U+00C9
	    Entry("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91", "PPSA09999"), // Greek capitals
	    Entry("", ""),
	    Entry("Ma\xC3\x9Fstab", "PPSA00100"), // U+00DF
	    Entry("\xE2\x84\xAAing", "UP0001"),   // U+212A KELVIN SIGN
	};
	const auto check = [&](std::string_view query, const std::vector<size_t>& expected,
	                       const std::string& name) {
		const auto result = FilterIndices(entries, query);
		Check(result == expected,
		      name + ": FilterIndices is " + Join(result) + ", expected " + Join(expected));
		for (size_t i = 0; i < entries.size(); i++) {
			const bool listed = std::find(expected.begin(), expected.end(), i) != expected.end();
			Check(MatchesSearch(entries[i], query) == listed,
			      name + ": MatchesSearch for entry " + std::to_string(i));
		}
	};
	const std::vector<size_t> all = {0, 1, 2, 3, 4, 5};
	// Blank queries match everything, including an entry without title and title ID.
	check("", all, "an empty query");
	check(" ", all, "a space");
	check("\t\n\r\v\f ", all, "ASCII whitespace");
	check("\xE3\x80\x80\xC2\xA0\xE2\x80\x83\xC2\x85\xE2\x80\xA8", all, "Unicode whitespace only");
	// Title matches, ignoring case and surrounding whitespace.
	check("zelda", {0}, "a title word");
	check("LEGEND OF", {0}, "inner whitespace is kept");
	check("legendof", {}, "inner whitespace is not removed");
	check("  zelda\t", {0}, "ASCII whitespace around the text");
	check("\xE3\x80\x80zelda\xC2\xA0", {0}, "U+3000 and U+00A0 around the text");
	check("\xC3\xA9lan", {1}, "U+00E9 finds U+00C9");
	check("\xCF\x83\xCE\xBF\xCF\x86", {2}, "Greek small letters find capitals");
	check("elan", {}, "an accented letter is not its base letter");
	// Title ID matches.
	check("ppsa", {0, 2, 4}, "a title ID prefix, ignoring case");
	check("01234", {0}, "a title ID part");
	check("CUSA00042", {1}, "a whole title ID");
	check(" up0001 ", {5}, "a title ID with whitespace around it");
	check("Zelda PPSA", {}, "title and title ID are separate texts");
	check("nothing like this", {}, "no match");
	// Letters whose case folding is not one-to-one match per code point, so a longer text never
	// matches more than its prefix (Requirement 7.14).
	check("MA\xE1\xBA\x9ESTAB", {4}, "U+1E9E finds U+00DF");
	check("massstab", {}, "ss does not find U+00DF");
	check("mas", {}, "s does not find U+00DF");
	check("ma", {4}, "the text before U+00DF");
	check("KING", {5}, "K finds U+212A KELVIN SIGN");
	check("\xE2\x84\xAA", {5}, "U+212A finds itself");
}

void TestSearchSpeed() {
	// Requirement 7.13: 1,000 entries well within 100 ms (the best of three runs, to be robust on
	// a loaded host).
	std::vector<GameEntry> entries;
	for (int i = 0; i < 1000; i++) {
		entries.push_back(
		    Entry("Game number " + std::to_string(i) + " \xC3\x89t\xC3\xA9 journ\xC3\xA9",
		          "PPSA" + std::to_string(10000 + i)));
	}
	auto   best  = std::chrono::steady_clock::duration::max();
	size_t found = 0;
	for (int run = 0; run < 3; run++) {
		const auto start = std::chrono::steady_clock::now();
		found            = FilterIndices(entries, " \xC3\xA9T\xC3\x89 JOURN\xC3\x89 9").size();
		best             = std::min(best, std::chrono::steady_clock::now() - start);
	}
	Check(found == 0, "no entry contains the text after its number");
	Check(FilterIndices(entries, "number 99").size() == 11, "number 99 and 990-999");
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(best).count();
	Check(ms < 100, "1,000 entries filtered in " + std::to_string(ms) + " ms");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameLibraryTests");
	TestRowFromEntry();
	TestTitleSort();
	TestTextColumns();
	TestVersionColumns();
	TestSizeColumn();
	TestTieBreaking();
	TestEmptyInput();
	TestSearch();
	TestSearchSpeed();
	std::printf("GameLibraryTests: all tests passed\n");
	return 0;
}
