#include "launcher/gtk/gameListView.h"

#include "launcher/core/text.h"
#include "launcher/gtk/byteSize.h"
#include "launcher/gtk/gameCell.h"
#include "launcher/gtk/gameViewSupport.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace kyty::gtk {

namespace {

constexpr std::size_t kColumnCount     = 8;
constexpr int         kIconSize        = 48; // the Qt launcher's list icon size
constexpr int         kPlaceholderSize = 32;
constexpr const char* kCellKey         = "kyty-game-cell";
constexpr const char* kDash            = "\u2014"; // an unknown size or an empty version, as in Qt
static_assert(kIconSize <= kLargestGameIconSize, "the icons are scaled to the grid's largest");

// The Qt launcher's drop-down order.
constexpr core::CompatibilityStatus kStatusOrder[] = {
    core::CompatibilityStatus::Unknown, core::CompatibilityStatus::MainMenu,
    core::CompatibilityStatus::InGame, core::CompatibilityStatus::Logo,
    core::CompatibilityStatus::DoesntBoot};

constexpr const char* kStatusClasses[] = {"status-unknown", "status-main-menu", "status-in-game",
                                          "status-logo", "status-doesnt-boot"};

const char* StatusClass(core::CompatibilityStatus status) {
	for (std::size_t i = 0; i < std::size(kStatusOrder); i++) {
		if (kStatusOrder[i] == status) {
			return kStatusClasses[i];
		}
	}
	return kStatusClasses[0];
}

guint StatusIndex(core::CompatibilityStatus status) {
	for (std::size_t i = 0; i < std::size(kStatusOrder); i++) {
		if (kStatusOrder[i] == status) {
			return static_cast<guint>(i);
		}
	}
	return 0;
}

// 1024-based units, as the Qt launcher's QLocale::DataSizeTraditionalFormat.
std::string SizeText(std::optional<uint64_t> bytes) {
	if (!bytes) {
		return kDash;
	}
	return FormatByteSize(*bytes);
}

std::string OrDash(const std::string& value) {
	return value.empty() ? std::string(kDash) : value;
}

std::string Printf(const char* format, const std::string& value) {
	const GCharPtr text(g_strdup_printf(format, value.c_str()));
	return std::string(text.View());
}

int CompareColumn(gconstpointer a, gconstpointer b, gpointer data) {
	auto*     game_a = KYTY_GAME(const_cast<gpointer>(a));
	auto*     game_b = KYTY_GAME(const_cast<gpointer>(b));
	const int result = core::CompareForColumn(kyty_game_get_row(game_a), kyty_game_get_row(game_b),
	                                          static_cast<core::Column>(GPOINTER_TO_INT(data)));
	if (result != 0) {
		return result;
	}
	const guint ordinal_a = kyty_game_get_ordinal(game_a);
	const guint ordinal_b = kyty_game_get_ordinal(game_b);
	return ordinal_a < ordinal_b ? -1 : (ordinal_a > ordinal_b ? 1 : 0);
}

void SetText(GtkWidget* cell, const std::string& text) {
	kyty_game_cell_set_text(KYTY_GAME_CELL(cell), text);
}

// The status texts in the drop-down's order, null-terminated.
std::array<const char*, std::size(kStatusOrder) + 1> StatusTexts() {
	std::array<const char*, std::size(kStatusOrder) + 1> texts {};
	for (std::size_t i = 0; i < std::size(kStatusOrder); i++) {
		texts[i] = StatusText(kStatusOrder[i]);
	}
	return texts;
}

// The natural width of a status drop-down showing its widest translated status (plan D26): every
// --local drop-down asks for it, so none is clipped and the column is as wide in every row. A
// drop-down without a root has no item in its button yet and measures only its arrow, so the
// probe is measured inside a window that is never shown.
int StatusDropDownWidth() {
	auto       texts  = StatusTexts();
	GtkWidget* window = gtk_window_new();
	GtkWidget* probe  = gtk_drop_down_new_from_strings(texts.data());
	gtk_window_set_child(GTK_WINDOW(window), probe);
	int width = 0;
	for (guint i = 0; i < std::size(kStatusOrder); i++) {
		gtk_drop_down_set_selected(GTK_DROP_DOWN(probe), i);
		int natural = 0;
		gtk_widget_measure(probe, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural, nullptr,
		                   nullptr);
		width = std::max(width, natural);
	}
	gtk_window_destroy(GTK_WINDOW(window));
	return width;
}

} // namespace

struct GameListViewState {
	struct Spec {
		GameListViewState* state  = nullptr;
		core::Column       column = core::Column::Title;
	};

	GameListModel*                                 model    = nullptr;
	bool                                           editable = false;
	GameListView::Callbacks                        callbacks;
	std::unique_ptr<GameViewShell>                 shell;
	GtkColumnView*                                 view = nullptr;
	std::array<GtkColumnViewColumn*, kColumnCount> columns {};
	std::array<Spec, kColumnCount>                 specs {};
	// The factories' and the view sorter's connections, and the model sorter's (plan D25): gone
	// with the state, so a view that outlives it never calls into it.
	std::vector<SignalScope> signals;
	SignalScope              model_sorter_changed;
	bool                     syncing        = false;
	int                      dropdown_width = -1; // --local: StatusDropDownWidth(), once
};

namespace {

using Spec = GameListViewState::Spec;

// The widgets of one cell and the game it shows: a KytyGameCell (gameCell.h), or with --local the
// Status cell's indicator and drop-down or the Comment cell's entry.
struct Cell {
	Spec*                spec      = nullptr;
	GtkWidget*           root      = nullptr;
	GtkWidget*           drawn     = nullptr; // a KytyGameCell, unless the cell is an editor
	GtkWidget*           indicator = nullptr; // --local Status
	GtkWidget*           dropdown  = nullptr; // --local Status
	GtkWidget*           entry     = nullptr; // --local Comment
	GObjectPtr<KytyGame> game;
	SignalScope          notify;
	SignalScope          edit;
	SignalScope          leave;
	bool                 updating = false;
};

Cell* CellOf(GObject* object) {
	return static_cast<Cell*>(g_object_get_data(object, kCellKey));
}

void Refresh(Cell& cell, const char* property) {
	KytyGame* game = cell.game.get();
	if (game == nullptr) {
		return;
	}
	const core::GameEntry& entry        = kyty_game_get_entry(game);
	const core::GameRow&   row          = kyty_game_get_row(game);
	const bool             has_title_id = !core::TrimWhitespace(row.title_id).empty();
	cell.updating                       = true;
	switch (cell.spec->column) {
		case core::Column::Title:
			kyty_game_cell_set_icon(KYTY_GAME_CELL(cell.drawn), game, kIconSize, kPlaceholderSize);
			SetText(cell.drawn, entry.title_name);
			if (kyty_game_get_running(game)) {
				gtk_widget_add_css_class(cell.drawn, "game-running");
			} else {
				gtk_widget_remove_css_class(cell.drawn, "game-running");
			}
			break;
		case core::Column::TitleId: SetText(cell.drawn, row.title_id); break;
		case core::Column::GameVersion: SetText(cell.drawn, OrDash(row.game_version)); break;
		case core::Column::FirmwareVersion:
			SetText(cell.drawn, OrDash(row.firmware_version));
			break;
		case core::Column::Size: SetText(cell.drawn, SizeText(row.size_bytes)); break;
		case core::Column::Path: SetText(cell.drawn, row.path); break;
		case core::Column::Status: {
			const core::CompatibilityStatus status = kyty_game_get_status(game);
			if (cell.dropdown != nullptr) {
				for (const char* css_class: kStatusClasses) {
					gtk_widget_remove_css_class(cell.indicator, css_class);
				}
				gtk_widget_add_css_class(cell.indicator, StatusClass(status));
				gtk_drop_down_set_selected(GTK_DROP_DOWN(cell.dropdown), StatusIndex(status));
				gtk_widget_set_sensitive(cell.dropdown, has_title_id ? TRUE : FALSE);
				const std::string name = Printf(_("Compatibility status of %s"), entry.title_name);
				gtk_accessible_update_property(GTK_ACCESSIBLE(cell.dropdown),
				                               GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
			} else {
				kyty_game_cell_set_status_class(KYTY_GAME_CELL(cell.drawn), StatusClass(status));
				SetText(cell.drawn, StatusText(status));
			}
			break;
		}
		case core::Column::Comment:
			if (cell.entry != nullptr) {
				// Only a new game or a new comment replaces the text, so that a notification of
				// another property does not undo typing.
				if (property == nullptr || g_strcmp0(property, "comment") == 0) {
					gtk_editable_set_text(GTK_EDITABLE(cell.entry),
					                      kyty_game_get_comment(game).c_str());
				}
				// A game without a title ID has no entry in the compatibility list, so its comment
				// cannot be stored: the entry is dimmed and takes no input (plan D26).
				gtk_editable_set_editable(GTK_EDITABLE(cell.entry), has_title_id ? TRUE : FALSE);
				gtk_widget_set_sensitive(cell.entry, has_title_id ? TRUE : FALSE);
				const std::string name =
				    Printf(_("Compatibility comment for %s"), entry.title_name);
				gtk_accessible_update_property(GTK_ACCESSIBLE(cell.entry),
				                               GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
			} else {
				SetText(cell.drawn, kyty_game_get_comment(game));
			}
			break;
	}
	cell.updating = false;
}

void OnGameNotify(GObject* /*game*/, GParamSpec* pspec, gpointer data) {
	Refresh(*static_cast<Cell*>(data), g_param_spec_get_name(pspec));
}

void OnStatusSelected(GObject* /*dropdown*/, GParamSpec* /*pspec*/, gpointer data) {
	auto* cell = static_cast<Cell*>(data);
	if (cell->updating || !cell->game) {
		return;
	}
	const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(cell->dropdown));
	if (index >= std::size(kStatusOrder)) {
		return;
	}
	const core::CompatibilityStatus status = kStatusOrder[index];
	// The re-sort below may bind the cell to another game.
	const GObjectPtr<KytyGame> game  = cell->game;
	GameListViewState*         state = cell->spec->state;
	if (kyty_game_get_status(game.get()) == status) {
		return;
	}
	kyty_game_set_status(game.get(), status);
	if (state->callbacks.status_edited) {
		state->callbacks.status_edited(game.get(), status);
	}
	if (state->model != nullptr) {
		state->model->ColumnChanged(core::Column::Status);
	}
}

void CommitComment(Cell* cell) {
	if (cell->updating || !cell->game || cell->entry == nullptr ||
	    gtk_editable_get_editable(GTK_EDITABLE(cell->entry)) == FALSE) {
		return;
	}
	std::string                text  = gtk_editable_get_text(GTK_EDITABLE(cell->entry));
	const GObjectPtr<KytyGame> game  = cell->game;
	GameListViewState*         state = cell->spec->state;
	if (text == kyty_game_get_comment(game.get())) {
		return;
	}
	kyty_game_set_comment(game.get(), text);
	if (state->callbacks.comment_edited) {
		state->callbacks.comment_edited(game.get(), text);
	}
	if (state->model != nullptr) {
		state->model->ColumnChanged(core::Column::Comment);
	}
}

void OnCommentActivate(GtkEntry* /*entry*/, gpointer data) {
	CommitComment(static_cast<Cell*>(data));
}

void OnCommentLeave(GtkEventControllerFocus* /*controller*/, gpointer data) {
	CommitComment(static_cast<Cell*>(data));
}

// The --local Status cell: the indicator and a drop-down as wide as its widest status.
void SetupStatusEditor(GameListViewState& state, Cell& cell) {
	cell.root      = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, kGameCellStatusGap);
	cell.indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(cell.indicator, "game-status-indicator");
	gtk_widget_set_valign(cell.indicator, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(cell.root), cell.indicator);
	auto texts    = StatusTexts();
	cell.dropdown = gtk_drop_down_new_from_strings(texts.data());
	gtk_widget_set_valign(cell.dropdown, GTK_ALIGN_CENTER);
	if (state.dropdown_width < 0) {
		state.dropdown_width = StatusDropDownWidth();
	}
	gtk_widget_set_size_request(cell.dropdown, state.dropdown_width, -1);
	gtk_box_append(GTK_BOX(cell.root), cell.dropdown);
	cell.edit = SignalScope::Connect(cell.dropdown, "notify::selected",
	                                 G_CALLBACK(&OnStatusSelected), &cell);
}

void SetupCommentEditor(Cell& cell) {
	cell.entry = gtk_entry_new();
	gtk_widget_set_valign(cell.entry, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(cell.entry, TRUE);
	cell.root = cell.entry;
	cell.edit = SignalScope::Connect(cell.entry, "activate", G_CALLBACK(&OnCommentActivate), &cell);
	GtkEventController* focus = gtk_event_controller_focus_new();
	gtk_widget_add_controller(cell.entry, focus);
	cell.leave = SignalScope::Connect(focus, "leave", G_CALLBACK(&OnCommentLeave), &cell);
}

void OnSetup(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer data) {
	auto* spec  = static_cast<Spec*>(data);
	auto* cell  = new Cell;
	cell->spec  = spec;
	auto& state = *spec->state;
	switch (spec->column) {
		case core::Column::Title:
			cell->drawn = kyty_game_cell_new(GameCellKind::Title);
			gtk_widget_add_css_class(cell->drawn, "game-title");
			break;
		case core::Column::Status:
			if (state.editable) {
				SetupStatusEditor(state, *cell);
			} else {
				cell->drawn = kyty_game_cell_new(GameCellKind::Status);
			}
			break;
		case core::Column::Comment:
			if (state.editable) {
				SetupCommentEditor(*cell);
			} else {
				cell->drawn = kyty_game_cell_new(GameCellKind::Text);
			}
			break;
		case core::Column::Path: cell->drawn = kyty_game_cell_new(GameCellKind::Path); break;
		case core::Column::TitleId:
		case core::Column::GameVersion:
		case core::Column::FirmwareVersion:
		case core::Column::Size: cell->drawn = kyty_game_cell_new(GameCellKind::Text); break;
	}
	if (cell->drawn != nullptr) {
		// The column view gives the cell's child the whole cell.
		cell->root = cell->drawn;
	} else {
		gtk_widget_set_hexpand(cell->root, TRUE);
	}
	auto* column_cell = GTK_COLUMN_VIEW_CELL(object);
	gtk_column_view_cell_set_child(column_cell, cell->root);
	// The row takes the keyboard focus; only the editors inside a cell are focusable.
	gtk_column_view_cell_set_focusable(column_cell, FALSE);
	state.shell->TrackItem(cell->root, object, spec->column == core::Column::Title);
	g_object_set_data_full(object, kCellKey, cell,
	                       [](gpointer data) { delete static_cast<Cell*>(data); });
}

void OnBind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	Cell* cell = CellOf(object);
	cell->game = GObjectPtr<KytyGame>::Ref(
	    KYTY_GAME(gtk_column_view_cell_get_item(GTK_COLUMN_VIEW_CELL(object))));
	cell->notify =
	    SignalScope::Connect(cell->game.get(), "notify", G_CALLBACK(&OnGameNotify), cell);
	Refresh(*cell, nullptr);
}

void OnUnbind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	Cell* cell = CellOf(object);
	cell->notify.Disconnect();
	cell->game.reset();
}

void OnTeardown(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer data) {
	auto* spec = static_cast<Spec*>(data);
	if (Cell* cell = CellOf(object); cell != nullptr) {
		spec->state->shell->UntrackItem(cell->root);
	}
	g_object_set_data(object, kCellKey, nullptr);
}

void OnRowBind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	auto* row  = GTK_COLUMN_VIEW_ROW(object);
	auto* game = KYTY_GAME(gtk_column_view_row_get_item(row));
	gtk_column_view_row_set_accessible_label(row, kyty_game_get_entry(game).title_name.c_str());
}

void OnRowUnbind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	gtk_column_view_row_set_accessible_label(GTK_COLUMN_VIEW_ROW(object), "");
}

// Shows the model's sort in the headers.
void SyncHeader(GameListViewState& state) {
	if (state.model == nullptr) {
		return;
	}
	GtkColumnViewColumn* column =
	    state.columns[static_cast<std::size_t>(state.model->SortColumn())];
	const GtkSortType order =
	    state.model->SortDescending() ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING;
	auto* sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(state.view));
	if (gtk_column_view_sorter_get_primary_sort_column(sorter) == column &&
	    gtk_column_view_sorter_get_primary_sort_order(sorter) == order &&
	    gtk_column_view_sorter_get_n_sort_columns(sorter) == 1) {
		return;
	}
	// The model sorts by one column (ties by scan order), so the headers show only that one: GTK
	// keeps the previously clicked columns as secondary keys and shows their arrows too.
	state.syncing = true;
	gtk_column_view_sort_by_column(state.view, nullptr, GTK_SORT_ASCENDING);
	gtk_column_view_sort_by_column(state.view, column, order);
	state.syncing = false;
}

// A header click: the clicked column sorts ascending, the current one reverses (Requirement 7.12).
void OnViewSorterChanged(GtkSorter* sorter, GtkSorterChange /*change*/, gpointer data) {
	auto& state = *static_cast<GameListViewState*>(data);
	if (state.syncing || state.model == nullptr) {
		return;
	}
	auto*                view_sorter = GTK_COLUMN_VIEW_SORTER(sorter);
	GtkColumnViewColumn* primary     = gtk_column_view_sorter_get_primary_sort_column(view_sorter);
	for (std::size_t i = 0; i < kColumnCount; i++) {
		if (primary != nullptr && state.columns[i] == primary) {
			state.model->SetSort(static_cast<core::Column>(i),
			                     gtk_column_view_sorter_get_primary_sort_order(view_sorter) ==
			                         GTK_SORT_DESCENDING);
			break;
		}
	}
	// Unsorted (no header does that) or a secondary column left: show the model's sort again.
	SyncHeader(state);
}

void OnModelSorterChanged(GtkSorter* /*sorter*/, GtkSorterChange /*change*/, gpointer data) {
	SyncHeader(*static_cast<GameListViewState*>(data));
}

struct ColumnInfo {
	core::Column column;
	const char*  id;
	const char*  title;
	int          width;
};

} // namespace

GameListView::GameListView(GameListModel& model, bool editable, Callbacks callbacks) {
	auto* state      = new GameListViewState;
	state->model     = &model;
	state->editable  = editable;
	state->callbacks = std::move(callbacks);

	// The shell gives the view the model while it is mapped.
	GtkWidget* view = gtk_column_view_new(nullptr);
	state->view     = GTK_COLUMN_VIEW(view);
	gtk_widget_add_css_class(view, "game-list");
	gtk_column_view_set_tab_behavior(state->view, GTK_LIST_TAB_ITEM);
	state->shell = std::make_unique<GameViewShell>(view, model.Selection());

	auto& signals = state->signals;
	// The view takes over the factories; the state's connections to them end with the state.
	GtkListItemFactory* rows = gtk_signal_list_item_factory_new();
	signals.push_back(SignalScope::Connect(rows, "bind", G_CALLBACK(&OnRowBind), nullptr));
	signals.push_back(SignalScope::Connect(rows, "unbind", G_CALLBACK(&OnRowUnbind), nullptr));
	gtk_column_view_set_row_factory(state->view, rows);
	g_object_unref(rows);

	// The LFI-01 columns with the Qt launcher's widths; Comments takes the remaining width.
	const ColumnInfo columns[kColumnCount] = {
	    {core::Column::Title, "game", _("Game"), 320},
	    {core::Column::TitleId, "serial", _("Serial"), 110},
	    {core::Column::GameVersion, "game-version", _("Game Version"), 120},
	    {core::Column::FirmwareVersion, "firmware-version", _("Firmware Version"), 150},
	    {core::Column::Size, "size", _("Size"), 100},
	    {core::Column::Path, "path", _("Path"), 320},
	    {core::Column::Status, "status", _("Status"), 150},
	    {core::Column::Comment, "comments", _("Comments"), 240},
	};
	for (std::size_t i = 0; i < kColumnCount; i++) {
		const ColumnInfo& info      = columns[i];
		state->specs[i]             = {state, info.column};
		Spec*               spec    = &state->specs[i];
		GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
		signals.push_back(SignalScope::Connect(factory, "setup", G_CALLBACK(&OnSetup), spec));
		signals.push_back(SignalScope::Connect(factory, "bind", G_CALLBACK(&OnBind), spec));
		signals.push_back(SignalScope::Connect(factory, "unbind", G_CALLBACK(&OnUnbind), spec));
		signals.push_back(SignalScope::Connect(factory, "teardown", G_CALLBACK(&OnTeardown), spec));
		GtkColumnViewColumn* column = gtk_column_view_column_new(info.title, factory);
		gtk_column_view_column_set_id(column, info.id);
		// The header needs a sorter to sort on click; the model's own sorter does the sorting.
		GtkCustomSorter* sorter = gtk_custom_sorter_new(
		    &CompareColumn, GINT_TO_POINTER(static_cast<int>(info.column)), nullptr);
		gtk_column_view_column_set_sorter(column, GTK_SORTER(sorter));
		g_object_unref(sorter);
		// With --local the Status column fits its drop-downs (plan D26: the widest status was
		// clipped at 150 pixels); every drop-down asks for the same width.
		const bool sized = editable && info.column == core::Column::Status;
		gtk_column_view_column_set_fixed_width(column, sized ? -1 : info.width);
		gtk_column_view_column_set_resizable(column, TRUE);
		gtk_column_view_column_set_expand(column,
		                                  info.column == core::Column::Comment ? TRUE : FALSE);
		gtk_column_view_append_column(state->view, column);
		state->columns[i] = column;
		g_object_unref(column);
	}

	m_root  = GObjectPtr<GtkWidget>::Ref(state->shell->Root());
	m_state = state;
	g_object_set_data_full(G_OBJECT(m_root.get()), "kyty-game-list-view", state,
	                       [](gpointer data) { delete static_cast<GameListViewState*>(data); });

	signals.push_back(SignalScope::Connect(gtk_column_view_get_sorter(state->view), "changed",
	                                       G_CALLBACK(&OnViewSorterChanged), state));
	state->model_sorter_changed =
	    SignalScope::Connect(model.Sorter(), "changed", G_CALLBACK(&OnModelSorterChanged), state);
	SyncHeader(*state);
}

GameListView::~GameListView() {
	m_state->model_sorter_changed.Disconnect();
	m_state->model     = nullptr;
	m_state->callbacks = {};
}

GtkColumnView* GameListView::ColumnView() const {
	return m_state->view;
}

GtkPopover* GameListView::GameMenu() const {
	return m_state->shell->GameMenu();
}

void GameListView::SetEmptySpaceMenu(GMenuModel* menu) {
	m_state->shell->SetEmptySpaceMenu(menu);
}

bool GameListView::PopupMenuAt(double x, double y) {
	return m_state->shell->PopupAt(x, y);
}

bool GameListView::PopupMenuForSelection() {
	return m_state->shell->PopupForSelection();
}

} // namespace kyty::gtk
