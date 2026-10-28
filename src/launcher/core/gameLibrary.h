#pragma once

// Library search and column sorting (Requirements 7.3, 7.12, 7.13, 7.14; plan D18). Ported from
// the Qt launcher's ConfigurationListWidget::filter_configurations()
// (src/launcher/src/configurationListWidget.cpp) and ConfigurationItem::operator<
// (src/launcher/src/configurationItem.cpp).

#include "launcher/core/gameMetadata.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// The list view columns.
enum class Column { Title, TitleId, GameVersion, FirmwareVersion, Size, Path, Status, Comment };

// The values one list row shows. The UI fills the columns that GameEntry does not provide: the size
// once it is known, the compatibility status as its display text (so the Status column sorts as
// text, Requirement 7.12), and the comment.
struct GameRow {
	std::string             title;
	std::string             title_id;
	std::string             game_version;
	std::string             firmware_version;
	std::optional<uint64_t> size_bytes; // absent while unknown
	std::string             path;
	std::string             status;
	std::string             comment;

	friend bool operator==(const GameRow&, const GameRow&) = default;
};

// The row for an entry: title, title ID, versions, and path (the entry's native path string) from
// the entry; no size, an empty status, and an empty comment.
GameRow RowFromEntry(const GameEntry& entry);

// True when the entry's title name or title ID contains TrimWhitespace(query), compared with
// ContainsCaseInsensitive. An empty or whitespace-only query matches every entry.
bool MatchesSearch(const GameEntry& entry, std::string_view query);

// The indices, in increasing order, of the entries that MatchesSearch(entry, query) accepts.
std::vector<size_t> FilterIndices(std::span<const GameEntry> entries, std::string_view query);

// The ascending order of two rows for a sort by column, as -1, 0, or 1. A total preorder:
// 1. the column value: an empty value (an absent size) orders before every non-empty one; then
//    the version columns compare with CompareVersions, Size by value, and the other columns with
//    CompareCaseInsensitive;
// 2. then the title with CompareCaseInsensitive, then the title bytes;
// 3. then the path bytes.
// Rows that are equal in all of these compare 0.
int CompareForColumn(const GameRow& a, const GameRow& b, Column column);

// The row indices in sorted order: ascending is a stable sort by CompareForColumn (rows that
// compare 0 keep their input order), and descending is the exact reverse of the ascending result.
std::vector<size_t> SortRows(std::span<const GameRow> rows, Column column, bool descending);

} // namespace kyty::core
