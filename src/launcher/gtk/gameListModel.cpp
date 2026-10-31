#include "launcher/gtk/gameListModel.h"

#include "launcher/core/text.h"

#include <set>
#include <utility>

namespace kyty::gtk {

struct GameSortState {
	core::Column column     = core::Column::Title;
	bool         descending = false;
};

struct GameFilterState {
	std::string text;    // as set
	std::string trimmed; // what MatchesSearch is given
};

namespace {

// core::CompareForColumn, then the ordinal (scan order), inverted as a whole for descending: the
// order of core::SortRows (plan D27).
int CompareGames(gconstpointer a, gconstpointer b, gpointer data) {
	const auto* state  = static_cast<const GameSortState*>(data);
	auto*       game_a = KYTY_GAME(const_cast<gpointer>(a));
	auto*       game_b = KYTY_GAME(const_cast<gpointer>(b));
	int         result =
	    core::CompareForColumn(kyty_game_get_row(game_a), kyty_game_get_row(game_b), state->column);
	if (result == 0) {
		const guint ordinal_a = kyty_game_get_ordinal(game_a);
		const guint ordinal_b = kyty_game_get_ordinal(game_b);
		result                = ordinal_a < ordinal_b ? -1 : (ordinal_a > ordinal_b ? 1 : 0);
	}
	return state->descending ? -result : result;
}

gboolean MatchGame(gpointer item, gpointer data) {
	const auto* state = static_cast<const GameFilterState*>(data);
	return core::MatchesSearch(kyty_game_get_entry(KYTY_GAME(item)), state->trimmed) ? TRUE : FALSE;
}

// The game at position (holder keeps the reference that g_list_model_get_item returns).
KytyGame* GameAt(GListModel* model, guint position, GObjectPtr<KytyGame>& holder) {
	holder =
	    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(model, position)));
	return holder.get();
}

} // namespace

GameListModel::GameListModel() {
	m_store = GObjectPtr<GListStore>::Adopt(g_list_store_new(KYTY_TYPE_GAME));

	m_filter_state = new GameFilterState;
	m_filter       = GObjectPtr<GtkFilter>::Adopt(
	    GTK_FILTER(gtk_custom_filter_new(&MatchGame, m_filter_state, [](gpointer data) {
		    delete static_cast<GameFilterState*>(data);
	    })));
	// The list models take over the references they are given.
	m_filtered = GObjectPtr<GtkFilterListModel>::Adopt(gtk_filter_list_model_new(
	    G_LIST_MODEL(g_object_ref(m_store.get())), GTK_FILTER(g_object_ref(m_filter.get()))));

	m_sort_state = new GameSortState;
	m_sorter     = GObjectPtr<GtkSorter>::Adopt(
	    GTK_SORTER(gtk_custom_sorter_new(&CompareGames, m_sort_state, [](gpointer data) {
		    delete static_cast<GameSortState*>(data);
	    })));
	m_sorted = GObjectPtr<GtkSortListModel>::Adopt(gtk_sort_list_model_new(
	    G_LIST_MODEL(g_object_ref(m_filtered.get())), GTK_SORTER(g_object_ref(m_sorter.get()))));

	m_selection = GObjectPtr<GtkSingleSelection>::Adopt(
	    gtk_single_selection_new(G_LIST_MODEL(g_object_ref(m_sorted.get()))));
	gtk_single_selection_set_autoselect(m_selection.get(), FALSE);
	gtk_single_selection_set_can_unselect(m_selection.get(), TRUE);
	gtk_single_selection_set_selected(m_selection.get(), GTK_INVALID_LIST_POSITION);
}

GameListModel::~GameListModel() = default;

GListModel* GameListModel::Games() const {
	return G_LIST_MODEL(m_store.get());
}

GListModel* GameListModel::Filtered() const {
	return G_LIST_MODEL(m_filtered.get());
}

GListModel* GameListModel::Sorted() const {
	return G_LIST_MODEL(m_sorted.get());
}

GtkSingleSelection* GameListModel::Selection() const {
	return m_selection.get();
}

GtkSorter* GameListModel::Sorter() const {
	return m_sorter.get();
}

void GameListModel::Replace(std::vector<core::GameEntry> entries) {
	std::filesystem::path           selected_path;
	std::set<std::filesystem::path> running;
	if (KytyGame* selected = Selected(); selected != nullptr) {
		selected_path = kyty_game_get_entry(selected).path;
	}
	const guint old_count = g_list_model_get_n_items(Games());
	for (guint i = 0; i < old_count; i++) {
		GObjectPtr<KytyGame> holder;
		KytyGame*            game = GameAt(Games(), i, holder);
		if (kyty_game_get_running(game)) {
			running.insert(kyty_game_get_entry(game).path);
		}
	}

	std::vector<GObjectPtr<KytyGame>> games;
	std::vector<gpointer>             items;
	games.reserve(entries.size());
	items.reserve(entries.size());
	for (std::size_t i = 0; i < entries.size(); i++) {
		const bool was_running = running.count(entries[i].path) != 0;
		auto       game        = GObjectPtr<KytyGame>::Adopt(
		    kyty_game_new(std::move(entries[i]), static_cast<guint>(i)));
		if (was_running) {
			kyty_game_set_running(game.get(), true);
		}
		items.push_back(game.get());
		games.push_back(std::move(game));
	}
	g_list_store_splice(m_store.get(), 0, old_count, items.data(),
	                    static_cast<guint>(items.size()));
	if (!selected_path.empty()) {
		Select(selected_path);
	}
}

void GameListModel::SetSearchText(std::string_view text) {
	m_filter_state->text = std::string(text);
	std::string trimmed(core::TrimWhitespace(text));
	if (trimmed == m_filter_state->trimmed) {
		return;
	}
	m_filter_state->trimmed = std::move(trimmed);
	gtk_filter_changed(m_filter.get(), GTK_FILTER_CHANGE_DIFFERENT);
}

const std::string& GameListModel::SearchText() const {
	return m_filter_state->text;
}

void GameListModel::SetSort(core::Column column, bool descending) {
	if (m_sort_state->column == column && m_sort_state->descending == descending) {
		return;
	}
	m_sort_state->column     = column;
	m_sort_state->descending = descending;
	gtk_sorter_changed(m_sorter.get(), GTK_SORTER_CHANGE_DIFFERENT);
}

core::Column GameListModel::SortColumn() const {
	return m_sort_state->column;
}

bool GameListModel::SortDescending() const {
	return m_sort_state->descending;
}

void GameListModel::ColumnChanged(core::Column column) {
	if (column == m_sort_state->column) {
		gtk_sorter_changed(m_sorter.get(), GTK_SORTER_CHANGE_DIFFERENT);
	}
}

KytyGame* GameListModel::Selected() const {
	return static_cast<KytyGame*>(gtk_single_selection_get_selected_item(m_selection.get()));
}

bool GameListModel::Select(const std::filesystem::path& path) {
	const guint count = g_list_model_get_n_items(Sorted());
	for (guint i = 0; i < count; i++) {
		GObjectPtr<KytyGame> holder;
		if (kyty_game_get_entry(GameAt(Sorted(), i, holder)).path == path) {
			gtk_single_selection_set_selected(m_selection.get(), i);
			return true;
		}
	}
	return false;
}

KytyGame* GameListModel::Find(const std::filesystem::path& path) const {
	const guint count = g_list_model_get_n_items(Games());
	for (guint i = 0; i < count; i++) {
		GObjectPtr<KytyGame> holder;
		KytyGame*            game = GameAt(Games(), i, holder);
		if (kyty_game_get_entry(game).path == path) {
			// The store keeps its own reference.
			return game;
		}
	}
	return nullptr;
}

} // namespace kyty::gtk
