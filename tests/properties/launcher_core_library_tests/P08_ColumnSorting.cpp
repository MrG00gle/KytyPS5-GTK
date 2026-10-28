// Feature: linux-gtk-modernization, Property 8: Column sorting
//
// Validates Requirements 7.3 and 7.12. Lists of 0-30 game rows (libraryGenerators.h) whose values
// repeat often: titles with another letter case, versions such as 1.9, 1.10, and 01.000 with
// leading zeros and word components, equal or absent sizes, shared paths, status texts in several
// cases, empty values in every column. For every column and both directions, SortRows must give a
// permutation in which every adjacent pair is ordered by CompareForColumn, the ascending result
// must equal a stable sort by an independent reference order (column value with empty values
// first, versions by numeric components, Size by value, other columns as case-folded text; then
// the folded title, the title bytes, and the path bytes), CompareForColumn must agree with that
// reference on every pair, and descending must be the exact reverse of ascending.

#include "launcher/core/gameLibrary.h"
#include "launcher/core/text.h"
#include "support/libraryGenerators.h"
#include "support/property.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace {

namespace LibraryGen = KytyTests::LibraryGen;
using kyty::core::Column;
using kyty::core::GameRow;

constexpr Column kColumns[] = {Column::Title,           Column::TitleId, Column::GameVersion,
                               Column::FirmwareVersion, Column::Size,    Column::Path,
                               Column::Status,          Column::Comment};

const char* ColumnName(Column column) {
	switch (column) {
		case Column::Title: return "Title";
		case Column::TitleId: return "TitleId";
		case Column::GameVersion: return "GameVersion";
		case Column::FirmwareVersion: return "FirmwareVersion";
		case Column::Size: return "Size";
		case Column::Path: return "Path";
		case Column::Status: return "Status";
		case Column::Comment: return "Comment";
	}
	return "?";
}

int Sign(int value) {
	return value < 0 ? -1 : (value > 0 ? 1 : 0);
}

bool IsNumber(const std::string& component) {
	return std::all_of(component.begin(), component.end(),
	                   [](char c) { return c >= '0' && c <= '9'; });
}

uint64_t NumberOf(const std::string& component) {
	uint64_t value = 0;
	for (const char c: component) {
		value = value * 10 + static_cast<uint64_t>(c - '0');
	}
	return value;
}

std::string Lower(std::string text) {
	for (auto& c: text) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return text;
}

// Versions by components (generated numbers stay far below overflow): a missing component is 0,
// numbers compare by value, a number orders before a word, words compare ignoring ASCII case.
int ReferenceCompareVersion(const std::vector<std::string>& a, const std::vector<std::string>& b) {
	if (a.empty() || b.empty()) {
		return a.empty() == b.empty() ? 0 : (a.empty() ? -1 : 1);
	}
	for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
		const std::string ca = i < a.size() ? a[i] : "0";
		const std::string cb = i < b.size() ? b[i] : "0";
		if (IsNumber(ca) && IsNumber(cb)) {
			if (NumberOf(ca) != NumberOf(cb)) {
				return NumberOf(ca) < NumberOf(cb) ? -1 : 1;
			}
		} else if (IsNumber(ca) != IsNumber(cb)) {
			return IsNumber(ca) ? -1 : 1;
		} else if (Lower(ca) != Lower(cb)) {
			return Lower(ca) < Lower(cb) ? -1 : 1;
		}
	}
	return 0;
}

int ReferenceCompareText(const std::string& a, const std::string& b) {
	if (a.empty() || b.empty()) {
		return a.empty() == b.empty() ? 0 : (a.empty() ? -1 : 1);
	}
	return LibraryGen::ReferenceCompareText(a, b);
}

int ReferenceCompareColumn(const LibraryGen::RowModel& a, const LibraryGen::RowModel& b,
                           Column column) {
	switch (column) {
		case Column::Title: return ReferenceCompareText(a.title.bytes, b.title.bytes);
		case Column::TitleId: return ReferenceCompareText(a.title_id.bytes, b.title_id.bytes);
		case Column::GameVersion: return ReferenceCompareVersion(a.game_version, b.game_version);
		case Column::FirmwareVersion:
			return ReferenceCompareVersion(a.firmware_version, b.firmware_version);
		case Column::Size:
			if (!a.size_bytes || !b.size_bytes) {
				return a.size_bytes.has_value() == b.size_bytes.has_value()
				           ? 0
				           : (a.size_bytes ? 1 : -1);
			}
			return *a.size_bytes == *b.size_bytes ? 0 : (*a.size_bytes < *b.size_bytes ? -1 : 1);
		case Column::Path: return ReferenceCompareText(a.path.bytes, b.path.bytes);
		case Column::Status: return ReferenceCompareText(a.status, b.status);
		case Column::Comment: return ReferenceCompareText(a.comment.bytes, b.comment.bytes);
	}
	return 0;
}

int ReferenceCompare(const LibraryGen::RowModel& a, const LibraryGen::RowModel& b, Column column) {
	if (const int result = ReferenceCompareColumn(a, b, column); result != 0) {
		return result;
	}
	if (const int result = LibraryGen::ReferenceCompareText(a.title.bytes, b.title.bytes);
	    result != 0) {
		return result;
	}
	if (const int result = Sign(a.title.bytes.compare(b.title.bytes)); result != 0) {
		return result;
	}
	return Sign(a.path.bytes.compare(b.path.bytes));
}

GameRow RowOf(const LibraryGen::RowModel& model) {
	GameRow row;
	row.title            = model.title.bytes;
	row.title_id         = model.title_id.bytes;
	row.game_version     = LibraryGen::JoinVersion(model.game_version);
	row.firmware_version = LibraryGen::JoinVersion(model.firmware_version);
	row.size_bytes       = model.size_bytes;
	row.path             = model.path.bytes;
	row.status           = model.status;
	row.comment          = model.comment.bytes;
	return row;
}

} // namespace

KYTY_PROPERTY(8, "Column sorting") {
	return rc::check(name, []() {
		const auto           models = LibraryGen::GenerateRows();
		std::vector<GameRow> rows;
		for (const auto& model: models) {
			rows.push_back(RowOf(model));
		}
		RC_LOG() << rows.size() << " rows\n";
		for (const auto& row: rows) {
			RC_LOG() << "  '" << row.title << "' '" << row.title_id << "' '" << row.game_version
			         << "' '" << row.firmware_version << "' "
			         << (row.size_bytes ? std::to_string(*row.size_bytes) : std::string("-"))
			         << " '" << row.path << "' '" << row.status << "' '" << row.comment << "'\n";
		}

		for (const Column column: kColumns) {
			RC_LOG() << "column " << ColumnName(column) << "\n";
			// The comparator agrees with the reference order on every pair (so it is a total
			// preorder, antisymmetric, with every row equal to itself).
			for (size_t i = 0; i < rows.size(); i++) {
				for (size_t j = 0; j < rows.size(); j++) {
					RC_ASSERT(kyty::core::CompareForColumn(rows[i], rows[j], column) ==
					          ReferenceCompare(models[i], models[j], column));
				}
			}

			const auto ascending  = kyty::core::SortRows(rows, column, false);
			const auto descending = kyty::core::SortRows(rows, column, true);

			// A permutation of the input.
			auto sorted = ascending;
			std::sort(sorted.begin(), sorted.end());
			std::vector<size_t> identity(rows.size());
			std::iota(identity.begin(), identity.end(), size_t {0});
			RC_ASSERT(sorted == identity);

			// Every adjacent pair in order.
			for (size_t k = 1; k < ascending.size(); k++) {
				RC_ASSERT(kyty::core::CompareForColumn(rows[ascending[k - 1]], rows[ascending[k]],
				                                       column) <= 0);
				RC_ASSERT(kyty::core::CompareForColumn(rows[descending[k - 1]], rows[descending[k]],
				                                       column) >= 0);
			}

			// A stable sort by the reference order.
			auto expected = identity;
			std::stable_sort(expected.begin(), expected.end(), [&](size_t a, size_t b) {
				return ReferenceCompare(models[a], models[b], column) < 0;
			});
			RC_ASSERT(ascending == expected);

			// Descending is the exact reverse of ascending.
			RC_ASSERT(std::equal(descending.begin(), descending.end(), ascending.rbegin(),
			                     ascending.rend()));
		}

		bool ties = false;
		for (size_t i = 0; i + 1 < rows.size() && !ties; i++) {
			for (size_t j = i + 1; j < rows.size() && !ties; j++) {
				ties = kyty::core::CompareCaseInsensitive(rows[i].title, rows[j].title) == 0;
			}
		}
		RC_TAG(rows.size() < 2 ? "0-1 rows" : (ties ? "equal titles" : "distinct titles"));
	});
}
