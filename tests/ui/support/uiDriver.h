#pragma once

// What the UI test cases drive the GTK launcher with (task 22.1; phase 7 plan D10, D17, D33).
// Main thread only. Every wait iterates the default main context; a wait that runs out fails the
// case (uiHarness.h) with what it waited for.
//
// Input: weston's headless backend has no seat, so no real key or pointer event can be injected
// (plan G4). Keyboard cases therefore test the bindings and their handlers, not the compositor's
// delivery: KeyPress() dispatches a key the way GTK does, over the event controllers of the focus
// widget's ancestors (capture phase from the window down, then target and bubble phases up), and
// MoveFocus() emits the window's move-focus keybinding signal, which GTK binds to Tab. Pointer
// clicks are Activate(). Popovers open without autohide (an autohide popover needs a grab, which
// needs a seat). Alerts are found by their text (a new alert may get the address of a closed one)
// and closed with their buttons, never destroyed from outside.

#include "launcher/gtk/gobjectPtr.h"

#include <adwaita.h>
#include <functional>
#include <gtk/gtk.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace KytyUi {

// Iterates the default main context for milliseconds.
void Pump(int milliseconds);

// Iterates the default main context until predicate() is true (true) or timeout_ms passed (false,
// after a Fail naming what).
bool WaitFor(const std::function<bool()>& predicate, int timeout_ms, std::string_view what);

// ---- Widgets

// Every widget below root, root included, in widget-tree order, for which match is true.
std::vector<GtkWidget*> FindAll(GtkWidget* root, const std::function<bool(GtkWidget*)>& match);
// The first one, or nullptr.
GtkWidget* FindFirst(GtkWidget* root, const std::function<bool(GtkWidget*)>& match);
GtkWidget* FindByType(GtkWidget* root, GType type);
GtkWidget* FindByBuildableId(GtkWidget* root, std::string_view id);
// A widget whose own visible text is label (WidgetText).
GtkWidget* FindByLabel(GtkWidget* root, std::string_view label);
// A widget whose accessible LABEL property is label.
GtkWidget* FindByAccessibleLabel(GtkWidget* root, std::string_view label);

// A widget's own visible text: a button's, check button's, or menu button's label, a label's or
// inscription's text, the text a game view cell draws (KytyGameCell), an AdwPreferencesRow's
// title; "" for other widgets.
std::string WidgetText(GtkWidget* widget);
// Every label, inscription, and game view cell text below root, one per line.
std::string AllText(GtkWidget* root);

// ---- Accessibility (GTK_A11Y=test, plan D9)

// An accessible property as GTK's test API prints it ("true", "3", "2.5", "vertical", the string),
// or nullopt when the widget has none. GTK's test API reads the value it compares with by the
// property's own type, so the helper passes one of that type: boolean, integer, number (double),
// string, or token (an enum). A property without one of these types (GTK declares reference and
// list values only for relations, see LabelledBy) fails the case and gives nullopt (phase 8 plan
// D17, p7-u2-2).
std::optional<std::string> AccessibleProperty(GtkWidget* widget, GtkAccessibleProperty property);
// The widgets of the widget's LABELLED_BY relation that are in its window's widget tree, in
// relation order (GTK's test API prints the relation as "<type><0x<address>>..."; the addresses
// are matched against the tree, never dereferenced).
std::vector<GtkWidget*> LabelledBy(GtkWidget* widget);
// The accessible name by the rule of plan D9: a non-empty LABEL property, else the texts of the
// LABELLED_BY widgets (their own text or LABEL, joined by spaces), else the widget's own visible
// text (WidgetText).
std::string AccessibleName(GtkWidget* widget);
// The type name of the widget's accessibility context ("GtkTestATContext" under GTK_A11Y=test).
std::string AtContextType(GtkWidget* widget);

// The interactive controls below root (Requirement 6.10's list: buttons, toggle buttons, check
// buttons, radio buttons, switches, combo boxes and drop-downs, text and search entries, sliders,
// spin buttons, links, tabs, menu items, and list, row, or grid items), by their accessible role,
// in widget-tree order, mapped ones only; list, row, and grid items only when they can take the
// focus (focusable, and no ancestor with can-focus off: not the cells of a column view row, not its
// header row, not the selected value that an AdwComboRow shows).
std::vector<GtkWidget*> InteractiveControls(GtkWidget* root);
// AccessibleName(control), or, for the internal button of a GtkMenuButton, GtkDropDown,
// GtkColorDialogButton, or GtkFontDialogButton (GTK's own part of a composite control), the
// composite's accessible name.
std::string ControlName(GtkWidget* control);
// "<type>[#<buildable id>] > ..." from the root down to widget, for failure messages.
std::string WidgetPath(GtkWidget* widget);
// The accessible role's nick ("button", "toggle-button", ...).
std::string RoleName(GtkWidget* widget);

// ---- Input (plan D10)

// A click: "clicked" for a button, else gtk_widget_activate. False when nothing handled it.
bool Activate(GtkWidget* widget);

// Emits window's move-focus signal (GTK_DIR_TAB_FORWARD for Tab, GTK_DIR_TAB_BACKWARD for
// Shift+Tab) and returns the new focus widget (nullptr when none).
GtkWidget* MoveFocus(GtkWindow* window, GtkDirectionType direction);

// Dispatches keyval with modifiers (GDK_CONTROL_MASK, GDK_SHIFT_MASK, GDK_ALT_MASK) to target, or
// to the focus widget when target is a window that has one: capture phase from the root to the
// target, then target and bubble phases back to the root. A local GtkShortcutController runs a
// shortcut whose trigger equals gtk_keyval_trigger_new(keyval, modifiers)
// (gtk_shortcut_trigger_compare; alternative triggers match either side) with
// gtk_shortcut_action_activate; a GtkEventControllerKey gets key-pressed. Shortcut controllers of
// global scope anywhere in the window, and of managed scope whose shortcut manager (the window, or
// a popover the key goes through, as for a menu item's accelerator) is on the key's path, run in
// the root's phases. When nothing handled the
// key and the window belongs to a GtkApplication, the application's accelerators for the key
// (gtk_application_get_actions_for_accel) activate their action. True when something handled it.
// As GTK's gtk_main_do_event does for every key event, the window first shows the focus
// (gtk_window_set_focus_visible).
bool KeyPress(GtkWidget* target, guint keyval, GdkModifierType modifiers = GdkModifierType(0));
// KeyPress with target as the key's target even when it is a window with a focus widget. For a key
// whose binding is on an ancestor of a GtkText (an entry's Escape, a window's Escape): GtkText's
// own key handling hides the pointer, which needs a pointer device, and the headless sessions have
// none.
bool KeyPressOn(GtkWidget* target, guint keyval, GdkModifierType modifiers = GdkModifierType(0));

// Opens the menu button's popover with autohide off and waits until it is mapped.
GtkPopover* OpenPopover(GtkMenuButton* button);

// ---- Windows and alerts

[[nodiscard]] guint ToplevelCount();
// window is still one of gtk_window_get_toplevels().
bool IsToplevel(GtkWindow* window);
// Waits until a mapped toplevel whose text (AllText) contains text exists; nullptr after a Fail.
GtkWindow* FindAlert(std::string_view text, int timeout_ms = 3000);
// Clicks the alert's button labelled label and waits until the alert is gone. False (after a
// Fail) when there is no such button or the alert stays.
bool CloseAlert(GtkWindow* alert, std::string_view label);

// ---- Frames and captures (plan D33)

// Waits for the next painted frame of widget's frame clock (after-paint) after queueing a draw.
bool WaitForPaintedFrame(GtkWidget* widget, int timeout_ms = 2000);

// window as its renderer draws it (gtk_widget_paintable_new, gsk_renderer_render_texture), after
// its first layout and a painted frame; an empty pointer after a Fail when it is not mapped, gets
// no size, or draws nothing.
kyty::gtk::GObjectPtr<GdkTexture> Render(GtkWindow* window, std::string_view what);

// Render() after each painted frame until consecutive renderings keep the same pixels over painted
// frames that span at least 50 ms of frame-clock time, and the last one (phase 8 plan D15,
// p7-pr-4): GTK animations stay on, so a state change (a focus ring, a hover) may still be in its
// CSS transition at the first painted frame, the more so under CPU load. Two identical renderings
// alone do not prove it ended: the frame clock can paint two frames at the same frame time (seen
// under CPU load), at which a running transition looks the same. Prints "<case>: <what>: settled
// after <n> renders, <ms> ms (unchanged for <ms> ms of frame time, <k> repeated frame times)". An
// empty pointer after a Fail when a Render() fails or the rendering has not settled after
// timeout_ms.
kyty::gtk::GObjectPtr<GdkTexture> RenderSettled(GtkWindow* window, std::string_view what,
                                                int timeout_ms = 3000);

// Without a capture directory: renders window and checks that it draws something. With one
// (uiHarness.h): writes <case>-<name>-light.png (ADW_COLOR_SCHEME_FORCE_LIGHT) and
// <case>-<name>-dark.png (ADW_COLOR_SCHEME_FORCE_DARK) there, then restores the color scheme.
bool Capture(GtkWindow* window, std::string_view name);

// ---- The fake portal (tests/ui/portal/FakePortal.cpp)

// TestPortal.SetColorScheme(value) on the session bus: the portal's color-scheme becomes value
// (0 no preference, 1 prefer dark, 2 prefer light) and it emits SettingChanged.
bool SetPortalColorScheme(guint32 value);

} // namespace KytyUi
