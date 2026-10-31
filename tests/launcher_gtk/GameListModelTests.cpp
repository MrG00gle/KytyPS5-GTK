// Example tests of the game list model (src/launcher/gtk/gameListModel.h, task 20.1; plan D26-D28,
// HC-15): the GTK sort order equals core::SortRows for every column, ascending and descending, on
// a fixture with ties (equal titles in other cases, equal rows, unknown sizes); the filtered games
// equal core::FilterIndices; Replace keeps the sort column, the direction, and the search text,
// and carries the selection and the running flag over by path; ColumnChanged re-sorts; KytyGame
// keeps entry and row together and notifies only on changes. GTK's list models are plain GObjects:
// no gtk_init, no display.

#include "launcher/core/gameLibrary.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "support/noDisplay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <gtk/gtk.h>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using kyty::core::Column;
using kyty::core::CompatibilityStatus;
using kyty::core::GameEntry;
using kyty::core::GameRow;
using kyty::gtk::GameListModel;
using kyty::gtk::GObjectPtr;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameListModelTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

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

constexpr Column kColumns[] = {Column::Title,           Column::TitleId, Column::GameVersion,
                               Column::FirmwareVersion, Column::Size,    Column::Path,
                               Column::Status,          Column::Comment};

GameEntry Entry(const std::string& path, const std::string& title, const std::string& title_id,
                const std::string& game_version, const std::string& firmware,
                bool metadata = true) {
	GameEntry entry;
	entry.path               = path;
	entry.kind               = path.size() > 4 && path.substr(path.size() - 4) == ".zar"
	                               ? GameEntry::Kind::Archive
	                               : GameEntry::Kind::Directory;
	entry.title_name         = title;
	entry.title_id           = title_id;
	entry.game_version       = game_version;
	entry.firmware_version   = firmware;
	entry.metadata_available = metadata;
	return entry;
}

// Ties in every column: titles that differ only in case, equal title IDs and versions ("1.10"
// and "01.10" compare equal as versions), empty values, unknown and equal sizes, equal statuses
// and comments, and two pairs of entries with the same path (rows that compare 0, so only the
// ordinal orders them).
std::vector<GameEntry> Fixture() {
	return {
	    Entry("/games/b", "Beta", "PPSA00002", "1.10", "4.50"),
	    Entry("/games/a", "alpha", "PPSA00001", "1.9", "4.50"),
	    Entry("/games/c", "Alpha", "ppsa00001", "", "", false),
	    Entry("/games/d", "ALPHA", "PPSA00003", "01.10", "10.00"),
	    Entry("/games/e.zar", "Gamma", "", "2.0", "4.5"),
	    Entry("/games/a", "alpha", "PPSA00001", "1.9", "4.50"),
	    Entry("/games/f",
	          "\xc3\x9c"
	          "n\xc3\xaf"
	          "c\xc3\xb6"
	          "de",
	          "PPSA00004", "1.10", "4.50"),
	    Entry("/games/g", "beta", "PPSA00002", "1.10", "4.50"),
	    Entry("/games/h.zar", "Delta Archive", "PPSA00005", "1.0", ""),
	    Entry("/games/b", "Beta", "PPSA00002", "1.10", "4.50"),
	};
}

// The games of a list model (transfer none: the model keeps them).
std::vector<KytyGame*> Games(GListModel* model) {
	std::vector<KytyGame*> games;
	const guint            count = g_list_model_get_n_items(model);
	for (guint i = 0; i < count; i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(model, i)));
		games.push_back(game.get());
	}
	return games;
}

std::vector<size_t> Ordinals(GListModel* model) {
	std::vector<size_t> ordinals;
	for (KytyGame* game: Games(model)) {
		ordinals.push_back(kyty_game_get_ordinal(game));
	}
	return ordinals;
}

// The rows in scan order (store position = ordinal).
std::vector<GameRow> Rows(const GameListModel& model) {
	std::vector<GameRow> rows;
	for (KytyGame* game: Games(model.Games())) {
		rows.push_back(kyty_game_get_row(game));
	}
	return rows;
}

// The presentation values of the fixture: statuses, comments, and sizes with ties.
void Decorate(const GameListModel& model) {
	const std::vector<CompatibilityStatus> statuses = {
	    CompatibilityStatus::InGame,     CompatibilityStatus::MainMenu,
	    CompatibilityStatus::Unknown,    CompatibilityStatus::InGame,
	    CompatibilityStatus::Unknown,    CompatibilityStatus::MainMenu,
	    CompatibilityStatus::DoesntBoot, CompatibilityStatus::Logo,
	    CompatibilityStatus::Unknown,    CompatibilityStatus::InGame};
	const std::vector<std::string>             comments = {"good", "", "",  "Good", "z",
	                                                       "",     "", "a", "",     "good"};
	const std::vector<std::optional<uint64_t>> sizes    = {100, 5, std::nullopt, 100, std::nullopt,
	                                                       5,   0, 4096,         100, 100};
	const auto                                 games    = Games(model.Games());
	Check(games.size() == statuses.size(), "the fixture has ten games");
	for (size_t i = 0; i < games.size(); i++) {
		kyty_game_set_status(games[i], statuses[i]);
		kyty_game_set_comment(games[i], comments[i]);
		kyty_game_set_size(games[i], sizes[i]);
	}
}

std::string Describe(const std::vector<size_t>& order) {
	std::string text;
	for (size_t value: order) {
		text += (text.empty() ? "" : " ") + std::to_string(value);
	}
	return text;
}

void CheckSortEqualsSortRows(GameListModel& model, const std::vector<size_t>& visible) {
	const std::vector<GameRow> rows = Rows(model);
	const std::set<size_t>     shown(visible.begin(), visible.end());
	for (Column column: kColumns) {
		for (bool descending: {false, true}) {
			model.SetSort(column, descending);
			Check(model.SortColumn() == column && model.SortDescending() == descending,
			      "SetSort sets the column and the direction");
			std::vector<size_t> expected;
			for (size_t index: kyty::core::SortRows(rows, column, descending)) {
				if (shown.count(index) != 0) {
					expected.push_back(index);
				}
			}
			const std::vector<size_t> actual = Ordinals(model.Sorted());
			if (actual != expected) {
				std::fprintf(stderr, "%s %s: GTK [%s], SortRows [%s]\n", ColumnName(column),
				             descending ? "descending" : "ascending", Describe(actual).c_str(),
				             Describe(expected).c_str());
			}
			Check(actual == expected, std::string("the GTK order equals SortRows for ") +
			                              ColumnName(column) +
			                              (descending ? " descending" : " ascending"));
		}
	}
}

std::vector<size_t> AllIndices(size_t count) {
	std::vector<size_t> indices;
	for (size_t i = 0; i < count; i++) {
		indices.push_back(i);
	}
	return indices;
}

void TestGameObject() {
	const GameEntry entry = Entry("/games/x", "X Game", "PPSA09999", "01.000", "5.00", false);
	auto            game  = GObjectPtr<KytyGame>::Adopt(kyty_game_new(entry, 7));
	Check(kyty_game_get_entry(game.get()) == entry, "the game owns its entry");
	Check(kyty_game_get_ordinal(game.get()) == 7, "the ordinal is kept");
	GameRow expected = kyty::core::RowFromEntry(entry);
	expected.status  = kyty::gtk::StatusText(CompatibilityStatus::Unknown);
	Check(kyty_game_get_row(game.get()) == expected,
	      "the row is RowFromEntry with the Unknown status text");
	Check(kyty_game_get_metadata_unavailable(game.get()), "metadata unavailable follows the entry");
	Check(kyty_game_get_texture(game.get()) == nullptr && !kyty_game_get_size(game.get()) &&
	          !kyty_game_get_running(game.get()) && kyty_game_get_comment(game.get()).empty(),
	      "a new game has no texture, size, comment, or running flag");

	int notifications = 0;
	g_signal_connect(game.get(), "notify", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
		                 (*static_cast<int*>(data))++;
	                 }),
	                 &notifications);
	kyty_game_set_status(game.get(), CompatibilityStatus::InGame);
	kyty_game_set_status(game.get(), CompatibilityStatus::InGame);
	Check(notifications == 1, "a status change notifies once, an equal value not at all");
	Check(kyty_game_get_row(game.get()).status ==
	          kyty::gtk::StatusText(CompatibilityStatus::InGame),
	      "the row's status is the status text");
	kyty_game_set_comment(game.get(), "works");
	kyty_game_set_comment(game.get(), "works");
	Check(notifications == 2 && kyty_game_get_row(game.get()).comment == "works",
	      "the comment notifies once and updates the row");
	kyty_game_set_size(game.get(), 1234);
	kyty_game_set_size(game.get(), 1234);
	Check(notifications == 3 && kyty_game_get_row(game.get()).size_bytes == 1234u,
	      "the size notifies once and updates the row");
	guint64 size_property = 0;
	g_object_get(game.get(), "size", &size_property, nullptr);
	Check(size_property == 1234, "the size property");
	kyty_game_set_size(game.get(), std::nullopt);
	g_object_get(game.get(), "size", &size_property, nullptr);
	Check(notifications == 4 && size_property == G_MAXUINT64, "an unknown size reads G_MAXUINT64");
	kyty_game_set_running(game.get(), true);
	kyty_game_set_running(game.get(), true);
	Check(notifications == 5 && kyty_game_get_running(game.get()), "running notifies once");

	const guint8 pixel[4] = {255, 0, 0, 255};
	GBytes*      bytes    = g_bytes_new(pixel, sizeof(pixel));
	auto         texture =
	    GObjectPtr<GdkTexture>::Adopt(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4));
	g_bytes_unref(bytes);
	kyty_game_set_texture(game.get(), texture.get());
	kyty_game_set_texture(game.get(), texture.get());
	Check(notifications == 6 && kyty_game_get_texture(game.get()) == texture.get(),
	      "the texture notifies once");
	kyty_game_set_texture(game.get(), nullptr);
	Check(notifications == 7 && kyty_game_get_texture(game.get()) == nullptr,
	      "clearing the texture notifies");

	gchar*   title    = nullptr;
	gboolean metadata = FALSE;
	gint     status   = -1;
	g_object_get(game.get(), "title", &title, "metadata-unavailable", &metadata, "status", &status,
	             nullptr);
	Check(g_strcmp0(title, "X Game") == 0 && metadata == TRUE &&
	          status == static_cast<int>(CompatibilityStatus::InGame),
	      "the title, metadata-unavailable, and status properties");
	g_free(title);

	const std::string tooltip = kyty::gtk::GameTooltip(game.get());
	Check(tooltip.find("X Game\nSerial ID: PPSA09999\nGame version: 01.000\nFirmware version: "
	                   "5.00\nPath: /games/x\nComment: works\n") == 0,
	      "the tooltip has the Qt launcher's lines: " + tooltip);
	Check(tooltip.find("param.json") != std::string::npos,
	      "the tooltip says that the metadata is unavailable");
}

void TestSortEqualsSortRows() {
	GameListModel model;
	Check(model.SortColumn() == Column::Title && !model.SortDescending(),
	      "the default sort is the title, ascending");
	model.Replace(Fixture());
	Check(Ordinals(model.Games()) == AllIndices(10),
	      "the store keeps scan order (ordinal = index)");
	Decorate(model);
	// The decoration changed statuses, comments, and sizes: re-sort for the default column.
	model.ColumnChanged(Column::Title);
	CheckSortEqualsSortRows(model, AllIndices(10));

	// The equal rows (ordinals 1 and 5, 0 and 9) stay in scan order ascending and reverse it
	// descending.
	model.SetSort(Column::Path, false);
	std::vector<size_t> ascending = Ordinals(model.Sorted());
	model.SetSort(Column::Path, true);
	std::vector<size_t> descending = Ordinals(model.Sorted());
	const auto          position   = [](const std::vector<size_t>& order, size_t value) {
		for (size_t i = 0; i < order.size(); i++) {
			if (order[i] == value) {
				return i;
			}
		}
		return order.size();
	};
	Check(position(ascending, 1) < position(ascending, 5) &&
	          position(ascending, 0) < position(ascending, 9),
	      "equal rows keep scan order ascending");
	Check(position(descending, 5) < position(descending, 1) &&
	          position(descending, 9) < position(descending, 0),
	      "equal rows are reversed descending");
	Check(std::vector<size_t>(ascending.rbegin(), ascending.rend()) == descending,
	      "descending is the exact reverse of ascending");

	// The sorter emits "changed" for a new sort and for ColumnChanged of the sort column only.
	int changes = 0;
	g_signal_connect(model.Sorter(), "changed",
	                 G_CALLBACK(+[](GtkSorter*, GtkSorterChange, gpointer data) {
		                 (*static_cast<int*>(data))++;
	                 }),
	                 &changes);
	model.SetSort(Column::Path, true);
	Check(changes == 0, "the same sort changes nothing");
	model.SetSort(Column::Size, true);
	Check(changes == 1, "a new sort emits changed");
	model.ColumnChanged(Column::Status);
	Check(changes == 1, "ColumnChanged of another column does nothing");

	// ColumnChanged of the sort column follows changed values.
	const auto games = Games(model.Games());
	kyty_game_set_size(games[2], 1u << 20u);
	kyty_game_set_size(games[0], std::nullopt);
	model.ColumnChanged(Column::Size);
	Check(changes == 2, "ColumnChanged of the sort column emits changed");
	const std::vector<size_t> expected = kyty::core::SortRows(Rows(model), Column::Size, true);
	Check(Ordinals(model.Sorted()) == expected, "after ColumnChanged the order equals SortRows");
	CheckSortEqualsSortRows(model, AllIndices(10));
}

void TestFilterEqualsFilterIndices() {
	GameListModel model;
	const auto    entries = Fixture();
	model.Replace(entries);
	Decorate(model);
	int filter_changes = 0;
	g_signal_connect(gtk_filter_list_model_get_filter(GTK_FILTER_LIST_MODEL(model.Filtered())),
	                 "changed", G_CALLBACK(+[](GtkFilter*, GtkFilterChange, gpointer data) {
		                 (*static_cast<int*>(data))++;
	                 }),
	                 &filter_changes);
	const std::vector<std::string> queries = {"",
	                                          "   ",
	                                          "alpha",
	                                          " ALPHA ",
	                                          "ppsa0000",
	                                          "PPSA00001",
	                                          "\xc3\xbc"
	                                          "n\xc3\xaf",
	                                          "zzz",
	                                          "a",
	                                          "1",
	                                          "\t beta\n",
	                                          "archive"};
	for (const auto& query: queries) {
		model.SetSearchText(query);
		Check(model.SearchText() == query, "SearchText returns the text as set");
		const std::vector<size_t> expected = kyty::core::FilterIndices(entries, query);
		Check(Ordinals(model.Filtered()) == expected,
		      "the filtered games equal FilterIndices for \"" + query + "\"");
		const auto             sorted = Ordinals(model.Sorted());
		const std::set<size_t> sorted_set(sorted.begin(), sorted.end());
		Check(sorted_set == std::set<size_t>(expected.begin(), expected.end()) &&
		          sorted.size() == expected.size(),
		      "the sorted model shows the same games for \"" + query + "\"");
	}
	model.SetSearchText("alpha");
	const int before = filter_changes;
	model.SetSearchText("  alpha ");
	Check(filter_changes == before && model.SearchText() == "  alpha ",
	      "a text that trims to the same text does not filter again");
	CheckSortEqualsSortRows(model, kyty::core::FilterIndices(entries, "alpha"));

	// The selection: a filtered-out selected game is unselected (as in Qt).
	Check(model.Select("/games/d"), "a shown game can be selected");
	Check(model.Selected() != nullptr && kyty_game_get_entry(model.Selected()).path == "/games/d",
	      "Selected returns it");
	model.SetSearchText("beta");
	Check(model.Selected() == nullptr, "filtering the selected game out clears the selection");
	Check(!model.Select("/games/d"), "a game that is filtered out cannot be selected");
	model.SetSearchText("");
	Check(model.Find("/games/d") != nullptr && model.Find("/games/zz") == nullptr,
	      "Find looks in every game");
}

void TestReplaceKeepsState() {
	GameListModel model;
	model.Replace(Fixture());
	Decorate(model);
	model.SetSort(Column::Size, true);
	model.SetSearchText(" alpha");
	Check(model.Select("/games/d"), "the game to keep is selected");
	kyty_game_set_running(model.Find("/games/a"), true);
	kyty_game_set_running(model.Find("/games/c"), true);
	KytyGame* old_d = model.Find("/games/d");

	// A rescan: /games/c is gone, /games/a and /games/d changed, two new games.
	std::vector<GameEntry> rescan = {
	    Entry("/games/n", "Alpha New", "PPSA00010", "1.0", "4.50"),
	    Entry("/games/d", "ALPHA", "PPSA00003", "01.20", "10.00"),
	    Entry("/games/a", "alpha renamed", "PPSA00001", "1.9", "4.50"),
	    Entry("/games/b", "Beta", "PPSA00002", "1.10", "4.50"),
	    Entry("/games/m", "alpha minor", "PPSA00011", "", ""),
	};
	model.Replace(rescan);
	Check(model.SortColumn() == Column::Size && model.SortDescending(),
	      "Replace keeps the sort column and the direction");
	Check(model.SearchText() == " alpha", "Replace keeps the search text");
	Check(Ordinals(model.Games()) == AllIndices(rescan.size()), "the new games have new ordinals");
	Check(model.Find("/games/d") != old_d, "Replace creates new game objects");
	Check(Ordinals(model.Filtered()) == kyty::core::FilterIndices(rescan, " alpha"),
	      "the search applies to the new games");
	CheckSortEqualsSortRows(model, kyty::core::FilterIndices(rescan, " alpha"));
	model.SetSort(Column::Size, true);
	Check(model.Selected() != nullptr && kyty_game_get_entry(model.Selected()).path == "/games/d",
	      "the selected path is selected again");
	Check(kyty_game_get_running(model.Find("/games/a")), "a running path stays running");
	Check(!kyty_game_get_running(model.Find("/games/d")) &&
	          !kyty_game_get_running(model.Find("/games/n")),
	      "other games are not running");
	Check(kyty_game_get_entry(model.Find("/games/a")).title_name == "alpha renamed",
	      "the new entry replaces the old one");

	model.Replace({Entry("/games/b", "Beta", "PPSA00002", "1.10", "4.50")});
	Check(model.Selected() == nullptr, "a selected path that is gone leaves no selection");
	Check(g_list_model_get_n_items(model.Sorted()) == 0, "\"alpha\" matches nothing now");
	model.SetSearchText("");
	Check(g_list_model_get_n_items(model.Sorted()) == 1, "clearing the search shows the game");
	model.Replace({});
	Check(g_list_model_get_n_items(model.Games()) == 0 && model.Selected() == nullptr,
	      "an empty rescan empties the list");
}

void TestSelectionModel() {
	GameListModel model;
	Check(gtk_single_selection_get_model(model.Selection()) == model.Sorted(),
	      "the selection is over the sorted model");
	Check(gtk_sort_list_model_get_model(GTK_SORT_LIST_MODEL(model.Sorted())) == model.Filtered() &&
	          gtk_filter_list_model_get_model(GTK_FILTER_LIST_MODEL(model.Filtered())) ==
	              model.Games(),
	      "the chain is store, filter, sort");
	Check(gtk_sort_list_model_get_sorter(GTK_SORT_LIST_MODEL(model.Sorted())) == model.Sorter(),
	      "the sorted model uses Sorter()");
	Check(gtk_single_selection_get_autoselect(model.Selection()) == FALSE &&
	          gtk_single_selection_get_can_unselect(model.Selection()) == TRUE,
	      "no autoselect, and the selection may be empty");
	model.Replace(Fixture());
	Check(model.Selected() == nullptr, "nothing is selected after the first scan");
}

// Informational (the gate's limit is checked with the views in the harness): the time of a search
// text change over 1,000 games, until the filter and sort models have emitted items-changed.
void TestSearchTiming() {
	GameListModel          model;
	std::vector<GameEntry> entries;
	for (int i = 0; i < 1000; i++) {
		char name[64];
		std::snprintf(name, sizeof(name), "Synthetic Game %04d", i);
		char id[16];
		std::snprintf(id, sizeof(id), "PPSA2%04d", i);
		entries.push_back(Entry(std::string("/synthetic/") + name, name, id, "01.000", "4.50"));
	}
	model.Replace(entries);
	double worst = 0;
	for (const char* query: {"synthetic", "game 05", "0999", "PPSA2", "", "no such game", "a"}) {
		const auto start = std::chrono::steady_clock::now();
		model.SetSearchText(query);
		const double ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
		        .count();
		worst = ms > worst ? ms : worst;
		Check(Ordinals(model.Filtered()) == kyty::core::FilterIndices(entries, query),
		      std::string("the 1,000-game filter equals FilterIndices for ") + query);
	}
	std::printf("GameListModelTests: search over 1000 games: worst %.2f ms (model only)\n", worst);
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameListModelTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestGameObject();
	TestSortEqualsSortRows();
	TestFilterEqualsFilterIndices();
	TestReplaceKeepsState();
	TestSelectionModel();
	TestSearchTiming();
	std::printf("GameListModelTests: all tests passed\n");
	return 0;
}
