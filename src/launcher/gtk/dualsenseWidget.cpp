#include "launcher/gtk/dualsenseWidget.h"

#include "launcher/core/inputMapping.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/inputLabels.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string_view>
#include <utility>

namespace kyty::gtk {

struct DualsenseState {
	std::set<std::string> pressed;
	std::string           selected;
	std::string           hovered;
	float                 left_x  = 0;
	float                 left_y  = 0;
	float                 right_x = 0;
	float                 right_y = 0;
};

} // namespace kyty::gtk

struct _KytyDualsenseWidget {
	GtkWidget                  parent_instance;
	kyty::gtk::DualsenseState* state;
};

G_DEFINE_FINAL_TYPE(KytyDualsenseWidget, kyty_dualsense_widget, GTK_TYPE_WIDGET)

namespace {

using kyty::gtk::DualsenseState;

guint g_control_clicked_signal = 0;

// ---- Geometry: the Qt launcher's diagram coordinates

struct Rect {
	float x = 0;
	float y = 0;
	float w = 0;
	float h = 0;

	[[nodiscard]] constexpr float Right() const { return x + w; }
	[[nodiscard]] constexpr float Bottom() const { return y + h; }
	[[nodiscard]] constexpr float CenterX() const { return x + w / 2; }
	[[nodiscard]] constexpr float CenterY() const { return y + h / 2; }
	[[nodiscard]] constexpr Rect  Adjusted(float left, float top, float right, float bottom) const {
		return {x + left, y + top, w - left + right, h - top + bottom};
	}
	[[nodiscard]] constexpr bool Contains(float px, float py) const {
		return px >= x && px < Right() && py >= y && py < Bottom();
	}
};

enum class Shape { Button, Shoulder, Touchpad, Face, Dpad, Stick, Direction };

struct Control {
	std::string_view id;
	Rect             bounds;
	Shape            shape = Shape::Button;
	int              angle = 0; // Dpad and Direction: degrees clockwise from up
};

// Painting and hit testing share a canvas with room above the shell for the shoulders.
constexpr Rect kCanvas {0, -20, 480, 330};

// In hit-testing order: the stick clicks come before the stick directions around them.
constexpr Control kControls[] = {
    {"L2", {90, -12, 58, 18}, Shape::Shoulder},
    {"R2", {332, -12, 58, 18}, Shape::Shoulder},
    {"L1", {84, 12, 64, 12}, Shape::Shoulder},
    {"R1", {332, 12, 64, 12}, Shape::Shoulder},
    {"TouchPad", {150, 25, 90, 100}, Shape::Touchpad},
    {"TouchPadRight", {240, 25, 90, 100}, Shape::Touchpad},
    {"Options", {330, 60, 9, 20}},
    {"Up", {105, 72, 22, 25}, Shape::Dpad, 0},
    {"Right", {124, 94, 25, 22}, Shape::Dpad, 90},
    {"Down", {105, 113, 22, 25}, Shape::Dpad, 180},
    {"Left", {83, 94, 25, 22}, Shape::Dpad, 270},
    {"Triangle", {353, 67, 26, 26}, Shape::Face},
    {"Circle", {382, 96, 26, 26}, Shape::Face},
    {"Cross", {353, 125, 26, 26}, Shape::Face},
    {"Square", {324, 96, 26, 26}, Shape::Face},
    {"L3", {143, 131, 66, 66}, Shape::Stick},
    {"R3", {271, 131, 66, 66}, Shape::Stick},
    {"LeftStickUp", {143, 131, 66, 66}, Shape::Direction, 0},
    {"LeftStickRight", {143, 131, 66, 66}, Shape::Direction, 90},
    {"LeftStickDown", {143, 131, 66, 66}, Shape::Direction, 180},
    {"LeftStickLeft", {143, 131, 66, 66}, Shape::Direction, 270},
    {"RightStickUp", {271, 131, 66, 66}, Shape::Direction, 0},
    {"RightStickRight", {271, 131, 66, 66}, Shape::Direction, 90},
    {"RightStickDown", {271, 131, 66, 66}, Shape::Direction, 180},
    {"RightStickLeft", {271, 131, 66, 66}, Shape::Direction, 270},
};

constexpr GdkRGBA Rgb(int red, int green, int blue) {
	return {static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F,
	        static_cast<float>(blue) / 255.0F, 1.0F};
}

// The Qt launcher's colors: a black shell with light outlines; pressed controls cyan, active
// (selected or under the pointer) ones dark blue with a light cyan outline.
constexpr GdkRGBA kBlack         = Rgb(0x11, 0x11, 0x11);
constexpr GdkRGBA kWhite         = Rgb(0xee, 0xee, 0xee);
constexpr GdkRGBA kPressedFill   = Rgb(0x2e, 0xb6, 0xdf);
constexpr GdkRGBA kActiveFill    = Rgb(0x3a, 0x61, 0x79);
constexpr GdkRGBA kActiveOutline = Rgb(0x79, 0xe3, 0xff);

using PathPtr = std::unique_ptr<GskPath, decltype(&gsk_path_unref)>;

PathPtr Finish(GskPathBuilder* builder) {
	return {gsk_path_builder_free_to_path(builder), &gsk_path_unref};
}

graphene_point_t Point(float x, float y) {
	return graphene_point_t {x, y};
}

graphene_rect_t RectOf(const Rect& rect) {
	return graphene_rect_t {{rect.x, rect.y}, {rect.w, rect.h}};
}

PathPtr ShellPath() {
	GskPathBuilder* b = gsk_path_builder_new();
	gsk_path_builder_move_to(b, 81, 48);
	gsk_path_builder_cubic_to(b, 151, 21, 329, 21, 399, 48);
	gsk_path_builder_cubic_to(b, 420, 94, 445, 166, 445, 215);
	gsk_path_builder_cubic_to(b, 447, 253, 437, 291, 419, 296);
	gsk_path_builder_cubic_to(b, 403, 302, 395, 297, 389, 283);
	gsk_path_builder_cubic_to(b, 381, 268, 373, 236, 354, 213);
	gsk_path_builder_cubic_to(b, 347, 205, 341, 205, 325, 205);
	gsk_path_builder_line_to(b, 155, 205);
	gsk_path_builder_cubic_to(b, 139, 205, 133, 205, 126, 213);
	gsk_path_builder_cubic_to(b, 107, 236, 99, 268, 91, 283);
	gsk_path_builder_cubic_to(b, 85, 297, 77, 302, 61, 296);
	gsk_path_builder_cubic_to(b, 43, 291, 33, 253, 35, 215);
	gsk_path_builder_cubic_to(b, 35, 166, 60, 94, 81, 48);
	gsk_path_builder_close(b);
	return Finish(b);
}

// Panel seams flow into the inner grips without a second outline across the belly.
PathPtr SeamsPath() {
	GskPathBuilder* b = gsk_path_builder_new();
	gsk_path_builder_move_to(b, 153, 35);
	gsk_path_builder_cubic_to(b, 153, 69, 176, 105, 158, 127);
	gsk_path_builder_cubic_to(b, 123, 166, 108, 226, 76, 297);
	gsk_path_builder_move_to(b, 327, 35);
	gsk_path_builder_cubic_to(b, 327, 69, 304, 105, 322, 127);
	gsk_path_builder_cubic_to(b, 357, 166, 372, 226, 404, 297);
	return Finish(b);
}

PathPtr TouchpadPath() {
	GskPathBuilder* b = gsk_path_builder_new();
	gsk_path_builder_move_to(b, 158, 42);
	gsk_path_builder_cubic_to(b, 156, 33, 168, 32, 178, 31);
	gsk_path_builder_cubic_to(b, 219, 28, 261, 28, 302, 31);
	gsk_path_builder_cubic_to(b, 312, 32, 324, 33, 322, 42);
	gsk_path_builder_cubic_to(b, 318, 64, 315, 87, 309, 105);
	gsk_path_builder_cubic_to(b, 306, 117, 299, 122, 286, 122);
	gsk_path_builder_line_to(b, 194, 122);
	gsk_path_builder_cubic_to(b, 181, 122, 174, 117, 171, 105);
	gsk_path_builder_cubic_to(b, 165, 87, 162, 64, 158, 42);
	gsk_path_builder_close(b);
	return Finish(b);
}

PathPtr RoundedRectPath(const Rect& rect, float radius) {
	GskPathBuilder*       b = gsk_path_builder_new();
	GskRoundedRect        rounded {};
	const graphene_rect_t bounds = RectOf(rect);
	gsk_rounded_rect_init_from_rect(&rounded, &bounds, radius);
	gsk_path_builder_add_rounded_rect(b, &rounded);
	return Finish(b);
}

PathPtr CirclePath(float x, float y, float radius) {
	GskPathBuilder*        b      = gsk_path_builder_new();
	const graphene_point_t center = Point(x, y);
	gsk_path_builder_add_circle(b, &center, radius);
	return Finish(b);
}

// Points of a shape drawn around the origin, rotated by angle degrees clockwise and moved to
// (cx, cy).
struct Placement {
	float cx     = 0;
	float cy     = 0;
	float cosine = 1;
	float sine   = 0;

	Placement(float x, float y, int angle): cx(x), cy(y) {
		const double radians = angle * std::numbers::pi / 180.0;
		cosine               = static_cast<float>(std::cos(radians));
		sine                 = static_cast<float>(std::sin(radians));
	}
	[[nodiscard]] float X(float x, float y) const { return cx + x * cosine - y * sine; }
	[[nodiscard]] float Y(float x, float y) const { return cy + x * sine + y * cosine; }
};

// A point on the circle of bounds at angle degrees counterclockwise from three o'clock (Qt's arc
// angles).
graphene_point_t ArcPoint(const Rect& bounds, double angle) {
	const double radians = angle * std::numbers::pi / 180.0;
	return Point(bounds.CenterX() + static_cast<float>(bounds.w / 2 * std::cos(radians)),
	             bounds.CenterY() - static_cast<float>(bounds.h / 2 * std::sin(radians)));
}

PathPtr ControlPath(const Control& control) {
	const Rect&     bounds = control.bounds;
	GskPathBuilder* b      = gsk_path_builder_new();
	switch (control.shape) {
		case Shape::Direction: {
			// An 82 degree segment of the ring between the stick's rim and its knob area.
			const Rect   inner       = bounds.Adjusted(7, 7, -7, -7);
			const double start       = 49 - control.angle;
			const auto   outer_start = ArcPoint(bounds, start);
			const auto   outer_end   = ArcPoint(bounds, start + 82);
			const auto   inner_end   = ArcPoint(inner, start + 82);
			const auto   inner_start = ArcPoint(inner, start);
			// Counterclockwise on the screen along the rim (Qt's positive arc span), clockwise
			// back along the inner edge.
			gsk_path_builder_move_to(b, outer_start.x, outer_start.y);
			gsk_path_builder_svg_arc_to(b, bounds.w / 2, bounds.h / 2, 0, FALSE, FALSE, outer_end.x,
			                            outer_end.y);
			gsk_path_builder_line_to(b, inner_end.x, inner_end.y);
			gsk_path_builder_svg_arc_to(b, inner.w / 2, inner.h / 2, 0, FALSE, TRUE, inner_start.x,
			                            inner_start.y);
			gsk_path_builder_close(b);
			break;
		}
		case Shape::Touchpad: gsk_path_builder_unref(b); return TouchpadPath();
		case Shape::Face: {
			const graphene_point_t center = Point(bounds.CenterX(), bounds.CenterY());
			gsk_path_builder_add_circle(b, &center, bounds.w / 2);
			break;
		}
		case Shape::Stick: {
			const graphene_point_t center = Point(bounds.CenterX(), bounds.CenterY());
			gsk_path_builder_add_circle(b, &center, bounds.w / 2 - 7);
			break;
		}
		case Shape::Dpad: {
			const Placement p(bounds.CenterX(), bounds.CenterY(), control.angle);
			const auto      move = [&](float x, float y) {
				gsk_path_builder_move_to(b, p.X(x, y), p.Y(x, y));
			};
			const auto line = [&](float x, float y) {
				gsk_path_builder_line_to(b, p.X(x, y), p.Y(x, y));
			};
			const auto quad = [&](float x1, float y1, float x2, float y2) {
				gsk_path_builder_quad_to(b, p.X(x1, y1), p.Y(x1, y1), p.X(x2, y2), p.Y(x2, y2));
			};
			move(-8, -12);
			quad(-11, -12, -11, -8);
			line(-10, 3);
			quad(-10, 6, -7, 8);
			line(0, 12);
			line(7, 8);
			quad(10, 6, 10, 3);
			line(11, -8);
			quad(11, -12, 8, -12);
			gsk_path_builder_close(b);
			break;
		}
		case Shape::Shoulder:
			gsk_path_builder_move_to(b, bounds.x, bounds.Bottom());
			gsk_path_builder_line_to(b, bounds.x + 2, bounds.CenterY());
			gsk_path_builder_cubic_to(b, bounds.x + 10, bounds.y, bounds.Right() - 10, bounds.y,
			                          bounds.Right() - 2, bounds.CenterY());
			gsk_path_builder_line_to(b, bounds.Right(), bounds.Bottom());
			gsk_path_builder_close(b);
			break;
		case Shape::Button: gsk_path_builder_unref(b); return RoundedRectPath(bounds, 4);
	}
	return Finish(b);
}

bool Hits(const Control& control, float x, float y) {
	const graphene_point_t point = Point(x, y);
	if (control.shape == Shape::Touchpad) {
		return control.bounds.Contains(x, y) &&
		       gsk_path_in_fill(TouchpadPath().get(), &point, GSK_FILL_RULE_WINDING) != FALSE;
	}
	return gsk_path_in_fill(ControlPath(control).get(), &point, GSK_FILL_RULE_WINDING) != FALSE;
}

void Fill(GtkSnapshot* snapshot, const PathPtr& path, const GdkRGBA& color) {
	gtk_snapshot_append_fill(snapshot, path.get(), GSK_FILL_RULE_WINDING, &color);
}

void Stroke(GtkSnapshot* snapshot, const PathPtr& path, float width, const GdkRGBA& color) {
	GskStroke* stroke = gsk_stroke_new(width);
	gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
	gtk_snapshot_append_stroke(snapshot, path.get(), stroke, &color);
	gsk_stroke_free(stroke);
}

void FillAndStroke(GtkSnapshot* snapshot, const PathPtr& path, const GdkRGBA& fill, float width,
                   const GdkRGBA& outline) {
	Fill(snapshot, path, fill);
	Stroke(snapshot, path, width, outline);
}

// The symbol on a face button or D-pad button, in white.
void DrawSymbol(GtkSnapshot* snapshot, const Control& control) {
	const float     cx = control.bounds.CenterX();
	const float     cy = control.bounds.CenterY();
	GskPathBuilder* b  = gsk_path_builder_new();
	if (control.shape == Shape::Dpad) {
		const Placement p(cx, cy, control.angle);
		gsk_path_builder_move_to(b, p.X(-4, 2), p.Y(-4, 2));
		gsk_path_builder_line_to(b, p.X(0, -2), p.Y(0, -2));
		gsk_path_builder_line_to(b, p.X(4, 2), p.Y(4, 2));
	} else if (control.id == "Triangle") {
		gsk_path_builder_move_to(b, cx, cy - 7);
		gsk_path_builder_line_to(b, cx + 7, cy + 5);
		gsk_path_builder_line_to(b, cx - 7, cy + 5);
		gsk_path_builder_close(b);
	} else if (control.id == "Circle") {
		const graphene_point_t center = Point(cx, cy);
		gsk_path_builder_add_circle(b, &center, 7);
	} else if (control.id == "Square") {
		const graphene_rect_t square = RectOf({cx - 6, cy - 6, 12, 12});
		gsk_path_builder_add_rect(b, &square);
	} else if (control.id == "Cross") {
		gsk_path_builder_move_to(b, cx - 5, cy - 5);
		gsk_path_builder_line_to(b, cx + 5, cy + 5);
		gsk_path_builder_move_to(b, cx - 5, cy + 5);
		gsk_path_builder_line_to(b, cx + 5, cy - 5);
	}
	Stroke(snapshot, Finish(b), 1.3F, kWhite);
}

// A shoulder's name, centered in its shape (9 px bold, as the Qt launcher drew it).
void DrawShoulderName(GtkWidget* widget, GtkSnapshot* snapshot, const Control& control) {
	const std::string text(control.id);
	auto              layout = kyty::gtk::GObjectPtr<PangoLayout>::Adopt(
	    gtk_widget_create_pango_layout(widget, text.c_str()));
	PangoFontDescription* font = pango_font_description_copy(
	    pango_context_get_font_description(pango_layout_get_context(layout.get())));
	pango_font_description_set_absolute_size(font, 9 * PANGO_SCALE);
	pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
	pango_layout_set_font_description(layout.get(), font);
	pango_font_description_free(font);
	int width  = 0;
	int height = 0;
	pango_layout_get_pixel_size(layout.get(), &width, &height);
	const Rect             area   = control.bounds.Adjusted(0, 2, 0, 0);
	const graphene_point_t origin = Point(area.CenterX() - static_cast<float>(width) / 2,
	                                      area.CenterY() - static_cast<float>(height) / 2);
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &origin);
	gtk_snapshot_append_layout(snapshot, layout.get(), &kWhite);
	gtk_snapshot_restore(snapshot);
}

// The scale and offset that fit kCanvas into width x height, centered.
struct View {
	float scale = 1;
	float dx    = 0;
	float dy    = 0;
};

View ViewFor(int width, int height) {
	View view;
	view.scale =
	    std::min(static_cast<float>(width) / kCanvas.w, static_cast<float>(height) / kCanvas.h);
	view.dx = static_cast<float>(width) / 2 - kCanvas.CenterX() * view.scale;
	view.dy = static_cast<float>(height) / 2 - kCanvas.CenterY() * view.scale;
	return view;
}

std::string HitTest(GtkWidget* widget, double x, double y) {
	const View view = ViewFor(gtk_widget_get_width(widget), gtk_widget_get_height(widget));
	if (view.scale <= 0) {
		return {};
	}
	const float cx = (static_cast<float>(x) - view.dx) / view.scale;
	const float cy = (static_cast<float>(y) - view.dy) / view.scale;
	for (const auto& control: kControls) {
		if (Hits(control, cx, cy)) {
			return std::string(control.id);
		}
	}
	return {};
}

void Snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
	const DualsenseState& state = *KYTY_DUALSENSE_WIDGET(widget)->state;
	const View view = ViewFor(gtk_widget_get_width(widget), gtk_widget_get_height(widget));
	if (view.scale <= 0) {
		return;
	}
	gtk_snapshot_save(snapshot);
	const graphene_point_t offset = Point(view.dx, view.dy);
	gtk_snapshot_translate(snapshot, &offset);
	gtk_snapshot_scale(snapshot, view.scale, view.scale);

	FillAndStroke(snapshot, ShellPath(), kBlack, 1.5F, kWhite);
	Stroke(snapshot, SeamsPath(), 1.5F, kWhite);
	FillAndStroke(snapshot, TouchpadPath(), kBlack, 1.5F, kWhite);
	// The Create button (not mapped) and the PS button.
	FillAndStroke(snapshot, RoundedRectPath({141, 60, 9, 20}, 4), kBlack, 1.5F, kWhite);
	FillAndStroke(snapshot, CirclePath(240, 166, 8), kBlack, 1.5F, kWhite);

	for (const auto& control: kControls) {
		const std::string id(control.id);
		const bool        pressed  = state.pressed.count(id) != 0;
		const bool        selected = state.selected == id;
		const bool        hovered  = state.hovered == id;
		const bool        active   = pressed || selected || hovered;
		if ((control.shape == Shape::Direction || control.shape == Shape::Touchpad) && !active) {
			continue;
		}
		const GdkRGBA& fill    = pressed ? kPressedFill : active ? kActiveFill : kBlack;
		const GdkRGBA& outline = active ? kActiveOutline : kWhite;
		const float    width   = selected ? 2.0F : 1.3F;
		if (control.shape == Shape::Touchpad) {
			// One shared surface, clipped to the half, so that the halves never add a seam.
			const graphene_rect_t clip = RectOf(control.bounds);
			gtk_snapshot_push_clip(snapshot, &clip);
			FillAndStroke(snapshot, TouchpadPath(), fill, width, outline);
			gtk_snapshot_pop(snapshot);
		} else if (control.shape == Shape::Stick) {
			const Rect& bounds = control.bounds;
			FillAndStroke(snapshot, CirclePath(bounds.CenterX(), bounds.CenterY(), bounds.w / 2),
			              kBlack, width, outline);
			const bool  left  = control.id == "L3";
			float       x     = std::clamp(left ? state.left_x : state.right_x, -1.0F, 1.0F);
			float       y     = std::clamp(left ? state.left_y : state.right_y, -1.0F, 1.0F);
			const float scale = 5.0F / std::max(1.0F, std::hypot(x, y));
			x *= scale;
			y *= scale;
			FillAndStroke(snapshot,
			              CirclePath(bounds.CenterX() + x, bounds.CenterY() + y, bounds.w / 2 - 12),
			              fill, width, outline);
		} else {
			FillAndStroke(snapshot, ControlPath(control), fill, width, outline);
			if (control.shape == Shape::Face || control.shape == Shape::Dpad) {
				DrawSymbol(snapshot, control);
			} else if (control.shape == Shape::Shoulder) {
				DrawShoulderName(widget, snapshot, control);
			} else if (control.id == "Options") {
				GskPathBuilder* b = gsk_path_builder_new();
				gsk_path_builder_move_to(b, control.bounds.CenterX(), control.bounds.CenterY() - 4);
				gsk_path_builder_line_to(b, control.bounds.CenterX(), control.bounds.CenterY() + 4);
				Stroke(snapshot, Finish(b), 1.3F, kWhite);
			}
		}
	}
	gtk_snapshot_restore(snapshot);
}

void Measure(GtkWidget* /*widget*/, GtkOrientation orientation, int /*for_size*/, int* minimum,
             int* natural, int* minimum_baseline, int* natural_baseline) {
	const bool horizontal = orientation == GTK_ORIENTATION_HORIZONTAL;
	*minimum              = horizontal ? 330 : 230;
	*natural              = horizontal ? static_cast<int>(kCanvas.w) : static_cast<int>(kCanvas.h);
	*minimum_baseline     = -1;
	*natural_baseline     = -1;
}

void SetHovered(KytyDualsenseWidget* self, std::string hovered) {
	if (self->state->hovered == hovered) {
		return;
	}
	gtk_widget_set_cursor_from_name(GTK_WIDGET(self), hovered.empty() ? nullptr : "pointer");
	self->state->hovered = std::move(hovered);
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

void OnMotion(GtkEventControllerMotion* /*controller*/, double x, double y, gpointer data) {
	auto* self = KYTY_DUALSENSE_WIDGET(data);
	SetHovered(self, HitTest(GTK_WIDGET(self), x, y));
}

void OnLeave(GtkEventControllerMotion* /*controller*/, gpointer data) {
	SetHovered(KYTY_DUALSENSE_WIDGET(data), {});
}

void OnPressed(GtkGestureClick* gesture, int /*presses*/, double x, double y, gpointer data) {
	auto*             self = KYTY_DUALSENSE_WIDGET(data);
	const std::string id   = HitTest(GTK_WIDGET(self), x, y);
	if (id.empty()) {
		return;
	}
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	g_signal_emit(self, g_control_clicked_signal, 0, id.c_str());
}

gboolean OnQueryTooltip(GtkWidget* widget, int x, int y, gboolean /*keyboard*/, GtkTooltip* tooltip,
                        gpointer /*data*/) {
	const std::string id = HitTest(widget, x, y);
	if (id.empty()) {
		return FALSE;
	}
	const auto* control = kyty::core::FindInputControl(id);
	gtk_tooltip_set_text(
	    tooltip, control != nullptr
	                 ? kyty::gtk::InputControlTitle(*control, kyty::core::InputDevice::Pad).c_str()
	                 : id.c_str());
	return TRUE;
}

} // namespace

static void kyty_dualsense_widget_init(KytyDualsenseWidget* self) {
	self->state       = new DualsenseState;
	GtkWidget* widget = GTK_WIDGET(self);
	gtk_widget_set_hexpand(widget, TRUE);
	gtk_widget_set_vexpand(widget, TRUE);
	gtk_widget_set_has_tooltip(widget, TRUE);
	g_signal_connect(widget, "query-tooltip", G_CALLBACK(&OnQueryTooltip), nullptr);

	GtkEventController* motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(&OnMotion), self);
	g_signal_connect(motion, "leave", G_CALLBACK(&OnLeave), self);
	gtk_widget_add_controller(widget, motion);

	GtkGesture* click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "pressed", G_CALLBACK(&OnPressed), self);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("DualSense controller"),
	    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
	    _("Click a control to select its binding. Connected controller input lights up the "
	      "controls."),
	    -1);
}

static void kyty_dualsense_widget_finalize(GObject* object) {
	auto* self = KYTY_DUALSENSE_WIDGET(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_dualsense_widget_parent_class)->finalize(object);
}

static void kyty_dualsense_widget_class_init(KytyDualsenseWidgetClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->finalize = kyty_dualsense_widget_finalize;
	widget_class->snapshot = Snapshot;
	widget_class->measure  = Measure;
	gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_IMG);
	gtk_widget_class_set_css_name(widget_class, "dualsense");
	// "control-clicked" (const char* control): a click on a control.
	g_control_clicked_signal =
	    g_signal_new("control-clicked", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
	                 nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
}

GtkWidget* kyty_dualsense_widget_new() {
	return GTK_WIDGET(g_object_new(KYTY_TYPE_DUALSENSE_WIDGET, nullptr));
}

void kyty_dualsense_widget_set_pressed(KytyDualsenseWidget*         widget,
                                       const std::set<std::string>& pressed) {
	g_return_if_fail(KYTY_IS_DUALSENSE_WIDGET(widget));
	if (widget->state->pressed != pressed) {
		widget->state->pressed = pressed;
		gtk_widget_queue_draw(GTK_WIDGET(widget));
	}
}

const std::set<std::string>& kyty_dualsense_widget_get_pressed(KytyDualsenseWidget* widget) {
	static const std::set<std::string> kNone;
	g_return_val_if_fail(KYTY_IS_DUALSENSE_WIDGET(widget), kNone);
	return widget->state->pressed;
}

void kyty_dualsense_widget_set_selected(KytyDualsenseWidget* widget, const std::string& control) {
	g_return_if_fail(KYTY_IS_DUALSENSE_WIDGET(widget));
	if (widget->state->selected != control) {
		widget->state->selected = control;
		gtk_widget_queue_draw(GTK_WIDGET(widget));
	}
}

void kyty_dualsense_widget_set_sticks(KytyDualsenseWidget* widget, float left_x, float left_y,
                                      float right_x, float right_y) {
	g_return_if_fail(KYTY_IS_DUALSENSE_WIDGET(widget));
	auto& state = *widget->state;
	if (state.left_x != left_x || state.left_y != left_y || state.right_x != right_x ||
	    state.right_y != right_y) {
		state.left_x  = left_x;
		state.left_y  = left_y;
		state.right_x = right_x;
		state.right_y = right_y;
		gtk_widget_queue_draw(GTK_WIDGET(widget));
	}
}

std::string kyty_dualsense_widget_hit_test(KytyDualsenseWidget* widget, double x, double y) {
	g_return_val_if_fail(KYTY_IS_DUALSENSE_WIDGET(widget), std::string());
	return HitTest(GTK_WIDGET(widget), x, y);
}
