#include "launcher/core/gameLibrary.h"

#include "launcher/core/text.h"
#include "launcher/core/version.h"

#include <algorithm>
#include <numeric>

namespace kyty::core {

namespace {

int Sign(int value) {
	return value < 0 ? -1 : (value > 0 ? 1 : 0);
}

// Empty values first, then compare(a, b).
template <typename Compare>
int CompareNonEmpty(std::string_view a, std::string_view b, Compare compare) {
	if (a.empty() || b.empty()) {
		return a.empty() == b.empty() ? 0 : (a.empty() ? -1 : 1);
	}
	return Sign(compare(a, b));
}

int CompareText(std::string_view a, std::string_view b) {
	return CompareNonEmpty(a, b, CompareCaseInsensitive);
}

int CompareVersionText(std::string_view a, std::string_view b) {
	return CompareNonEmpty(a, b, CompareVersions);
}

int CompareSize(const std::optional<uint64_t>& a, const std::optional<uint64_t>& b) {
	if (!a.has_value() || !b.has_value()) {
		return a.has_value() == b.has_value() ? 0 : (a.has_value() ? 1 : -1);
	}
	return *a == *b ? 0 : (*a < *b ? -1 : 1);
}

int CompareColumnValue(const GameRow& a, const GameRow& b, Column column) {
	switch (column) {
		case Column::Title: return CompareText(a.title, b.title);
		case Column::TitleId: return CompareText(a.title_id, b.title_id);
		case Column::GameVersion: return CompareVersionText(a.game_version, b.game_version);
		case Column::FirmwareVersion:
			return CompareVersionText(a.firmware_version, b.firmware_version);
		case Column::Size: return CompareSize(a.size_bytes, b.size_bytes);
		case Column::Path: return CompareText(a.path, b.path);
		case Column::Status: return CompareText(a.status, b.status);
		case Column::Comment: return CompareText(a.comment, b.comment);
	}
	return 0;
}

} // namespace

GameRow RowFromEntry(const GameEntry& entry) {
	GameRow row;
	row.title            = entry.title_name;
	row.title_id         = entry.title_id;
	row.game_version     = entry.game_version;
	row.firmware_version = entry.firmware_version;
	row.path             = entry.path.string();
	return row;
}

namespace {

bool MatchesTrimmed(const GameEntry& entry, std::string_view trimmed) {
	return trimmed.empty() || ContainsCaseInsensitive(entry.title_name, trimmed) ||
	       ContainsCaseInsensitive(entry.title_id, trimmed);
}

} // namespace

bool MatchesSearch(const GameEntry& entry, std::string_view query) {
	return MatchesTrimmed(entry, TrimWhitespace(query));
}

std::vector<size_t> FilterIndices(std::span<const GameEntry> entries, std::string_view query) {
	const auto          trimmed = TrimWhitespace(query);
	std::vector<size_t> indices;
	for (size_t i = 0; i < entries.size(); i++) {
		if (MatchesTrimmed(entries[i], trimmed)) {
			indices.push_back(i);
		}
	}
	return indices;
}

int CompareForColumn(const GameRow& a, const GameRow& b, Column column) {
	if (const int result = CompareColumnValue(a, b, column); result != 0) {
		return result;
	}
	if (const int result = CompareCaseInsensitive(a.title, b.title); result != 0) {
		return result;
	}
	if (const int result = Sign(a.title.compare(b.title)); result != 0) {
		return result;
	}
	return Sign(a.path.compare(b.path));
}

std::vector<size_t> SortRows(std::span<const GameRow> rows, Column column, bool descending) {
	std::vector<size_t> indices(rows.size());
	std::iota(indices.begin(), indices.end(), size_t {0});
	std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
		return CompareForColumn(rows[a], rows[b], column) < 0;
	});
	if (descending) {
		std::reverse(indices.begin(), indices.end());
	}
	return indices;
}

} // namespace kyty::core
