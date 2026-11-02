#include "ui/support/uiDriver.h"

#include "ui/support/uiHarness.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

namespace KytyUi {

using kyty::gtk::GCharPtr;
using kyty::gtk::GErrorPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;

namespace {

// A 5 ms tick that wakes a blocking main context iteration, so no wait sleeps past its deadline.
class Tick {
public:
	Tick()
	    : m_id(g_timeout_add(5, [](gpointer) -> gboolean { return G_SOURCE_CONTINUE; }, nullptr)) {}
	~Tick() { g_source_remove(m_id); }

	Tick(const Tick&)            = delete;
	Tick& operator=(const Tick&) = delete;

private:
	guint m_id;
};

void Collect(GtkWidget* widget, const std::function<bool(GtkWidget*)>& match,
             std::vector<GtkWidget*>& found) {
	if (match(widget)) {
		found.push_back(widget);
	}
	for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
	     child            = gtk_widget_get_next_sibling(child)) {
		Collect(child, match, found);
	}
}

std::vector<GtkWindow*> Toplevels() {
	GListModel*             list = gtk_window_get_toplevels();
	std::vector<GtkWindow*> windows;
	for (guint i = 0; i < g_list_model_get_n_items(list); i++) {
		auto window =
		    GObjectPtr<GtkWindow>::Adopt(static_cast<GtkWindow*>(g_list_model_get_item(list, i)));
		windows.push_back(window.get());
	}
	return windows;
}

std::string Text(const char* text) {
	return text != nullptr ? text : "";
}

// ---- KeyPress

std::vector<GObjectPtr<GtkEventController>> Controllers(GtkWidget* widget) {
	// observe_controllers adds bookkeeping while its list exists, so the list is dropped at once.
	auto list = GObjectPtr<GListModel>::Adopt(gtk_widget_observe_controllers(widget));
	std::vector<GObjectPtr<GtkEventController>> controllers;
	for (guint i = 0; i < g_list_model_get_n_items(list.get()); i++) {
		controllers.push_back(GObjectPtr<GtkEventController>::Adopt(
		    static_cast<GtkEventController*>(g_list_model_get_item(list.get(), i))));
	}
	return controllers;
}

// The root's own controllers of its GtkShortcutManager run the global and managed shortcut
// controllers in GTK; KeyPress runs those directly (ManagedControllers) instead.
bool IsShortcutManagerController(GtkEventController* controller) {
	const char* name = gtk_event_controller_get_name(controller);
	return name != nullptr && g_str_has_prefix(name, "gtk-shortcut-manager");
}

bool IsLocalShortcutController(GtkEventController* controller) {
	return GTK_IS_SHORTCUT_CONTROLLER(controller) &&
	       gtk_shortcut_controller_get_scope(GTK_SHORTCUT_CONTROLLER(controller)) ==
	           GTK_SHORTCUT_SCOPE_LOCAL;
}

// The first GtkShortcutManager (a window or a popover) at or above widget.
GtkWidget* ShortcutManagerOf(GtkWidget* widget) {
	while (widget != nullptr && !G_TYPE_CHECK_INSTANCE_TYPE(widget, GTK_TYPE_SHORTCUT_MANAGER)) {
		widget = gtk_widget_get_parent(widget);
	}
	return widget;
}

// The shortcut controllers below root that GTK runs for a key whose propagation chain is chain:
// those of global scope (the root's), and those of managed scope whose shortcut manager is in the
// chain. A popover is a shortcut manager of its own, so the managed shortcuts of its items (a menu
// item's accelerator) run only while the key goes to the popover.
std::vector<GObjectPtr<GtkEventController>>
ManagedControllers(GtkWidget* root, const std::vector<GtkWidget*>& chain) {
	std::vector<GObjectPtr<GtkEventController>> managed;
	for (GtkWidget* widget: FindAll(root, [](GtkWidget*) { return true; })) {
		for (auto& controller: Controllers(widget)) {
			if (!GTK_IS_SHORTCUT_CONTROLLER(controller.get()) ||
			    IsLocalShortcutController(controller.get()) ||
			    IsShortcutManagerController(controller.get())) {
				continue;
			}
			const bool global =
			    gtk_shortcut_controller_get_scope(GTK_SHORTCUT_CONTROLLER(controller.get())) ==
			    GTK_SHORTCUT_SCOPE_GLOBAL;
			GtkWidget* manager = ShortcutManagerOf(widget);
			if (global || std::find(chain.begin(), chain.end(), manager) != chain.end()) {
				managed.push_back(controller);
			}
		}
	}
	return managed;
}

bool TriggerMatches(GtkShortcutTrigger* trigger, GtkShortcutTrigger* key) {
	if (GTK_IS_ALTERNATIVE_TRIGGER(trigger)) {
		return TriggerMatches(gtk_alternative_trigger_get_first(GTK_ALTERNATIVE_TRIGGER(trigger)),
		                      key) ||
		       TriggerMatches(gtk_alternative_trigger_get_second(GTK_ALTERNATIVE_TRIGGER(trigger)),
		                      key);
	}
	return GTK_IS_KEYVAL_TRIGGER(trigger) && gtk_shortcut_trigger_compare(trigger, key) == 0;
}

struct Key {
	guint                          keyval    = 0;
	GdkModifierType                modifiers = GdkModifierType(0);
	guint                          keycode   = 0;
	GObjectPtr<GtkShortcutTrigger> trigger;
};

// One controller as GTK's event delivery would run it for the key; true when it handled the key.
bool Run(GtkEventController* controller, const Key& key) {
	GtkWidget* widget = gtk_event_controller_get_widget(controller);
	if (widget == nullptr || gtk_widget_is_sensitive(widget) == FALSE) {
		return false;
	}
	if (GTK_IS_SHORTCUT_CONTROLLER(controller)) {
		GListModel* shortcuts = G_LIST_MODEL(controller);
		for (guint i = 0; i < g_list_model_get_n_items(shortcuts); i++) {
			auto item = GObjectPtr<GObject>::Adopt(
			    static_cast<GObject*>(g_list_model_get_item(shortcuts, i)));
			if (!GTK_IS_SHORTCUT(item.get())) {
				continue;
			}
			auto* shortcut = GTK_SHORTCUT(item.get());
			if (TriggerMatches(gtk_shortcut_get_trigger(shortcut), key.trigger.get()) &&
			    gtk_shortcut_action_activate(gtk_shortcut_get_action(shortcut),
			                                 GTK_SHORTCUT_ACTION_EXCLUSIVE, widget,
			                                 gtk_shortcut_get_arguments(shortcut)) != FALSE) {
				return true;
			}
		}
		return false;
	}
	if (GTK_IS_EVENT_CONTROLLER_KEY(controller)) {
		gboolean handled = FALSE;
		g_signal_emit_by_name(controller, "key-pressed", key.keyval, key.keycode,
		                      static_cast<guint>(key.modifiers), &handled);
		return handled != FALSE;
	}
	return false;
}

bool RunApplicationAccelerators(GtkWidget* target, const Key& key) {
	GtkRoot* root = gtk_widget_get_root(target);
	if (!GTK_IS_WINDOW(root)) {
		return false;
	}
	GtkApplication* application = gtk_window_get_application(GTK_WINDOW(root));
	if (application == nullptr) {
		return false;
	}
	const GCharPtr accelerator(gtk_accelerator_name(key.keyval, key.modifiers));
	gchar**        actions = gtk_application_get_actions_for_accel(application, accelerator.get());
	bool           handled = false;
	for (gchar** action = actions; action != nullptr && *action != nullptr && !handled; action++) {
		GCharPtr  name;
		GVariant* parameter = nullptr;
		GErrorPtr error;
		if (g_action_parse_detailed_name(*action, name.Out(), &parameter, error.Out()) == FALSE) {
			continue;
		}
		handled = gtk_widget_activate_action_variant(target, name.get(), parameter) != FALSE;
		if (parameter != nullptr) {
			g_variant_unref(parameter);
		}
	}
	g_strfreev(actions);
	return handled;
}

} // namespace

void Pump(int milliseconds) {
	const Tick   tick;
	const gint64 end = g_get_monotonic_time() + gint64 {milliseconds} * 1000;
	while (g_get_monotonic_time() < end) {
		g_main_context_iteration(nullptr, TRUE);
	}
}

bool WaitFor(const std::function<bool()>& predicate, int timeout_ms, std::string_view what) {
	const Tick   tick;
	const gint64 end = g_get_monotonic_time() + gint64 {timeout_ms} * 1000;
	while (!predicate()) {
		if (g_get_monotonic_time() >= end) {
			Fail("timed out after " + std::to_string(timeout_ms) +
			     " ms waiting for: " + std::string(what));
			return false;
		}
		g_main_context_iteration(nullptr, TRUE);
	}
	return true;
}

// ---- Widgets

std::vector<GtkWidget*> FindAll(GtkWidget* root, const std::function<bool(GtkWidget*)>& match) {
	std::vector<GtkWidget*> found;
	if (root != nullptr) {
		Collect(root, match, found);
	}
	return found;
}

GtkWidget* FindFirst(GtkWidget* root, const std::function<bool(GtkWidget*)>& match) {
	const auto found = FindAll(root, match);
	return found.empty() ? nullptr : found.front();
}

GtkWidget* FindByType(GtkWidget* root, GType type) {
	return FindFirst(root, [type](GtkWidget* widget) {
		return G_TYPE_CHECK_INSTANCE_TYPE(widget, type) != FALSE;
	});
}

GtkWidget* FindByBuildableId(GtkWidget* root, std::string_view id) {
	return FindFirst(root, [id](GtkWidget* widget) {
		const char* own = gtk_buildable_get_buildable_id(GTK_BUILDABLE(widget));
		return own != nullptr && id == own;
	});
}

GtkWidget* FindByLabel(GtkWidget* root, std::string_view label) {
	return FindFirst(root, [label](GtkWidget* widget) { return WidgetText(widget) == label; });
}

GtkWidget* FindByAccessibleLabel(GtkWidget* root, std::string_view label) {
	return FindFirst(root, [label](GtkWidget* widget) {
		const auto own = AccessibleProperty(widget, GTK_ACCESSIBLE_PROPERTY_LABEL);
		return own && *own == label;
	});
}

std::string WidgetText(GtkWidget* widget) {
	if (GTK_IS_BUTTON(widget)) {
		return Text(gtk_button_get_label(GTK_BUTTON(widget)));
	}
	if (GTK_IS_CHECK_BUTTON(widget)) {
		return Text(gtk_check_button_get_label(GTK_CHECK_BUTTON(widget)));
	}
	if (GTK_IS_MENU_BUTTON(widget)) {
		return Text(gtk_menu_button_get_label(GTK_MENU_BUTTON(widget)));
	}
	if (GTK_IS_LABEL(widget)) {
		return Text(gtk_label_get_text(GTK_LABEL(widget)));
	}
	if (GTK_IS_INSCRIPTION(widget)) {
		return Text(gtk_inscription_get_text(GTK_INSCRIPTION(widget)));
	}
	if (ADW_IS_BUTTON_CONTENT(widget)) {
		return Text(adw_button_content_get_label(ADW_BUTTON_CONTENT(widget)));
	}
	if (ADW_IS_PREFERENCES_ROW(widget)) {
		return Text(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)));
	}
	return "";
}

std::string AllText(GtkWidget* root) {
	std::string text;
	for (GtkWidget* widget: FindAll(root, [](GtkWidget* candidate) {
		     return GTK_IS_LABEL(candidate) || GTK_IS_INSCRIPTION(candidate);
	     })) {
		const std::string own = WidgetText(widget);
		if (!own.empty()) {
			text += own + "\n";
		}
	}
	return text;
}

// ---- Accessibility

std::optional<std::string> AccessibleProperty(GtkWidget* widget, GtkAccessibleProperty property) {
	if (gtk_test_accessible_has_property(GTK_ACCESSIBLE(widget), property) == FALSE) {
		return std::nullopt;
	}
	// check_property returns the actual value as text unless it equals the one passed.
	const GCharPtr actual(
	    gtk_test_accessible_check_property(GTK_ACCESSIBLE(widget), property, "\x01"));
	return std::string(actual.View());
}

std::vector<GtkWidget*> LabelledBy(GtkWidget* widget) {
	std::vector<GtkWidget*> found;
	if (gtk_test_accessible_has_relation(GTK_ACCESSIBLE(widget),
	                                     GTK_ACCESSIBLE_RELATION_LABELLED_BY) == FALSE) {
		return found;
	}
	// check_relation returns the actual value as text unless it equals the one passed (a widget
	// is never labelled by itself alone). Since GTK 4.14 the value is a NULL-terminated list.
	const GCharPtr    actual(gtk_test_accessible_check_relation(
	    GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_RELATION_LABELLED_BY, widget, nullptr));
	const std::string text(actual.View());
	GtkWidget*        root = GTK_WIDGET(gtk_widget_get_root(widget));
	if (root == nullptr) {
		root = widget;
	}
	const auto tree = FindAll(root, [](GtkWidget*) { return true; });
	for (std::size_t at = text.find("<0x"); at != std::string::npos;
	     at             = text.find("<0x", at + 3)) {
		const auto address =
		    static_cast<std::uintptr_t>(std::strtoull(text.c_str() + at + 3, nullptr, 16));
		for (GtkWidget* candidate: tree) {
			if (reinterpret_cast<std::uintptr_t>(candidate) == address) {
				found.push_back(candidate);
				break;
			}
		}
	}
	return found;
}

std::string AccessibleName(GtkWidget* widget) {
	if (auto label = AccessibleProperty(widget, GTK_ACCESSIBLE_PROPERTY_LABEL);
	    label && !label->empty()) {
		return *label;
	}
	std::string labels;
	for (GtkWidget* label: LabelledBy(widget)) {
		std::string text = WidgetText(label);
		if (text.empty()) {
			text = AccessibleProperty(label, GTK_ACCESSIBLE_PROPERTY_LABEL).value_or("");
		}
		if (!text.empty()) {
			labels += (labels.empty() ? "" : " ") + text;
		}
	}
	if (!labels.empty()) {
		return labels;
	}
	return WidgetText(widget);
}

std::vector<GtkWidget*> InteractiveControls(GtkWidget* root) {
	static constexpr GtkAccessibleRole kControls[] = {
	    GTK_ACCESSIBLE_ROLE_BUTTON,         GTK_ACCESSIBLE_ROLE_TOGGLE_BUTTON,
	    GTK_ACCESSIBLE_ROLE_CHECKBOX,       GTK_ACCESSIBLE_ROLE_RADIO,
	    GTK_ACCESSIBLE_ROLE_SWITCH,         GTK_ACCESSIBLE_ROLE_COMBO_BOX,
	    GTK_ACCESSIBLE_ROLE_TEXT_BOX,       GTK_ACCESSIBLE_ROLE_SEARCH_BOX,
	    GTK_ACCESSIBLE_ROLE_SPIN_BUTTON,    GTK_ACCESSIBLE_ROLE_SLIDER,
	    GTK_ACCESSIBLE_ROLE_LINK,           GTK_ACCESSIBLE_ROLE_TAB,
	    GTK_ACCESSIBLE_ROLE_MENU_ITEM,      GTK_ACCESSIBLE_ROLE_MENU_ITEM_CHECKBOX,
	    GTK_ACCESSIBLE_ROLE_MENU_ITEM_RADIO};
	static constexpr GtkAccessibleRole kItems[] = {
	    GTK_ACCESSIBLE_ROLE_LIST_ITEM, GTK_ACCESSIBLE_ROLE_ROW, GTK_ACCESSIBLE_ROLE_GRID_CELL};
	std::vector<GtkWidget*>                     controls;
	const std::function<void(GtkWidget*, bool)> walk = [&](GtkWidget* widget, bool reachable) {
		if (gtk_widget_get_mapped(widget) == FALSE) {
			return;
		}
		// Nothing below a widget that cannot take the focus takes it.
		reachable                    = reachable && gtk_widget_get_can_focus(widget) != FALSE;
		const GtkAccessibleRole role = gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(widget));
		const bool              control =
		    std::find(std::begin(kControls), std::end(kControls), role) != std::end(kControls);
		const bool item = std::find(std::begin(kItems), std::end(kItems), role) != std::end(kItems);
		if (control || (item && reachable && gtk_widget_get_focusable(widget) != FALSE)) {
			controls.push_back(widget);
		}
		for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
		     child            = gtk_widget_get_next_sibling(child)) {
			walk(child, reachable);
		}
	};
	if (root != nullptr) {
		walk(root, true);
	}
	return controls;
}

std::string ControlName(GtkWidget* control) {
	std::string name   = AccessibleName(control);
	GtkWidget*  parent = gtk_widget_get_parent(control);
	if (name.empty() && parent != nullptr && GTK_IS_BUTTON(control) &&
	    (GTK_IS_MENU_BUTTON(parent) || GTK_IS_DROP_DOWN(parent) ||
	     GTK_IS_COLOR_DIALOG_BUTTON(parent) || GTK_IS_FONT_DIALOG_BUTTON(parent))) {
		name = AccessibleName(parent);
	}
	return name;
}

std::string WidgetPath(GtkWidget* widget) {
	std::string path;
	for (GtkWidget* at = widget; at != nullptr; at = gtk_widget_get_parent(at)) {
		std::string segment = G_OBJECT_TYPE_NAME(at);
		const char* id      = gtk_buildable_get_buildable_id(GTK_BUILDABLE(at));
		if (id != nullptr && !g_str_has_prefix(id, "___object_")) {
			segment += std::string("#") + id;
		}
		path = path.empty() ? segment : segment + " > " + path;
	}
	return path;
}

std::string RoleName(GtkWidget* widget) {
	auto*             klass = static_cast<GEnumClass*>(g_type_class_ref(GTK_TYPE_ACCESSIBLE_ROLE));
	const GEnumValue* value =
	    g_enum_get_value(klass, gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(widget)));
	std::string name = value != nullptr ? value->value_nick : "unknown";
	g_type_class_unref(klass);
	return name;
}

std::string AtContextType(GtkWidget* widget) {
	GtkATContext* context = gtk_accessible_get_at_context(GTK_ACCESSIBLE(widget));
	if (context == nullptr) {
		return "(none)";
	}
	std::string type = G_OBJECT_TYPE_NAME(context);
	g_object_unref(context);
	return type;
}

// ---- Input

bool Activate(GtkWidget* widget) {
	if (widget == nullptr || gtk_widget_is_sensitive(widget) == FALSE) {
		return false;
	}
	if (GTK_IS_BUTTON(widget)) {
		g_signal_emit_by_name(widget, "clicked");
		return true;
	}
	return gtk_widget_activate(widget) != FALSE;
}

GtkWidget* MoveFocus(GtkWindow* window, GtkDirectionType direction) {
	g_signal_emit_by_name(window, "move-focus", direction);
	return gtk_window_get_focus(window);
}

bool KeyPress(GtkWidget* target, guint keyval, GdkModifierType modifiers) {
	if (GTK_IS_WINDOW(target) && gtk_window_get_focus(GTK_WINDOW(target)) != nullptr) {
		target = gtk_window_get_focus(GTK_WINDOW(target));
	}
	return KeyPressOn(target, keyval, modifiers);
}

bool KeyPressOn(GtkWidget* target, guint keyval, GdkModifierType modifiers) {
	// gtk_main_do_event makes a window that gets a key event show the focus.
	if (GtkRoot* root = gtk_widget_get_root(target); GTK_IS_WINDOW(root)) {
		gtk_window_set_focus_visible(GTK_WINDOW(root), TRUE);
	}
	Key key;
	key.keyval    = keyval;
	key.modifiers = modifiers;
	key.trigger = GObjectPtr<GtkShortcutTrigger>::Adopt(gtk_keyval_trigger_new(keyval, modifiers));
	GdkKeymapKey* keys  = nullptr;
	gint          count = 0;
	if (gdk_display_map_keyval(gtk_widget_get_display(target), keyval, &keys, &count) != FALSE &&
	    count > 0) {
		key.keycode = keys[0].keycode;
	}
	g_free(keys);

	// The ancestors, the root first.
	std::vector<GtkWidget*> chain;
	for (GtkWidget* widget = target; widget != nullptr; widget = gtk_widget_get_parent(widget)) {
		chain.insert(chain.begin(), widget);
	}
	const auto managed = ManagedControllers(chain.front(), chain);
	const auto runs    = [&key](const GObjectPtr<GtkEventController>& controller,
	                            GtkPropagationPhase                   phase) {
		return gtk_event_controller_get_propagation_phase(controller.get()) == phase &&
		       Run(controller.get(), key);
	};
	const auto local = [](const GObjectPtr<GtkEventController>& controller) {
		return !IsShortcutManagerController(controller.get()) &&
		       (IsLocalShortcutController(controller.get()) ||
		        GTK_IS_EVENT_CONTROLLER_KEY(controller.get()));
	};

	// Capture: the global and managed shortcuts at the root, then each ancestor down to the target.
	for (const auto& controller: managed) {
		if (runs(controller, GTK_PHASE_CAPTURE)) {
			return true;
		}
	}
	for (GtkWidget* widget: chain) {
		for (const auto& controller: Controllers(widget)) {
			if (local(controller) && runs(controller, GTK_PHASE_CAPTURE)) {
				return true;
			}
		}
	}
	// Target, then bubble up to the root, then the global and managed shortcuts at the root.
	for (const auto& controller: Controllers(target)) {
		if (local(controller) && runs(controller, GTK_PHASE_TARGET)) {
			return true;
		}
	}
	for (auto widget = chain.rbegin(); widget != chain.rend(); ++widget) {
		for (const auto& controller: Controllers(*widget)) {
			if (local(controller) && runs(controller, GTK_PHASE_BUBBLE)) {
				return true;
			}
		}
	}
	for (const auto& controller: managed) {
		if (runs(controller, GTK_PHASE_BUBBLE) || runs(controller, GTK_PHASE_TARGET)) {
			return true;
		}
	}
	return RunApplicationAccelerators(target, key);
}

GtkPopover* OpenPopover(GtkMenuButton* button) {
	GtkPopover* popover = gtk_menu_button_get_popover(button);
	if (popover == nullptr) {
		Fail("the menu button has no popover");
		return nullptr;
	}
	gtk_popover_set_autohide(popover, FALSE);
	gtk_menu_button_popup(button);
	if (!WaitFor([popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE; }, 2000,
	             "the menu button's popover to be mapped")) {
		return nullptr;
	}
	return popover;
}

// ---- Windows and alerts

guint ToplevelCount() {
	return g_list_model_get_n_items(gtk_window_get_toplevels());
}

bool IsToplevel(GtkWindow* window) {
	for (GtkWindow* candidate: Toplevels()) {
		if (candidate == window) {
			return true;
		}
	}
	return false;
}

GtkWindow* FindAlert(std::string_view text, int timeout_ms) {
	GtkWindow* found = nullptr;
	WaitFor(
	    [&]() {
		    for (GtkWindow* window: Toplevels()) {
			    if (gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE &&
			        AllText(GTK_WIDGET(window)).find(text) != std::string::npos) {
				    found = window;
				    return true;
			    }
		    }
		    return false;
	    },
	    timeout_ms, "a mapped window with the text \"" + std::string(text) + "\"");
	return found;
}

bool CloseAlert(GtkWindow* alert, std::string_view label) {
	GtkWidget* button = FindFirst(GTK_WIDGET(alert), [label](GtkWidget* widget) {
		return GTK_IS_BUTTON(widget) && WidgetText(widget) == label;
	});
	if (button == nullptr) {
		Fail("the alert has no button \"" + std::string(label) +
		     "\"; its text: " + AllText(GTK_WIDGET(alert)));
		return false;
	}
	g_signal_emit_by_name(button, "clicked");
	return WaitFor([alert]() { return !IsToplevel(alert); }, 2000,
	               "the alert to close after \"" + std::string(label) + "\"");
}

// ---- Frames and captures

bool WaitForPaintedFrame(GtkWidget* widget, int timeout_ms) {
	GdkFrameClock* clock = gtk_widget_get_frame_clock(widget);
	if (clock == nullptr) {
		Fail(std::string("a ") + G_OBJECT_TYPE_NAME(widget) +
		     " without a frame clock (not realized)");
		return false;
	}
	bool       painted = false;
	const auto scope   = SignalScope::Connect(
	    clock, "after-paint", G_CALLBACK(+[](GdkFrameClock* /*clock*/, gpointer data) {
		    *static_cast<bool*>(data) = true;
	    }),
	    &painted);
	gtk_widget_queue_draw(widget);
	return WaitFor([&painted]() { return painted; }, timeout_ms,
	               std::string("a painted frame of a ") + G_OBJECT_TYPE_NAME(widget));
}

GObjectPtr<GdkTexture> Render(GtkWindow* window, std::string_view what) {
	const std::string name(what);
	if (gtk_widget_get_mapped(GTK_WIDGET(window)) == FALSE) {
		Fail(name + ": the window is not mapped");
		return {};
	}
	// A window that was just mapped has no size until its first layout.
	if (!WaitFor(
	        [window]() {
		        return gtk_widget_get_width(GTK_WIDGET(window)) > 0 &&
		               gtk_widget_get_height(GTK_WIDGET(window)) > 0;
	        },
	        2000, name + ": the window's first layout") ||
	    !WaitForPaintedFrame(GTK_WIDGET(window))) {
		return {};
	}
	auto paintable  = GObjectPtr<GdkPaintable>::Adopt(gtk_widget_paintable_new(GTK_WIDGET(window)));
	const int width = gdk_paintable_get_intrinsic_width(paintable.get());
	const int height = gdk_paintable_get_intrinsic_height(paintable.get());
	if (width <= 0 || height <= 0) {
		Fail(name + ": the window is empty (" + std::to_string(width) + "x" +
		     std::to_string(height) + ")");
		return {};
	}
	GtkSnapshot* snapshot = gtk_snapshot_new();
	gdk_paintable_snapshot(paintable.get(), snapshot, width, height);
	GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
	if (node == nullptr) {
		Fail(name + ": nothing was drawn");
		return {};
	}
	GskRenderer*    renderer = gtk_native_get_renderer(GTK_NATIVE(window));
	graphene_rect_t bounds;
	gsk_render_node_get_bounds(node, &bounds);
	auto texture =
	    GObjectPtr<GdkTexture>::Adopt(gsk_renderer_render_texture(renderer, node, &bounds));
	gsk_render_node_unref(node);
	if (!texture) {
		Fail(name + ": the renderer made no texture");
	}
	return texture;
}

bool Capture(GtkWindow* window, std::string_view name) {
	const auto& directory = CaptureDirectory();
	if (directory.empty()) {
		return static_cast<bool>(Render(window, name));
	}
	AdwStyleManager*     style    = adw_style_manager_get_default();
	const AdwColorScheme previous = adw_style_manager_get_color_scheme(style);
	bool                 ok       = true;
	for (const bool dark: {false, true}) {
		adw_style_manager_set_color_scheme(style, dark ? ADW_COLOR_SCHEME_FORCE_DARK
		                                               : ADW_COLOR_SCHEME_FORCE_LIGHT);
		ok = WaitFor(
		         [style, dark]() { return (adw_style_manager_get_dark(style) != FALSE) == dark; },
		         2000, dark ? "the dark style" : "the light style") &&
		     ok;
		ok = WaitForPaintedFrame(GTK_WIDGET(window)) && ok;
		Pump(300);
		const std::string file =
		    CaseName() + "-" + std::string(name) + (dark ? "-dark.png" : "-light.png");
		const auto path    = directory / file;
		const auto texture = Render(window, file);
		if (!texture || gdk_texture_save_to_png(texture.get(), path.c_str()) == FALSE) {
			Fail("cannot write " + path.string());
			ok = false;
			continue;
		}
		std::printf("%s: captured %s (%dx%d)\n", CaseName().c_str(), path.c_str(),
		            gdk_texture_get_width(texture.get()), gdk_texture_get_height(texture.get()));
	}
	adw_style_manager_set_color_scheme(style, previous);
	return WaitForPaintedFrame(GTK_WIDGET(window)) && ok;
}

// ---- The fake portal

bool SetPortalColorScheme(guint32 value) {
	GErrorPtr  error;
	const auto bus = GObjectPtr<GDBusConnection>::Adopt(
	    g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, error.Out()));
	if (!bus) {
		Fail("no session bus: " + std::string(error.Message()));
		return false;
	}
	GVariant* reply = g_dbus_connection_call_sync(
	    bus.get(), "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
	    "io.github.MrG00gle.KytyPS5_GTK.TestPortal", "SetColorScheme", g_variant_new("(u)", value),
	    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, error.Out());
	if (reply == nullptr) {
		Fail("TestPortal.SetColorScheme(" + std::to_string(value) +
		     ") failed: " + std::string(error.Message()));
		return false;
	}
	g_variant_unref(reply);
	return true;
}

} // namespace KytyUi
