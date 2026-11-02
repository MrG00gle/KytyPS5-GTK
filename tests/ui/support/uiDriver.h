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
// inscription's text, an AdwPreferencesRow's title; "" for other widgets.
std::string WidgetText(GtkWidget* widget);
// Every label and inscription text below root, one per line.
std::string AllText(GtkWidget* root);

// ---- Accessibility (GTK_A11Y=test, plan D9)

// An accessible property as GTK's test API reports it, or nullopt when the widget has none.
std::optional<std::string> AccessibleProperty(GtkWidget* widget, GtkAccessibleProperty property);
// The accessible name by the rule of plan D9 for the cases here: a non-empty LABEL property, else
// the widget's own visible text (WidgetText). LABELLED_BY relations are U3's.
std::string AccessibleName(GtkWidget* widget);
// The type name of the widget's accessibility context ("GtkTestATContext" under GTK_A11Y=test).
std::string AtContextType(GtkWidget* widget);

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
// global or managed scope anywhere in the window run in the root's phases. When nothing handled the
// key and the window belongs to a GtkApplication, the application's accelerators for the key
// (gtk_application_get_actions_for_accel) activate their action. True when something handled it.
bool KeyPress(GtkWidget* target, guint keyval, GdkModifierType modifiers = GdkModifierType(0));

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

// Without a capture directory: renders window and checks that it draws something. With one
// (uiHarness.h): writes <case>-<name>-light.png (ADW_COLOR_SCHEME_FORCE_LIGHT) and
// <case>-<name>-dark.png (ADW_COLOR_SCHEME_FORCE_DARK) there, then restores the color scheme.
bool Capture(GtkWindow* window, std::string_view name);

// ---- The fake portal (tests/ui/portal/FakePortal.cpp)

// TestPortal.SetColorScheme(value) on the session bus: the portal's color-scheme becomes value
// (0 no preference, 1 prefer dark, 2 prefer light) and it emits SettingChanged.
bool SetPortalColorScheme(guint32 value);

} // namespace KytyUi
