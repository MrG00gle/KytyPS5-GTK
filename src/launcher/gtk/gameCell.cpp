#include "launcher/gtk/gameCell.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <pango/pangocairo.h>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using kyty::gtk::GameCellKind;
using kyty::gtk::GObjectPtr;

constexpr const char* kPlaceholderIcon = "applications-games-symbolic";
// The opacity of the dim-label style class that the placeholder image had (libadwaita's
// --dim-opacity, 55 %).
constexpr float kDimOpacity     = 0.55F;
constexpr int   kEmblemIconSize = 16;
constexpr int   kTileLines      = 2;

struct CellState {
	GameCellKind           kind = GameCellKind::Text;
	std::string            text;
	PangoLayout*           layout = nullptr; // owned; made on first use
	bool                   icon   = false;   // Title and Tile: an icon (texture or placeholder)
	GObjectPtr<GdkTexture> texture;
	int                    icon_size        = 0;
	int                    placeholder_size = 0;
	int                    tile_width       = 0;
	GtkWidget*             indicator        = nullptr; // Status: child
	GtkWidget*             metadata_emblem  = nullptr; // child, made when needed
	GtkWidget*             running_emblem   = nullptr; // child, made when needed
	// Tile: the title's width and height on one line, in the font of text_serial (GTK measures a
	// grid item many times, at several widths: the title is shaped once).
	bool  text_measured = false;
	guint text_serial   = 0;
	int   text_width    = 0;
	int   text_height   = 0;
	// From the last allocation.
	bool            allocated = false;
	graphene_rect_t square {};    // the icon square
	graphene_rect_t icon_rect {}; // the drawn texture or placeholder glyph
	graphene_rect_t text_rect {};
};

// What is measured per font and resolution (main thread only): every cell of a view shares a few
// fonts, and a filter change measures every realized cell again, about 1,600 list cells or 150
// grid tiles, each of them new.
struct FontMetrics {
	PangoFontDescription* font        = nullptr; // owned
	double                resolution  = 0;
	int                   line_height = 0;
	// The one-line size of the titles measured in this font (a tile's title is measured each time
	// a tile is made for it), at most kMaxMeasuredTexts.
	std::unordered_map<std::string, std::pair<int, int>> texts;
};

constexpr std::size_t kMaxFonts         = 16;
constexpr std::size_t kMaxMeasuredTexts = 8192;

FontMetrics& MetricsFor(PangoContext* context) {
	static std::vector<std::unique_ptr<FontMetrics>> cache;
	const PangoFontDescription* font       = pango_context_get_font_description(context);
	const double                resolution = pango_cairo_context_get_resolution(context);
	for (const auto& entry: cache) {
		if (entry->resolution == resolution && pango_font_description_equal(entry->font, font)) {
			return *entry;
		}
	}
	if (cache.size() >= kMaxFonts) {
		pango_font_description_free(cache.front()->font);
		cache.erase(cache.begin());
	}
	auto entry        = std::make_unique<FontMetrics>();
	entry->font       = pango_font_description_copy(font);
	entry->resolution = resolution;
	// The height of one line of text.
	PangoLayout* layout = pango_layout_new(context);
	pango_layout_set_text(layout, "Ag", -1);
	pango_layout_get_pixel_size(layout, nullptr, &entry->line_height);
	g_object_unref(layout);
	cache.push_back(std::move(entry));
	return *cache.back();
}

int LineHeight(PangoContext* context) {
	return MetricsFor(context).line_height;
}

bool SingleLine(GameCellKind kind) {
	return kind != GameCellKind::Tile;
}

bool HasIcon(GameCellKind kind) {
	return kind == GameCellKind::Title || kind == GameCellKind::Tile;
}

} // namespace

struct _KytyGameCell {
	GtkWidget  parent_instance;
	CellState* state;
};

static void kyty_game_cell_accessible_text_init(GtkAccessibleTextInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(KytyGameCell, kyty_game_cell, GTK_TYPE_WIDGET,
                              G_IMPLEMENT_INTERFACE(GTK_TYPE_ACCESSIBLE_TEXT,
                                                    kyty_game_cell_accessible_text_init))

namespace {

CellState& StateOf(GtkWidget* widget) {
	return *KYTY_GAME_CELL(widget)->state;
}

PangoLayout* LayoutOf(GtkWidget* widget) {
	CellState& state = StateOf(widget);
	if (state.layout == nullptr) {
		state.layout = gtk_widget_create_pango_layout(widget, state.text.c_str());
		if (SingleLine(state.kind)) {
			pango_layout_set_single_paragraph_mode(state.layout, TRUE);
			pango_layout_set_ellipsize(state.layout, state.kind == GameCellKind::Path
			                                             ? PANGO_ELLIPSIZE_MIDDLE
			                                             : PANGO_ELLIPSIZE_END);
		} else {
			pango_layout_set_wrap(state.layout, PANGO_WRAP_WORD_CHAR);
			pango_layout_set_ellipsize(state.layout, PANGO_ELLIPSIZE_END);
			pango_layout_set_height(state.layout, -kTileLines);
			pango_layout_set_alignment(state.layout, PANGO_ALIGN_CENTER);
		}
	}
	return state.layout;
}

// The height of the Tile's title at width: one line when the title fits it, else kTileLines (the
// wrapped title is ellipsized at the end of its last line). The title is laid out on one line once
// per text and font.
int TileTextHeight(GtkWidget* widget, int width) {
	CellState&    state   = StateOf(widget);
	PangoContext* context = gtk_widget_get_pango_context(widget);
	if (!state.text_measured || state.text_serial != pango_context_get_serial(context)) {
		FontMetrics& metrics = MetricsFor(context);
		if (const auto found = metrics.texts.find(state.text); found != metrics.texts.end()) {
			std::tie(state.text_width, state.text_height) = found->second;
		} else {
			PangoLayout* layout = LayoutOf(widget);
			pango_layout_set_width(layout, -1);
			pango_layout_get_pixel_size(layout, &state.text_width, &state.text_height);
			state.text_height = std::max(state.text_height, metrics.line_height);
			if (metrics.texts.size() >= kMaxMeasuredTexts) {
				metrics.texts.clear();
			}
			metrics.texts.emplace(state.text, std::pair(state.text_width, state.text_height));
		}
		state.text_serial   = pango_context_get_serial(context);
		state.text_measured = true;
	}
	return (state.text_width <= width ? 1 : kTileLines) * state.text_height;
}

// An emblem of the icon, created the first time it is shown.
void SetEmblem(GtkWidget* cell, GtkWidget*& emblem, bool metadata, bool show) {
	if (emblem == nullptr) {
		if (!show) {
			return;
		}
		const char* text = _("The metadata is unavailable");
		if (!metadata) {
			// TRANSLATORS: the emblem of the game that is running.
			text = _("Running");
		}
		emblem = gtk_image_new_from_icon_name(metadata ? "dialog-warning-symbolic"
		                                               : "media-playback-start-symbolic");
		gtk_image_set_pixel_size(GTK_IMAGE(emblem), kEmblemIconSize);
		gtk_widget_add_css_class(emblem, "game-emblem");
		gtk_widget_add_css_class(emblem, metadata ? "warning" : "accent");
		gtk_widget_set_tooltip_text(emblem, text);
		gtk_accessible_update_property(GTK_ACCESSIBLE(emblem), GTK_ACCESSIBLE_PROPERTY_LABEL, text,
		                               -1);
		gtk_widget_set_parent(emblem, cell);
	}
	if ((gtk_widget_get_visible(emblem) != FALSE) != show) {
		gtk_widget_set_visible(emblem, show ? TRUE : FALSE);
	}
}

// Allocates emblem centered on the corner (x, y) of the drawn icon, kept inside the icon square.
void AllocateEmblem(GtkWidget* emblem, const graphene_rect_t& square, float x, float y) {
	if (emblem == nullptr || gtk_widget_get_visible(emblem) == FALSE) {
		return;
	}
	int width  = 0;
	int height = 0;
	gtk_widget_measure(emblem, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &width, nullptr, nullptr);
	gtk_widget_measure(emblem, GTK_ORIENTATION_VERTICAL, width, nullptr, &height, nullptr, nullptr);
	const float left = std::clamp(
	    x - static_cast<float>(width) / 2.0F, square.origin.x,
	    std::max(square.origin.x, square.origin.x + square.size.width - static_cast<float>(width)));
	const float top = std::clamp(y - static_cast<float>(height) / 2.0F, square.origin.y,
	                             std::max(square.origin.y, square.origin.y + square.size.height -
	                                                           static_cast<float>(height)));
	const GtkAllocation allocation {static_cast<int>(left), static_cast<int>(top), width, height};
	gtk_widget_size_allocate(emblem, &allocation, -1);
}

void Measure(GtkWidget* widget, GtkOrientation orientation, int for_size, int* minimum,
             int* natural, int* minimum_baseline, int* natural_baseline) {
	CellState& state = StateOf(widget);
	int        size  = 0;
	if (orientation == GTK_ORIENTATION_HORIZONTAL) {
		switch (state.kind) {
			case GameCellKind::Text:
			case GameCellKind::Path: break;
			case GameCellKind::Title: size = state.icon_size + kyty::gtk::kGameCellTitleGap; break;
			case GameCellKind::Status:
				if (state.indicator != nullptr) {
					gtk_widget_measure(state.indicator, GTK_ORIENTATION_HORIZONTAL, -1, nullptr,
					                   &size, nullptr, nullptr);
				}
				size += kyty::gtk::kGameCellStatusGap;
				break;
			case GameCellKind::Tile: size = state.tile_width; break;
		}
	} else if (state.kind == GameCellKind::Tile) {
		const int width = for_size >= 0 ? for_size : state.tile_width;
		size = state.icon_size + kyty::gtk::kGameCellTileGap + TileTextHeight(widget, width);
	} else {
		// One line in the cell's font. The text cells of a list inherit the list's font (the
		// views' CSS sets no font on them), so they measure it with the list's Pango context
		// instead of making one each (a search measures every realized cell, about 1,600); the
		// Game cell, whose title is bold while the game runs, uses its own.
		GtkWidget* font_widget = widget;
		if (state.kind != GameCellKind::Title) {
			if (GtkWidget* list = gtk_widget_get_ancestor(widget, GTK_TYPE_COLUMN_VIEW);
			    list != nullptr) {
				font_widget = list;
			}
		}
		size = LineHeight(gtk_widget_get_pango_context(font_widget));
		if (state.kind == GameCellKind::Title) {
			size = std::max(size, state.icon_size);
		} else if (state.kind == GameCellKind::Status && state.indicator != nullptr) {
			int indicator = 0;
			gtk_widget_measure(state.indicator, GTK_ORIENTATION_VERTICAL, -1, nullptr, &indicator,
			                   nullptr, nullptr);
			size = std::max(size, indicator);
		}
	}
	*minimum          = size;
	*natural          = size;
	*minimum_baseline = -1;
	*natural_baseline = -1;
}

GtkSizeRequestMode RequestMode(GtkWidget* widget) {
	return StateOf(widget).kind == GameCellKind::Tile ? GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH
	                                                  : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

void SizeAllocate(GtkWidget* widget, int width, int height, int /*baseline*/) {
	CellState& state = StateOf(widget);
	const bool rtl   = gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL;
	const auto w     = static_cast<float>(width);
	const auto h     = static_cast<float>(height);
	const auto icon  = static_cast<float>(state.icon_size);
	state.allocated  = true;
	switch (state.kind) {
		case GameCellKind::Text:
		case GameCellKind::Path: state.text_rect = GRAPHENE_RECT_INIT(0, 0, w, h); break;
		case GameCellKind::Title: {
			const float gap = icon + static_cast<float>(kyty::gtk::kGameCellTitleGap);
			state.square    = GRAPHENE_RECT_INIT(rtl ? w - icon : 0, (h - icon) / 2, icon, icon);
			state.text_rect = GRAPHENE_RECT_INIT(rtl ? 0 : gap, 0, std::max(0.0F, w - gap), h);
			break;
		}
		case GameCellKind::Status: {
			float text_x = static_cast<float>(kyty::gtk::kGameCellStatusGap);
			if (state.indicator != nullptr) {
				int indicator_w = 0;
				int indicator_h = 0;
				gtk_widget_measure(state.indicator, GTK_ORIENTATION_HORIZONTAL, -1, nullptr,
				                   &indicator_w, nullptr, nullptr);
				gtk_widget_measure(state.indicator, GTK_ORIENTATION_VERTICAL, indicator_w, nullptr,
				                   &indicator_h, nullptr, nullptr);
				const GtkAllocation allocation {rtl ? width - indicator_w : 0,
				                                (height - indicator_h) / 2, indicator_w,
				                                indicator_h};
				gtk_widget_size_allocate(state.indicator, &allocation, -1);
				text_x += static_cast<float>(indicator_w);
			}
			state.text_rect =
			    GRAPHENE_RECT_INIT(rtl ? 0 : text_x, 0, std::max(0.0F, w - text_x), h);
			break;
		}
		case GameCellKind::Tile: {
			const float top = icon + static_cast<float>(kyty::gtk::kGameCellTileGap);
			state.square    = GRAPHENE_RECT_INIT((w - icon) / 2, 0, icon, icon);
			state.text_rect = GRAPHENE_RECT_INIT(0, top, w, std::max(0.0F, h - top));
			break;
		}
	}
	if (!HasIcon(state.kind)) {
		return;
	}
	// The drawn icon: the texture fitted into the square (its aspect ratio kept), or the
	// placeholder glyph in its middle.
	if (state.texture) {
		// As GtkImage draws a paintable at its pixel size: the longer side is the square's.
		const auto texture_w = static_cast<float>(gdk_texture_get_width(state.texture.get()));
		const auto texture_h = static_cast<float>(gdk_texture_get_height(state.texture.get()));
		float      fit_w     = icon;
		float      fit_h     = icon;
		if (texture_w > texture_h && texture_w > 0) {
			fit_h = icon * texture_h / texture_w;
		} else if (texture_h > texture_w && texture_h > 0) {
			fit_w = icon * texture_w / texture_h;
		}
		state.icon_rect =
		    GRAPHENE_RECT_INIT(state.square.origin.x + (icon - fit_w) / 2,
		                       state.square.origin.y + (icon - fit_h) / 2, fit_w, fit_h);
	} else {
		const auto glyph = static_cast<float>(state.placeholder_size);
		state.icon_rect =
		    GRAPHENE_RECT_INIT(state.square.origin.x + (icon - glyph) / 2,
		                       state.square.origin.y + (icon - glyph) / 2, glyph, glyph);
	}
	const graphene_rect_t& r      = state.icon_rect;
	const float            left   = r.origin.x;
	const float            right  = r.origin.x + r.size.width;
	const float            bottom = r.origin.y + r.size.height;
	// As the overlay had them: running at the top start, metadata unavailable at the bottom end.
	AllocateEmblem(state.running_emblem, state.square, rtl ? right : left, r.origin.y);
	AllocateEmblem(state.metadata_emblem, state.square, rtl ? left : right, bottom);
}

void SnapshotIcon(GtkWidget* widget, GtkSnapshot* snapshot, const GdkRGBA& color) {
	CellState& state = StateOf(widget);
	if (!state.icon || !state.allocated) {
		return;
	}
	const graphene_rect_t& r = state.icon_rect;
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &r.origin);
	if (state.texture) {
		gdk_paintable_snapshot(GDK_PAINTABLE(state.texture.get()), snapshot, r.size.width,
		                       r.size.height);
	} else {
		GtkIconTheme*     theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(widget));
		GtkIconPaintable* icon =
		    gtk_icon_theme_lookup_icon(theme, kPlaceholderIcon, nullptr, state.placeholder_size,
		                               gtk_widget_get_scale_factor(widget),
		                               gtk_widget_get_direction(widget), GtkIconLookupFlags(0));
		gtk_snapshot_push_opacity(snapshot, kDimOpacity);
		gtk_symbolic_paintable_snapshot_symbolic(GTK_SYMBOLIC_PAINTABLE(icon), snapshot,
		                                         r.size.width, r.size.height, &color, 1);
		gtk_snapshot_pop(snapshot);
		g_object_unref(icon);
	}
	gtk_snapshot_restore(snapshot);
}

void SnapshotText(GtkWidget* widget, GtkSnapshot* snapshot, const GdkRGBA& color) {
	CellState&             state = StateOf(widget);
	const graphene_rect_t& r     = state.text_rect;
	if (state.text.empty() || !state.allocated || r.size.width < 1) {
		return;
	}
	PangoLayout* layout = LayoutOf(widget);
	pango_layout_set_width(layout, static_cast<int>(r.size.width) * PANGO_SCALE);
	if (SingleLine(state.kind)) {
		pango_layout_set_alignment(layout, gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL
		                                       ? PANGO_ALIGN_RIGHT
		                                       : PANGO_ALIGN_LEFT);
	}
	int height = 0;
	pango_layout_get_pixel_size(layout, nullptr, &height);
	float y = r.origin.y;
	if (SingleLine(state.kind)) {
		y += (r.size.height - static_cast<float>(height)) / 2;
	}
	gtk_snapshot_save(snapshot);
	const graphene_point_t origin = GRAPHENE_POINT_INIT(r.origin.x, y);
	gtk_snapshot_translate(snapshot, &origin);
	gtk_snapshot_append_layout(snapshot, layout, &color);
	gtk_snapshot_restore(snapshot);
}

void Snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
	CellState& state = StateOf(widget);
	GdkRGBA    color {};
	gtk_widget_get_color(widget, &color);
	SnapshotIcon(widget, snapshot, color);
	SnapshotText(widget, snapshot, color);
	for (GtkWidget* child: {state.indicator, state.metadata_emblem, state.running_emblem}) {
		if (child != nullptr) {
			gtk_widget_snapshot_child(widget, child, snapshot);
		}
	}
}

// ---- GtkAccessibleText: the cell's text, as GtkLabel provides its own (GTK 4.14).

GBytes* TextSlice(const std::string& text, unsigned int start, unsigned int end) {
	const glong length = g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size()));
	const glong from   = std::min<glong>(start, length);
	const glong to     = end == G_MAXUINT ? length : std::clamp<glong>(end, from, length);
	const char* begin  = g_utf8_offset_to_pointer(text.c_str(), from);
	const char* finish = g_utf8_offset_to_pointer(text.c_str(), to);
	return g_bytes_new(begin, static_cast<gsize>(finish - begin));
}

GBytes* AccessibleContents(GtkAccessibleText* self, unsigned int start, unsigned int end) {
	return TextSlice(StateOf(GTK_WIDGET(self)).text, start, end);
}

GBytes* AccessibleContentsAt(GtkAccessibleText* self, unsigned int offset,
                             GtkAccessibleTextGranularity granularity, unsigned int* start,
                             unsigned int* end) {
	const std::string& text = StateOf(GTK_WIDGET(self)).text;
	const auto         length =
	    static_cast<unsigned int>(g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size())));
	// One character, or the whole text: a cell is one line (a tile title one paragraph).
	if (granularity == GTK_ACCESSIBLE_TEXT_GRANULARITY_CHARACTER && length > 0) {
		*start = std::min(offset, length - 1);
		*end   = *start + 1;
	} else {
		*start = 0;
		*end   = length;
	}
	return TextSlice(text, *start, *end);
}

unsigned int AccessibleCaret(GtkAccessibleText* /*self*/) {
	return 0;
}

gboolean AccessibleSelection(GtkAccessibleText* /*self*/, gsize* n_ranges,
                             GtkAccessibleTextRange** ranges) {
	*n_ranges = 0;
	if (ranges != nullptr) {
		*ranges = nullptr;
	}
	return FALSE;
}

gboolean AccessibleAttributes(GtkAccessibleText* /*self*/, unsigned int /*offset*/, gsize* n_ranges,
                              GtkAccessibleTextRange** ranges, char*** attribute_names,
                              char*** attribute_values) {
	*n_ranges = 0;
	if (ranges != nullptr) {
		*ranges = nullptr;
	}
	if (attribute_names != nullptr) {
		*attribute_names = g_new0(char*, 1);
	}
	if (attribute_values != nullptr) {
		*attribute_values = g_new0(char*, 1);
	}
	return FALSE;
}

void AccessibleDefaultAttributes(GtkAccessibleText* /*self*/, char*** attribute_names,
                                 char*** attribute_values) {
	if (attribute_names != nullptr) {
		*attribute_names = g_new0(char*, 1);
	}
	if (attribute_values != nullptr) {
		*attribute_values = g_new0(char*, 1);
	}
}

} // namespace

static void kyty_game_cell_accessible_text_init(GtkAccessibleTextInterface* iface) {
	iface->get_contents           = AccessibleContents;
	iface->get_contents_at        = AccessibleContentsAt;
	iface->get_caret_position     = AccessibleCaret;
	iface->get_selection          = AccessibleSelection;
	iface->get_attributes         = AccessibleAttributes;
	iface->get_default_attributes = AccessibleDefaultAttributes;
}

static void kyty_game_cell_init(KytyGameCell* cell) {
	cell->state = new CellState;
}

static void kyty_game_cell_dispose(GObject* object) {
	CellState& state = StateOf(GTK_WIDGET(object));
	for (GtkWidget** child: {&state.indicator, &state.metadata_emblem, &state.running_emblem}) {
		if (*child != nullptr) {
			gtk_widget_unparent(*child);
			*child = nullptr;
		}
	}
	G_OBJECT_CLASS(kyty_game_cell_parent_class)->dispose(object);
}

static void kyty_game_cell_finalize(GObject* object) {
	KytyGameCell* cell = KYTY_GAME_CELL(object);
	g_clear_object(&cell->state->layout);
	delete cell->state;
	cell->state = nullptr;
	G_OBJECT_CLASS(kyty_game_cell_parent_class)->finalize(object);
}

static void kyty_game_cell_class_init(KytyGameCellClass* klass) {
	GObjectClass*   object_class   = G_OBJECT_CLASS(klass);
	GtkWidgetClass* widget_class   = GTK_WIDGET_CLASS(klass);
	object_class->dispose          = kyty_game_cell_dispose;
	object_class->finalize         = kyty_game_cell_finalize;
	widget_class->measure          = Measure;
	widget_class->get_request_mode = RequestMode;
	widget_class->size_allocate    = SizeAllocate;
	widget_class->snapshot         = Snapshot;
	gtk_widget_class_set_css_name(widget_class, "gamecell");
	gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_LABEL);
}

GtkWidget* kyty_game_cell_new(kyty::gtk::GameCellKind kind) {
	auto* cell        = static_cast<KytyGameCell*>(g_object_new(KYTY_TYPE_GAME_CELL, nullptr));
	cell->state->kind = kind;
	if (kind == GameCellKind::Status) {
		cell->state->indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_add_css_class(cell->state->indicator, "game-status-indicator");
		gtk_widget_set_parent(cell->state->indicator, GTK_WIDGET(cell));
	}
	return GTK_WIDGET(cell);
}

kyty::gtk::GameCellKind kyty_game_cell_get_kind(KytyGameCell* cell) {
	return cell->state->kind;
}

void kyty_game_cell_set_text(KytyGameCell* cell, std::string_view text) {
	CellState& state = *cell->state;
	if (state.text == text) {
		return;
	}
	const glong removed = g_utf8_strlen(state.text.c_str(), static_cast<gssize>(state.text.size()));
	state.text          = std::string(text);
	state.text_measured = false;
	if (state.layout != nullptr) {
		pango_layout_set_text(state.layout, state.text.c_str(), -1);
	}
	if (state.kind == GameCellKind::Tile) {
		gtk_widget_queue_resize(GTK_WIDGET(cell));
	} else {
		gtk_widget_queue_draw(GTK_WIDGET(cell));
	}
	// Assistive technologies read the text when they need it; a change of a cell they can see is
	// announced (an unmapped cell, such as one bound before it is shown, has nothing to announce).
	if (gtk_widget_get_mapped(GTK_WIDGET(cell)) != FALSE) {
		auto* accessible = GTK_ACCESSIBLE_TEXT(cell);
		if (removed > 0) {
			gtk_accessible_text_update_contents(accessible,
			                                    GTK_ACCESSIBLE_TEXT_CONTENT_CHANGE_REMOVE, 0,
			                                    static_cast<unsigned int>(removed));
		}
		const glong added =
		    g_utf8_strlen(state.text.c_str(), static_cast<gssize>(state.text.size()));
		if (added > 0) {
			gtk_accessible_text_update_contents(accessible,
			                                    GTK_ACCESSIBLE_TEXT_CONTENT_CHANGE_INSERT, 0,
			                                    static_cast<unsigned int>(added));
		}
	}
}

const std::string& kyty_game_cell_get_text(KytyGameCell* cell) {
	return cell->state->text;
}

void kyty_game_cell_set_icon(KytyGameCell* cell, KytyGame* game, int icon_size,
                             int placeholder_size) {
	CellState&  state      = *cell->state;
	GdkTexture* texture    = game != nullptr ? kyty_game_get_texture(game) : nullptr;
	const bool  resize     = state.icon_size != icon_size;
	const bool  changed    = resize || state.placeholder_size != placeholder_size ||
	                         state.icon != (game != nullptr) || state.texture.get() != texture;
	state.icon_size        = icon_size;
	state.placeholder_size = placeholder_size;
	state.icon             = game != nullptr;
	if (state.texture.get() != texture) {
		state.texture = GObjectPtr<GdkTexture>::Ref(texture);
	}
	SetEmblem(GTK_WIDGET(cell), state.metadata_emblem, true,
	          game != nullptr && kyty_game_get_metadata_unavailable(game));
	SetEmblem(GTK_WIDGET(cell), state.running_emblem, false,
	          game != nullptr && kyty_game_get_running(game));
	if (resize) {
		gtk_widget_queue_resize(GTK_WIDGET(cell));
	} else if (changed) {
		// The drawn icon's rectangle, and with it the emblems' places, may change.
		gtk_widget_queue_allocate(GTK_WIDGET(cell));
		gtk_widget_queue_draw(GTK_WIDGET(cell));
	}
}

GdkTexture* kyty_game_cell_get_texture(KytyGameCell* cell) {
	return cell->state->texture.get();
}

bool kyty_game_cell_get_placeholder(KytyGameCell* cell) {
	return cell->state->icon && !cell->state->texture;
}

bool kyty_game_cell_get_icon_bounds(KytyGameCell* cell, graphene_rect_t* bounds) {
	if (!cell->state->icon || !cell->state->allocated) {
		return false;
	}
	*bounds = cell->state->icon_rect;
	return true;
}

GtkWidget* kyty_game_cell_get_metadata_emblem(KytyGameCell* cell) {
	return cell->state->metadata_emblem;
}

GtkWidget* kyty_game_cell_get_running_emblem(KytyGameCell* cell) {
	return cell->state->running_emblem;
}

void kyty_game_cell_set_status_class(KytyGameCell* cell, const char* css_class) {
	GtkWidget* indicator = cell->state->indicator;
	if (indicator == nullptr) {
		return;
	}
	if (css_class != nullptr && gtk_widget_has_css_class(indicator, css_class) != FALSE) {
		return;
	}
	// The indicator's classes: game-status-indicator and at most one status-* class.
	char** classes = gtk_widget_get_css_classes(indicator);
	for (char** name = classes; name != nullptr && *name != nullptr; name++) {
		if (g_str_has_prefix(*name, "status-") != FALSE) {
			gtk_widget_remove_css_class(indicator, *name);
		}
	}
	g_strfreev(classes);
	if (css_class != nullptr) {
		gtk_widget_add_css_class(indicator, css_class);
	}
}

GtkWidget* kyty_game_cell_get_indicator(KytyGameCell* cell) {
	return cell->state->indicator;
}

void kyty_game_cell_set_tile_width(KytyGameCell* cell, int width) {
	if (cell->state->tile_width != width) {
		cell->state->tile_width = width;
		gtk_widget_queue_resize(GTK_WIDGET(cell));
	}
}
