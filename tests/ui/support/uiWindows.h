#pragma once

// Windows, rows, buttons, alerts, and the log pane in UI cases (task 22.5; phase 7 plan section 6
// U6), shared by InventoryTests.cpp, KeyboardTests.cpp, SettingsTests.cpp, and RunTests.cpp. Main
// thread only; failures go to the running case (uiHarness.h).

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/mainWindow.h"
#include "ui/support/uiContext.h"

#include <adwaita.h>
#include <filesystem>
#include <gtk/gtk.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace KytyUi {

// ---- Files and fixture games

bool Exists(const std::filesystem::path& path);

// The GameEntry of the fixture game titled title ("Alpha Game" or "Beta Quest") below games, as
// the scan finds it (path and title ID), for the core's settings functions.
kyty::core::GameEntry FixtureEntry(const std::filesystem::path& games, std::string_view title);

// ---- Windows

// The mapped toplevel of type, or nullptr.
GtkWindow* MappedToplevel(GType type);
// Waits for a mapped and painted toplevel of type; nullptr after a Fail.
GtkWindow* WaitForWindow(GType type, const std::string& what, int timeout_ms = 5000);
// Escape (on the window itself: a GtkText of the window would need a pointer device) closes it.
bool CloseWithEscape(GtkWindow* window, const std::string& what);
// Waits until window is no toplevel any more (compared by address: it may be gone).
bool WaitClosed(GtkWindow* window, const std::string& what, int timeout_ms = 3000);

// ---- Widgets

// The mapped button below root whose action is action, or nullptr.
GtkWidget* ButtonWithAction(GtkWidget* root, std::string_view action);
// The button below root labelled with one of labels (its own text), or nullptr.
GtkWidget* ButtonLabelled(GtkWidget* root, std::initializer_list<std::string_view> labels);
// The preferences row below root titled title, or nullptr.
GtkWidget* RowTitled(GtkWidget* root, std::string_view title);
// The scale below root whose accessible label is name (its row has the same title), or nullptr.
GtkRange* ScaleNamed(GtkWidget* root, std::string_view name);
// The view stack page name is shown (it exists and its page is visible).
bool PageShown(GtkWidget* root, const char* name);
// The view stack's visible page name ("" without one).
std::string VisiblePage(GtkWidget* root);

// ---- Alerts and menus

// Closes an error alert with its Close button (GtkAlertDialog's own, "_Close" or "Close").
bool CloseError(GtkWindow* alert);
// The menu item below root (a GtkModelButton of a popover menu) whose label is label.
GtkWidget* MenuItem(GtkWidget* root, std::string_view label);
// Opens the menu button's popover (autohide off), clicks its item labelled label, and waits until
// the popover is closed again. False when the popover or the item is missing.
bool ClickMenuItem(GtkWidget* button, std::string_view label);

// ---- The main window's game actions and log

// The game's action (the game menu's item, the header bar button, and the key all activate it);
// false after a Fail when it is disabled.
bool ActivateGameAction(KytyMainWindow* window, const char* action);
// The text lines of the log pane's model.
std::vector<std::string> LogLines(KytyMainWindow* window);
bool                     HasLine(const std::vector<std::string>& lines, std::string_view line);
// The log pane (the main window's log_area box) and its list view.
GtkWidget*   LogPane(KytyMainWindow* window);
GtkListView* LogList(KytyMainWindow* window);
// The mapped row widgets (GtkInscription) of the log list, in widget order.
std::vector<GtkWidget*> LogRows(KytyMainWindow* window);
// Shows or hides the log pane through app.toggle-log, unless it already is, and waits until it is
// mapped and painted (or unmapped); false after a Fail.
bool ShowLogPane(KytyMainWindow* window, bool shown);

} // namespace KytyUi
