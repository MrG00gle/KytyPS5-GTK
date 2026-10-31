#pragma once

// The model of the game library (task 20.1; Requirements 7.3, 7.4, 7.11, 7.13; plan D27, D28):
//
//   GListStore of KytyGame (scan order)
//     -> GtkFilterListModel: core::MatchesSearch(entry, search text)
//     -> GtkSortListModel: one GtkCustomSorter that compares the rows with core::CompareForColumn
//        on the sort column, breaks ties by the ordinal, and inverts the whole comparison for
//        descending, so the order equals core::SortRows(rows, column, descending)
//     -> one GtkSingleSelection (no autoselect, may be empty) that the list and the grid view
//        share.
//
// Nothing is incremental: every change is applied before the call returns. The default sort is
// the title, ascending (LFI-06). The sort and search state lives in the sorter and the filter
// objects, so a view widget that still holds the chain keeps working after the model is destroyed;
// a GameListView object calls the model and must be destroyed first.

#include "launcher/core/gameLibrary.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"

#include <filesystem>
#include <gtk/gtk.h>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::gtk {

// The sort and search state, owned by the sorter and the filter object (gameListModel.cpp).
struct GameSortState;
struct GameFilterState;

class GameListModel {
public:
	GameListModel();
	~GameListModel();

	GameListModel(const GameListModel&)            = delete;
	GameListModel& operator=(const GameListModel&) = delete;

	// Every game in scan order (a GListStore of KytyGame).
	[[nodiscard]] GListModel* Games() const;
	// The games that match the search text, in scan order.
	[[nodiscard]] GListModel* Filtered() const;
	// The matching games in sort order.
	[[nodiscard]] GListModel* Sorted() const;
	// The selection over Sorted() that both views use.
	[[nodiscard]] GtkSingleSelection* Selection() const;
	// The sorter of Sorted(); it emits "changed" when the sort column or direction changes and on
	// ColumnChanged() for the sort column.
	[[nodiscard]] GtkSorter* Sorter() const;

	// Replaces every game with new KytyGame objects for entries (ordinal = index) in one
	// g_list_store_splice (plan D28). The sort column, the direction, and the search text stay
	// (Requirement 7.4); a game whose path was running is running again, and the selected game's
	// path is selected again when it is still listed and matches the search. During the splice the
	// selection is briefly empty.
	void Replace(std::vector<core::GameEntry> entries);

	// Filters by core::MatchesSearch (Requirement 7.13). Text that trims to the current trimmed
	// text changes nothing but SearchText().
	void                             SetSearchText(std::string_view text);
	[[nodiscard]] const std::string& SearchText() const;

	// Sorts by column (Requirement 7.12); does nothing when both are already set.
	void                       SetSort(core::Column column, bool descending);
	[[nodiscard]] core::Column SortColumn() const;
	[[nodiscard]] bool         SortDescending() const;

	// Sorts again after the caller changed the column's value (status, comment, size) in games;
	// does nothing unless column is the sort column.
	void ColumnChanged(core::Column column);

	// The selected game (transfer none), or nullptr.
	[[nodiscard]] KytyGame* Selected() const;
	// Selects the first game in sort order whose entry path is path; false when no listed game has
	// it (the selection is then unchanged).
	bool Select(const std::filesystem::path& path);
	// The first game in scan order whose entry path is path (transfer none), or nullptr.
	[[nodiscard]] KytyGame* Find(const std::filesystem::path& path) const;

private:
	GObjectPtr<GListStore>         m_store;
	GObjectPtr<GtkFilter>          m_filter;
	GObjectPtr<GtkFilterListModel> m_filtered;
	GObjectPtr<GtkSorter>          m_sorter;
	GObjectPtr<GtkSortListModel>   m_sorted;
	GObjectPtr<GtkSingleSelection> m_selection;
	GameFilterState*               m_filter_state = nullptr; // owned by m_filter
	GameSortState*                 m_sort_state   = nullptr; // owned by m_sorter
};

} // namespace kyty::gtk
