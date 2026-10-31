#include "launcher/gtk/gameViewSupport.h"

#include "launcher/gtk/i18n.h"

#include <algorithm>

namespace kyty::gtk {

namespace {

constexpr const char* kPlaceholderIcon = "applications-games-symbolic";

enum class Emblem { MetadataUnavailable, Running };

// Puts icon.image into a new overlay at the image's place in its parent box.
void EnsureOverlay(GameIcon& icon) {
	if (icon.overlay != nullptr) {
		return;
	}
	GtkWidget* box      = gtk_widget_get_parent(icon.image);
	GtkWidget* previous = gtk_widget_get_prev_sibling(icon.image);
	icon.overlay        = gtk_overlay_new();
	gtk_widget_set_halign(icon.overlay, gtk_widget_get_halign(icon.image));
	gtk_widget_set_valign(icon.overlay, gtk_widget_get_valign(icon.image));
	g_object_ref(icon.image);
	if (box != nullptr) {
		gtk_box_remove(GTK_BOX(box), icon.image);
	}
	gtk_overlay_set_child(GTK_OVERLAY(icon.overlay), icon.image);
	g_object_unref(icon.image);
	if (box != nullptr) {
		gtk_box_insert_child_after(GTK_BOX(box), icon.overlay, previous);
	}
}

// Shows or hides an emblem of icon, creating it (and the overlay) the first time it is shown.
void SetEmblem(GameIcon& icon, GtkWidget*& emblem, Emblem kind, bool show) {
	if (emblem == nullptr) {
		if (!show) {
			return;
		}
		EnsureOverlay(icon);
		const bool  metadata = kind == Emblem::MetadataUnavailable;
		const char* text     = _("The metadata is unavailable");
		if (!metadata) {
			// TRANSLATORS: the emblem of the game that is running.
			text = _("Running");
		}
		emblem = gtk_image_new_from_icon_name(metadata ? "dialog-warning-symbolic"
		                                               : "media-playback-start-symbolic");
		gtk_image_set_pixel_size(GTK_IMAGE(emblem), 16);
		gtk_widget_add_css_class(emblem, "game-emblem");
		gtk_widget_add_css_class(emblem, metadata ? "warning" : "accent");
		gtk_widget_set_halign(emblem, metadata ? GTK_ALIGN_END : GTK_ALIGN_START);
		gtk_widget_set_valign(emblem, metadata ? GTK_ALIGN_END : GTK_ALIGN_START);
		gtk_widget_set_tooltip_text(emblem, text);
		gtk_accessible_update_property(GTK_ACCESSIBLE(emblem), GTK_ACCESSIBLE_PROPERTY_LABEL, text,
		                               -1);
		gtk_overlay_add_overlay(GTK_OVERLAY(icon.overlay), emblem);
	}
	gtk_widget_set_visible(emblem, show ? TRUE : FALSE);
}

// The child at depth 1 or 2 of widget that is a tracked item (a click on a cell's or row's
// padding picks the cell or row widget, whose descendant is the item widget).
template <typename Map>
GtkWidget* TrackedBelow(GtkWidget* widget, const Map& items, int depth) {
	for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
	     child            = gtk_widget_get_next_sibling(child)) {
		if (items.count(child) != 0) {
			return child;
		}
		if (depth > 1) {
			if (GtkWidget* found = TrackedBelow(child, items, depth - 1); found != nullptr) {
				return found;
			}
		}
	}
	return nullptr;
}

// The position and the item of a GtkColumnViewCell or GtkListItem.
guint ItemPosition(GObject* item) {
	return GTK_IS_COLUMN_VIEW_CELL(item)
	           ? gtk_column_view_cell_get_position(GTK_COLUMN_VIEW_CELL(item))
	           : gtk_list_item_get_position(GTK_LIST_ITEM(item));
}

gpointer ItemObject(GObject* item) {
	return GTK_IS_COLUMN_VIEW_CELL(item) ? gtk_column_view_cell_get_item(GTK_COLUMN_VIEW_CELL(item))
	                                     : gtk_list_item_get_item(GTK_LIST_ITEM(item));
}

} // namespace

GObjectPtr<GMenuModel> LoadGameMenu() {
	auto builder = GObjectPtr<GtkBuilder>::Adopt(gtk_builder_new_from_resource(kGameMenuResource));
	return GObjectPtr<GMenuModel>::Ref(
	    G_MENU_MODEL(gtk_builder_get_object(builder.get(), "game-menu")));
}

GameIcon MakeGameIcon() {
	GameIcon icon;
	icon.image = gtk_image_new();
	gtk_widget_set_halign(icon.image, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(icon.image, GTK_ALIGN_CENTER);
	return icon;
}

void UpdateGameIcon(GameIcon& icon, KytyGame* game, int texture_size, int placeholder_size) {
	gtk_widget_set_size_request(icon.image, texture_size, texture_size);
	if (game == nullptr) {
		gtk_image_clear(GTK_IMAGE(icon.image));
		SetEmblem(icon, icon.metadata_emblem, Emblem::MetadataUnavailable, false);
		SetEmblem(icon, icon.running_emblem, Emblem::Running, false);
		return;
	}
	// Only a change of the shown image resets it (a reset reloads the icon).
	auto* image = GTK_IMAGE(icon.image);
	if (GdkTexture* texture = kyty_game_get_texture(game); texture != nullptr) {
		if (gtk_image_get_storage_type(image) != GTK_IMAGE_PAINTABLE ||
		    gtk_image_get_paintable(image) != GDK_PAINTABLE(texture)) {
			gtk_image_set_from_paintable(image, GDK_PAINTABLE(texture));
		}
		gtk_image_set_pixel_size(image, texture_size);
		gtk_widget_remove_css_class(icon.image, "dim-label");
	} else {
		if (gtk_image_get_storage_type(image) != GTK_IMAGE_ICON_NAME) {
			gtk_image_set_from_icon_name(image, kPlaceholderIcon);
		}
		gtk_image_set_pixel_size(image, placeholder_size);
		gtk_widget_add_css_class(icon.image, "dim-label");
	}
	SetEmblem(icon, icon.metadata_emblem, Emblem::MetadataUnavailable,
	          kyty_game_get_metadata_unavailable(game));
	SetEmblem(icon, icon.running_emblem, Emblem::Running, kyty_game_get_running(game));
}

GameViewShell::GameViewShell(GtkWidget* view, GtkSingleSelection* selection)
    : m_selection(GObjectPtr<GtkSingleSelection>::Ref(selection)), m_view(view) {
	m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(m_root, "game-view");
	GtkWidget* scrolled = gtk_scrolled_window_new();
	gtk_widget_set_hexpand(scrolled, TRUE);
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), m_view);
	gtk_box_append(GTK_BOX(m_root), scrolled);

	// The box's layout manager presents the popovers it parents.
	auto menu   = LoadGameMenu();
	m_game_menu = NewMenu(menu.get());
	gtk_widget_set_parent(m_game_menu, m_root);

	GtkGesture* click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(&OnPressed), this);
	gtk_widget_add_controller(m_view, GTK_EVENT_CONTROLLER(click));

	g_signal_connect(m_view, "activate", G_CALLBACK(&OnActivate), this);
	g_signal_connect(m_view, "map", G_CALLBACK(&OnMap), this);
	g_signal_connect(m_view, "unmap", G_CALLBACK(&OnUnmap), this);

	// One tooltip handler for every item (setting a tooltip text per item at every bind would
	// start a tooltip query each time).
	gtk_widget_set_has_tooltip(m_view, TRUE);
	g_signal_connect(m_view, "query-tooltip", G_CALLBACK(&OnQueryTooltip), this);

	GtkEventController* keys = gtk_shortcut_controller_new();
	gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys), GTK_SHORTCUT_SCOPE_LOCAL);
	const auto add = [keys](GtkShortcutTrigger* trigger, GtkShortcutAction* action) {
		gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
		                                     gtk_shortcut_new(trigger, action));
	};
	add(gtk_keyval_trigger_new(GDK_KEY_Delete, GdkModifierType(0)),
	    gtk_named_action_new("game.clear-config"));
	add(gtk_keyval_trigger_new(GDK_KEY_KP_Delete, GdkModifierType(0)),
	    gtk_named_action_new("game.clear-config"));
	add(gtk_keyval_trigger_new(GDK_KEY_F10, GDK_SHIFT_MASK),
	    gtk_callback_action_new(&OnMenuKey, this, nullptr));
	add(gtk_keyval_trigger_new(GDK_KEY_Menu, GdkModifierType(0)),
	    gtk_callback_action_new(&OnMenuKey, this, nullptr));
	gtk_widget_add_controller(m_view, keys);
}

GameViewShell::~GameViewShell() = default;

GtkWidget* GameViewShell::NewMenu(GMenuModel* model) {
	GtkWidget* menu = gtk_popover_menu_new_from_model(model);
	gtk_popover_set_has_arrow(GTK_POPOVER(menu), FALSE);
	gtk_popover_set_position(GTK_POPOVER(menu), GTK_POS_BOTTOM);
	gtk_widget_set_halign(menu, GTK_ALIGN_START);
	return menu;
}

void GameViewShell::TrackItem(GtkWidget* widget, GObject* item, bool anchor) {
	m_items[widget] = {item, anchor};
}

void GameViewShell::UntrackItem(GtkWidget* widget) {
	m_items.erase(widget);
}

GObject* GameViewShell::ItemAt(GtkWidget* picked, GtkWidget** tracked) const {
	for (GtkWidget* widget = picked; widget != nullptr && widget != m_root;
	     widget            = gtk_widget_get_parent(widget)) {
		GtkWidget* found = m_items.count(widget) != 0 ? widget : nullptr;
		if (found == nullptr && widget != m_view) {
			found = TrackedBelow(widget, m_items, 2);
		}
		if (found != nullptr) {
			if (tracked != nullptr) {
				*tracked = found;
			}
			return m_items.find(found)->second.item;
		}
		if (widget == m_view) {
			break;
		}
	}
	return nullptr;
}

gboolean GameViewShell::OnQueryTooltip(GtkWidget* view, int x, int y, gboolean keyboard,
                                       GtkTooltip* tooltip, gpointer data) {
	auto*    shell = static_cast<GameViewShell*>(data);
	gpointer game  = nullptr;
	if (keyboard != FALSE) {
		game = gtk_single_selection_get_selected_item(shell->m_selection.get());
	} else if (GtkWidget* picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
	           picked != nullptr) {
		GtkWidget* widget = nullptr;
		if (GObject* item = shell->ItemAt(picked, &widget); item != nullptr) {
			game = ItemObject(item);
			// The tooltip follows the pointer to the next item.
			graphene_rect_t bounds;
			if (gtk_widget_compute_bounds(widget, view, &bounds) != FALSE) {
				const GdkRectangle area {
				    static_cast<int>(bounds.origin.x), static_cast<int>(bounds.origin.y),
				    static_cast<int>(bounds.size.width), static_cast<int>(bounds.size.height)};
				gtk_tooltip_set_tip_area(tooltip, &area);
			}
		}
	}
	if (game == nullptr) {
		return FALSE;
	}
	gtk_tooltip_set_text(tooltip, GameTooltip(KYTY_GAME(game)).c_str());
	return TRUE;
}

void GameViewShell::Show(GtkWidget* menu, const graphene_rect_t& target) {
	const GdkRectangle rect {static_cast<int>(target.origin.x), static_cast<int>(target.origin.y),
	                         std::max(1, static_cast<int>(target.size.width)),
	                         std::max(1, static_cast<int>(target.size.height))};
	gtk_popover_set_pointing_to(GTK_POPOVER(menu), &rect);
	gtk_popover_popup(GTK_POPOVER(menu));
}

bool GameViewShell::PopupAt(double x, double y) {
	GtkWidget*             picked = gtk_widget_pick(m_view, x, y, GTK_PICK_DEFAULT);
	GObject*               item   = picked != nullptr ? ItemAt(picked) : nullptr;
	const graphene_point_t in {static_cast<float>(x), static_cast<float>(y)};
	graphene_point_t       out = in;
	if (gtk_widget_compute_point(m_view, m_root, &in, &out) == FALSE) {
		out = in;
	}
	const graphene_rect_t target {out, {1, 1}};
	if (item != nullptr) {
		const guint position = ItemPosition(item);
		if (position == GTK_INVALID_LIST_POSITION) {
			return false;
		}
		gtk_selection_model_select_item(GTK_SELECTION_MODEL(m_selection.get()), position, TRUE);
		Show(m_game_menu, target);
		return true;
	}
	if (m_empty_menu != nullptr) {
		Show(m_empty_menu, target);
		return true;
	}
	return false;
}

bool GameViewShell::PopupForSelection() {
	gpointer        selected = gtk_single_selection_get_selected_item(m_selection.get());
	graphene_rect_t target {{0, 0}, {1, 1}};
	if (selected == nullptr) {
		if (m_empty_menu == nullptr) {
			return false;
		}
		Show(m_empty_menu, target);
		return true;
	}
	for (const auto& [widget, tracked]: m_items) {
		if (tracked.anchor && ItemObject(tracked.item) == selected &&
		    gtk_widget_get_mapped(widget) != FALSE) {
			graphene_rect_t bounds;
			if (gtk_widget_compute_bounds(widget, m_root, &bounds) != FALSE) {
				target = bounds;
			}
			break;
		}
	}
	Show(m_game_menu, target);
	return true;
}

void GameViewShell::SetEmptySpaceMenu(GMenuModel* menu) {
	if (m_empty_menu != nullptr) {
		gtk_widget_unparent(m_empty_menu);
		m_empty_menu = nullptr;
	}
	if (menu != nullptr) {
		m_empty_menu = NewMenu(menu);
		gtk_widget_set_parent(m_empty_menu, m_root);
	}
}

void GameViewShell::OnPressed(GtkGestureClick* gesture, int /*presses*/, double x, double y,
                              gpointer data) {
	if (static_cast<GameViewShell*>(data)->PopupAt(x, y)) {
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	}
}

void GameViewShell::OnActivate(GtkWidget* view, guint position, gpointer data) {
	auto* shell = static_cast<GameViewShell*>(data);
	gtk_selection_model_select_item(GTK_SELECTION_MODEL(shell->m_selection.get()), position, TRUE);
	gtk_widget_activate_action(view, "game.run", nullptr);
}

gboolean GameViewShell::OnMenuKey(GtkWidget* /*widget*/, GVariant* /*args*/, gpointer data) {
	return static_cast<GameViewShell*>(data)->PopupForSelection() ? TRUE : FALSE;
}

void GameViewShell::OnMap(GtkWidget* view, gpointer data) {
	auto* shell = static_cast<GameViewShell*>(data);
	g_object_set(view, "model", shell->m_selection.get(), nullptr);
	// As the Qt launcher did when the view changed: the selected game is in sight.
	const guint selected = gtk_single_selection_get_selected(shell->m_selection.get());
	if (selected == GTK_INVALID_LIST_POSITION) {
		return;
	}
	if (GTK_IS_COLUMN_VIEW(view)) {
		gtk_column_view_scroll_to(GTK_COLUMN_VIEW(view), selected, nullptr, GTK_LIST_SCROLL_NONE,
		                          nullptr);
	} else if (GTK_IS_GRID_VIEW(view)) {
		gtk_grid_view_scroll_to(GTK_GRID_VIEW(view), selected, GTK_LIST_SCROLL_NONE, nullptr);
	}
}

void GameViewShell::OnUnmap(GtkWidget* view, gpointer /*data*/) {
	g_object_set(view, "model", nullptr, nullptr);
}

} // namespace kyty::gtk
