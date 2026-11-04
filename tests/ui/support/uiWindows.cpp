#include "ui/support/uiWindows.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/logModel.h"
#include "launcher/gtk/logView.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"

#include <algorithm>
#include <optional>
#include <system_error>

namespace KytyUi {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;

bool Exists(const fs::path& path) {
	std::error_code ec;
	return fs::exists(path, ec);
}

core::GameEntry FixtureEntry(const fs::path& games, std::string_view title) {
	core::GameEntry entry;
	if (title == "Alpha Game") {
		entry.path     = games / "Alpha Game";
		entry.title_id = "PPSA01001";
	} else {
		entry.path     = games / "Beta" / "Beta Quest";
		entry.title_id = "PPSA01002";
	}
	entry.title_name = std::string(title);
	return entry;
}

GtkWindow* MappedToplevel(GType type) {
	GListModel* toplevels = gtk_window_get_toplevels();
	for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
		auto window = GObjectPtr<GtkWindow>::Adopt(
		    static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
		if (G_TYPE_CHECK_INSTANCE_TYPE(window.get(), type) &&
		    gtk_widget_get_mapped(GTK_WIDGET(window.get())) != FALSE) {
			return window.get();
		}
	}
	return nullptr;
}

GtkWindow* WaitForWindow(GType type, const std::string& what, int timeout_ms) {
	GtkWindow* found = nullptr;
	if (WaitFor(
	        [&found, type]() {
		        found = MappedToplevel(type);
		        return found != nullptr;
	        },
	        timeout_ms, what) &&
	    WaitForPaintedFrame(GTK_WIDGET(found))) {
		return found;
	}
	return nullptr;
}

bool CloseWithEscape(GtkWindow* window, const std::string& what) {
	Check(KeyPressOn(GTK_WIDGET(window), GDK_KEY_Escape), "Escape is handled in " + what);
	return WaitClosed(window, "Escape to close " + what);
}

bool WaitClosed(GtkWindow* window, const std::string& what, int timeout_ms) {
	return WaitFor([window]() { return !IsToplevel(window); }, timeout_ms, what);
}

GtkWidget* ButtonWithAction(GtkWidget* root, std::string_view action) {
	return FindFirst(root, [action](GtkWidget* widget) {
		if (!GTK_IS_BUTTON(widget) || gtk_widget_get_mapped(widget) == FALSE) {
			return false;
		}
		const char* name = gtk_actionable_get_action_name(GTK_ACTIONABLE(widget));
		return name != nullptr && action == name;
	});
}

GtkWidget* ButtonLabelled(GtkWidget* root, std::initializer_list<std::string_view> labels) {
	return FindFirst(root, [labels](GtkWidget* widget) {
		const std::string text = WidgetText(widget);
		return GTK_IS_BUTTON(widget) &&
		       std::find(labels.begin(), labels.end(), text) != labels.end();
	});
}

GtkWidget* RowTitled(GtkWidget* root, std::string_view title) {
	return FindFirst(root, [title](GtkWidget* widget) {
		return ADW_IS_PREFERENCES_ROW(widget) && WidgetText(widget) == title;
	});
}

GtkRange* ScaleNamed(GtkWidget* root, std::string_view name) {
	GtkWidget* found = FindFirst(root, [name](GtkWidget* widget) {
		return GTK_IS_SCALE(widget) && AccessibleProperty(widget, GTK_ACCESSIBLE_PROPERTY_LABEL) ==
		                                   std::optional<std::string>(std::string(name));
	});
	return found != nullptr ? GTK_RANGE(found) : nullptr;
}

bool PageShown(GtkWidget* root, const char* name) {
	auto*      stack = ADW_VIEW_STACK(FindByBuildableId(root, "stack"));
	GtkWidget* child = stack != nullptr ? adw_view_stack_get_child_by_name(stack, name) : nullptr;
	return child != nullptr &&
	       adw_view_stack_page_get_visible(adw_view_stack_get_page(stack, child)) != FALSE;
}

std::string VisiblePage(GtkWidget* root) {
	GtkWidget*  stack = FindByBuildableId(root, "stack");
	const char* name =
	    stack != nullptr ? adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(stack)) : nullptr;
	return name != nullptr ? name : "";
}

bool CloseError(GtkWindow* alert) {
	return CloseAlert(alert, ButtonLabelled(GTK_WIDGET(alert), {"_Close"}) != nullptr ? "_Close"
	                                                                                  : "Close");
}

GtkWidget* MenuItem(GtkWidget* root, std::string_view label) {
	return FindFirst(root, [label](GtkWidget* widget) {
		if (g_strcmp0(G_OBJECT_TYPE_NAME(widget), "GtkModelButton") != 0) {
			return false;
		}
		const std::string text = AllText(widget);
		return text.substr(0, text.find('\n')) == label;
	});
}

bool ClickMenuItem(GtkWidget* button, std::string_view label) {
	if (button == nullptr || !GTK_IS_MENU_BUTTON(button)) {
		return false;
	}
	GtkPopover* popover = OpenPopover(GTK_MENU_BUTTON(button));
	if (popover == nullptr) {
		return false;
	}
	GtkWidget* item = MenuItem(GTK_WIDGET(popover), label);
	if (item != nullptr) {
		g_signal_emit_by_name(item, "clicked");
	}
	if (gtk_widget_get_mapped(GTK_WIDGET(popover)) != FALSE) {
		gtk_popover_popdown(popover);
	}
	WaitFor([popover]() { return gtk_widget_get_mapped(GTK_WIDGET(popover)) == FALSE; }, 2000,
	        "the menu to close after \"" + std::string(label) + "\"");
	return item != nullptr;
}

bool ActivateGameAction(KytyMainWindow* window, const char* action) {
	GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
	if (!Check(g_action_group_get_action_enabled(actions, action) != FALSE,
	           std::string("game.") + action + " is enabled")) {
		return false;
	}
	g_action_group_activate_action(actions, action, nullptr);
	return true;
}

std::vector<std::string> LogLines(KytyMainWindow* window) {
	std::vector<std::string> lines;
	GListModel*              log = G_LIST_MODEL(kyty::gtk::MainWindowLog(window));
	for (guint i = 0; i < g_list_model_get_n_items(log); i++) {
		auto line = GObjectPtr<GtkStringObject>::Adopt(
		    static_cast<GtkStringObject*>(g_list_model_get_item(log, i)));
		lines.emplace_back(gtk_string_object_get_string(line.get()));
	}
	return lines;
}

bool HasLine(const std::vector<std::string>& lines, std::string_view line) {
	return std::find(lines.begin(), lines.end(), line) != lines.end();
}

GtkWidget* LogPane(KytyMainWindow* window) {
	return FindByBuildableId(GTK_WIDGET(window), "log_area");
}

GtkListView* LogList(KytyMainWindow* window) {
	return kyty::gtk::MainWindowLogView(window).ListView();
}

std::vector<GtkWidget*> LogRows(KytyMainWindow* window) {
	return FindAll(GTK_WIDGET(LogList(window)), [](GtkWidget* widget) {
		return GTK_IS_INSCRIPTION(widget) && gtk_widget_get_mapped(widget) != FALSE;
	});
}

bool ShowLogPane(KytyMainWindow* window, bool shown) {
	GtkWidget* pane = LogPane(window);
	if (!Check(pane != nullptr, "the main window has its log pane")) {
		return false;
	}
	if ((gtk_widget_get_visible(pane) != FALSE) != shown) {
		gtk_widget_activate_action(GTK_WIDGET(window), "app.toggle-log", nullptr);
	}
	return WaitFor([pane, shown]() { return (gtk_widget_get_mapped(pane) != FALSE) == shown; },
	               3000, shown ? "the log pane to be shown" : "the log pane to be hidden") &&
	       (!shown || WaitForPaintedFrame(pane));
}

} // namespace KytyUi
