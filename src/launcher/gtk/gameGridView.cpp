#include "launcher/gtk/gameGridView.h"

#include "launcher/gtk/gameViewSupport.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace kyty::gtk {

namespace {

constexpr const char* kItemKey = "kyty-game-grid-item";

// The widgets of one grid item and the game it shows.
struct Item {
	GameGridViewState*   state = nullptr;
	GtkWidget*           root  = nullptr;
	GameIcon             icon;
	GtkWidget*           label = nullptr;
	GObjectPtr<KytyGame> game;
	SignalScope          notify;
};

} // namespace

struct GameGridViewState {
	std::unique_ptr<GameViewShell> shell;
	GtkGridView*                   view       = nullptr;
	int                            icon_width = GameGridView::kDefaultIconWidth;
	std::unordered_set<Item*>      items; // every item widget set, bound or not
};

namespace {

Item* ItemOf(GObject* object) {
	return static_cast<Item*>(g_object_get_data(object, kItemKey));
}

int IconHeight(int width) {
	return width * 9 / 16;
}

// The item's content is the icon width wide (the title's width request; the item's CSS padding
// comes on top); the icon, a height x height square, is centered above the title.
void Resize(Item& item) {
	const int width  = item.state->icon_width;
	const int height = IconHeight(width);
	gtk_widget_set_size_request(item.label, width, -1);
	UpdateGameIcon(item.icon, item.game.get(), height, height / 2);
}

void Refresh(Item& item) {
	KytyGame* game = item.game.get();
	if (game == nullptr) {
		return;
	}
	const std::string& title = kyty_game_get_entry(game).title_name;
	if (g_strcmp0(gtk_label_get_text(GTK_LABEL(item.label)), title.c_str()) != 0) {
		gtk_label_set_text(GTK_LABEL(item.label), title.c_str());
	}
	if (kyty_game_get_running(game)) {
		gtk_widget_add_css_class(item.label, "game-running");
	} else {
		gtk_widget_remove_css_class(item.label, "game-running");
	}
	Resize(item);
}

void OnGameNotify(GObject* /*game*/, GParamSpec* /*pspec*/, gpointer data) {
	Refresh(*static_cast<Item*>(data));
}

void OnSetup(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer data) {
	auto* state = static_cast<GameGridViewState*>(data);
	auto* item  = new Item;
	item->state = state;
	item->root  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(item->root, "game-grid-item");
	item->icon = MakeGameIcon();
	gtk_box_append(GTK_BOX(item->root), item->icon.image);
	// Two lines at most, wrapped to the item's width (its natural width is tiny, so its width
	// request, the icon width, sets the item's width).
	item->label = gtk_label_new(nullptr);
	gtk_label_set_wrap(GTK_LABEL(item->label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(item->label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_lines(GTK_LABEL(item->label), 2);
	gtk_label_set_ellipsize(GTK_LABEL(item->label), PANGO_ELLIPSIZE_END);
	gtk_label_set_justify(GTK_LABEL(item->label), GTK_JUSTIFY_CENTER);
	gtk_label_set_max_width_chars(GTK_LABEL(item->label), 1);
	gtk_widget_add_css_class(item->label, "game-title");
	gtk_box_append(GTK_BOX(item->root), item->label);
	auto* list_item = GTK_LIST_ITEM(object);
	gtk_list_item_set_child(list_item, item->root);
	state->items.insert(item);
	state->shell->TrackItem(item->root, object, true);
	g_object_set_data_full(object, kItemKey, item,
	                       [](gpointer data) { delete static_cast<Item*>(data); });
}

void OnBind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	auto* list_item = GTK_LIST_ITEM(object);
	Item* item      = ItemOf(object);
	item->game      = GObjectPtr<KytyGame>::Ref(KYTY_GAME(gtk_list_item_get_item(list_item)));
	item->notify =
	    SignalScope::Connect(item->game.get(), "notify", G_CALLBACK(&OnGameNotify), item);
	gtk_list_item_set_accessible_label(list_item,
	                                   kyty_game_get_entry(item->game.get()).title_name.c_str());
	Refresh(*item);
}

void OnUnbind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	Item* item = ItemOf(object);
	item->notify.Disconnect();
	item->game.reset();
	gtk_list_item_set_accessible_label(GTK_LIST_ITEM(object), "");
}

void OnTeardown(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer data) {
	auto* state = static_cast<GameGridViewState*>(data);
	if (Item* item = ItemOf(object); item != nullptr) {
		state->shell->UntrackItem(item->root);
		state->items.erase(item);
	}
	g_object_set_data(object, kItemKey, nullptr);
}

} // namespace

int GameGridView::SnapIconWidth(int width) {
	const int clamped = std::clamp(width, kMinIconWidth, kMaxIconWidth);
	const int steps   = (clamped - kMinIconWidth + kIconWidthStep / 2) / kIconWidthStep;
	return kMinIconWidth + steps * kIconWidthStep;
}

GameGridView::GameGridView(GameListModel& model) {
	auto* state = new GameGridViewState;

	GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(&OnSetup), state);
	g_signal_connect(factory, "bind", G_CALLBACK(&OnBind), state);
	g_signal_connect(factory, "unbind", G_CALLBACK(&OnUnbind), state);
	g_signal_connect(factory, "teardown", G_CALLBACK(&OnTeardown), state);
	// The view takes over the factory; the shell gives it the model while it is mapped.
	GtkWidget* view = gtk_grid_view_new(nullptr, factory);
	state->view     = GTK_GRID_VIEW(view);
	gtk_widget_add_css_class(view, "game-grid");
	gtk_grid_view_set_max_columns(state->view, 32);
	gtk_grid_view_set_tab_behavior(state->view, GTK_LIST_TAB_ITEM);
	// The shell exists before the view creates its first item widget (when it is realized).
	state->shell = std::make_unique<GameViewShell>(view, model.Selection());

	m_root  = GObjectPtr<GtkWidget>::Ref(state->shell->Root());
	m_state = state;
	g_object_set_data_full(G_OBJECT(m_root.get()), "kyty-game-grid-view", state,
	                       [](gpointer data) { delete static_cast<GameGridViewState*>(data); });
}

GameGridView::~GameGridView() = default;

GtkGridView* GameGridView::GridView() const {
	return m_state->view;
}

GtkPopover* GameGridView::GameMenu() const {
	return m_state->shell->GameMenu();
}

void GameGridView::SetIconWidth(int width) {
	const int snapped = SnapIconWidth(width);
	if (snapped == m_state->icon_width) {
		return;
	}
	m_state->icon_width = snapped;
	for (Item* item: m_state->items) {
		Resize(*item);
	}
}

int GameGridView::IconWidth() const {
	return m_state->icon_width;
}

void GameGridView::SetEmptySpaceMenu(GMenuModel* menu) {
	m_state->shell->SetEmptySpaceMenu(menu);
}

bool GameGridView::PopupMenuAt(double x, double y) {
	return m_state->shell->PopupAt(x, y);
}

bool GameGridView::PopupMenuForSelection() {
	return m_state->shell->PopupForSelection();
}

} // namespace kyty::gtk
