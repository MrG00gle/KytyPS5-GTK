#include "launcher/gtk/mainWindow.h"

#include "launcher/gtk/additionalContentWindow.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/compressionDialog.h"
#include "launcher/gtk/gameGridView.h"
#include "launcher/gtk/gameListView.h"
#include "launcher/gtk/gameViewSupport.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/inputMappingWindow.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/logModel.h"
#include "launcher/gtk/logView.h"
#include "launcher/gtk/runController.h"
#include "launcher/gtk/services/fileChooser.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/trophyOverviewWindow.h"
#include "launcher/gtk/trophyWindow.h"

#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

struct MainWindowState {
	LauncherContext* context = nullptr;
	// Where the app.* actions are: the application's action map, or own_app_actions.
	GActionMap*                        app_actions = nullptr;
	GObjectPtr<GApplication>           application; // the application the actions were added to
	GObjectPtr<GSimpleActionGroup>     own_app_actions;
	GObjectPtr<GSimpleActionGroup>     game_actions;
	std::vector<std::string>           added_app_actions;
	std::unique_ptr<GameListView>      list_view;
	std::unique_ptr<GameGridView>      grid_view;
	std::unique_ptr<LibraryController> library;
	GObjectPtr<KytyLogModel>           log;
	std::unique_ptr<LogView>           log_view;
	std::unique_ptr<RunController>     run;
	// Quitting while a game runs (D34): what runs once the game has exited, whether the
	// confirmation is open, and whether the game was asked to stop for the quit.
	std::function<void()> quit_done;
	bool                  quit_asking   = false;
	bool                  quit_stopping = false;
	SourceId              quit_idle;
	// Cancelled when the window goes away: no alert callback runs after it.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	std::vector<SignalScope> signals;
	SourceId                 icon_width_save; // 500 ms after the slider stops (D32)
	bool                     loading  = false;
	bool                     disposed = false;
};

} // namespace kyty::gtk

struct _KytyMainWindow {
	AdwApplicationWindow parent_instance;
	AdwToastOverlay*     toast_overlay;
	GtkScale*            icon_width_scale;
	GtkSearchEntry*      search_entry;
	GtkMenuButton*       game_menu_button;
	GMenuModel*          empty_space_menu;
	AdwBanner*           settings_banner;
	AdwBanner*           emulator_banner;
	AdwBanner*           scan_banner;
	GtkPaned*            log_paned;
	GtkWidget*           log_area;
	GtkStack*            content_stack;
	GtkWidget*           library_overlay;
	GtkPicture*          background_art;
	GtkWidget*           art_scrim;
	GtkStack*            views_stack;
	GtkLabel*            settings_label;
	GtkLabel*            emulator_label;
	GtkLabel*            version_label;

	kyty::gtk::MainWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyMainWindow, kyty_main_window, ADW_TYPE_APPLICATION_WINDOW)

namespace {

using kyty::gtk::MainWindowState;
namespace core = kyty::core;

constexpr const char* kTemplateResource = "/io/github/MrG00gle/KytyPS5_GTK/ui/main-window.xml";

// The app.* and game.* actions of later tasks (plan D30): registered now, enabled by their tasks.
constexpr const char* kLaterAppActions[] = {"check-updates", "open-releases-page"};
constexpr const char* kGameActions[]     = {"run",
                                            "stop",
                                            "open-folder",
                                            "trophies",
                                            "cheats",
                                            "additional-content",
                                            "compress",
                                            "remove-save-data",
                                            "edit-config",
                                            "clear-config",
                                            "input-mapping",
                                            "export-settings",
                                            "import-settings"};

struct Accelerator {
	const char* action;
	const char* keys[2];
};

// D30: the application-wide accelerators. Delete (game.clear-config) and Enter (game.run) are the
// views' own keys (D31).
constexpr Accelerator kAccelerators[] = {
    {"app.refresh", {"F5", nullptr}},
    {"app.search", {"<Control>f", nullptr}},
    {"app.toggle-grid", {"<Control>g", nullptr}},
    {"app.global-settings", {"<Control>comma", nullptr}},
    {"app.toggle-log", {"<Control>l", nullptr}},
    {"app.save-log", {"<Control>s", nullptr}},
    {"app.check-updates", {"<Control>u", nullptr}},
    {"app.quit", {"<Control>q", nullptr}},
    {"game.run", {"<Control>r", nullptr}},
    {"game.stop", {"<Control>period", nullptr}},
    {"game.open-folder", {"<Control>o", nullptr}},
    {"game.edit-config", {"<Control>e", nullptr}},
};

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	kyty::gtk::GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyMainWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

GSimpleAction* FindAction(GActionMap* map, const char* name) {
	GAction* action = map != nullptr ? g_action_map_lookup_action(map, name) : nullptr;
	return action != nullptr && G_IS_SIMPLE_ACTION(action) ? G_SIMPLE_ACTION(action) : nullptr;
}

void SetEnabled(GActionMap* map, const char* name, bool enabled) {
	if (GSimpleAction* action = FindAction(map, name); action != nullptr) {
		g_simple_action_set_enabled(action, enabled ? TRUE : FALSE);
	}
}

// What a toast's Details button shows.
struct ToastDetails {
	KytyMainWindow* window = nullptr;
	std::string     heading;
	std::string     details;
};

void OnToastDetails(AdwToast* /*toast*/, gpointer data) {
	const auto* details = static_cast<const ToastDetails*>(data);
	if (Alive(details->window)) {
		kyty::gtk::ShowError(GTK_WINDOW(details->window), details->heading, details->details,
		                     details->window->state->cancellable.get());
	}
}

// A failure the user should know about without being stopped: the heading in a toast, and the
// core's message (paths and reasons, often too long for a toast) behind its Details button.
void Toast(KytyMainWindow* self, const std::string& heading, const std::string& details) {
	AdwToast* toast = adw_toast_new(heading.c_str());
	adw_toast_set_use_markup(toast, FALSE);
	adw_toast_set_timeout(toast, 8);
	adw_toast_set_button_label(toast, _("Details"));
	// The toast belongs to the window's overlay, so it never outlives the window.
	g_signal_connect_data(
	    toast, "button-clicked", G_CALLBACK(&OnToastDetails),
	    new ToastDetails {self, heading, details},
	    [](gpointer data, GClosure* /*closure*/) { delete static_cast<ToastDetails*>(data); },
	    GConnectFlags(0));
	adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

void SaveAutomatically(KytyMainWindow* self, core::LauncherSettings next) {
	if (const auto error =
	        kyty::gtk::SaveSettingsAutomatically(*self->state->context, std::move(next))) {
		Toast(self, _("The settings could not be saved"), error->message);
	}
}

// ---- Banners, footer, content

void UpdateSettingsBanner(KytyMainWindow* self) {
	const auto& store = self->state->context->settings;
	const bool  show  = store.load_error && !store.overwrite_confirmed;
	if (show) {
		adw_banner_set_title(self->settings_banner,
		                     Format(_("The settings file could not be loaded (%s). Nothing is "
		                              "saved until you allow it."),
		                            store.load_error->message.c_str())
		                         .c_str());
	}
	adw_banner_set_revealed(self->settings_banner, show ? TRUE : FALSE);
}

void UpdateScanBanner(KytyMainWindow* self, const std::vector<core::ScanWarning>& warnings) {
	if (!warnings.empty()) {
		const auto count = static_cast<unsigned long>(warnings.size());
		adw_banner_set_title(self->scan_banner,
		                     Format(kyty::gtk::NGettext("%lu folder could not be read",
		                                                "%lu folders could not be read", count),
		                            count)
		                         .c_str());
	}
	adw_banner_set_revealed(self->scan_banner, warnings.empty() ? FALSE : TRUE);
}

void UpdateContent(KytyMainWindow* self) {
	const bool empty = self->state->context->settings.loaded.settings.game_folders.empty();
	gtk_stack_set_visible_child_name(self->content_stack, empty ? "empty" : "library");
}

void UpdateActions(KytyMainWindow* self) {
	auto&       state   = *self->state;
	KytyGame*   game    = state.context->library.Selected();
	GActionMap* actions = G_ACTION_MAP(state.game_actions.get());
	SetEnabled(actions, "edit-config", game != nullptr && !kyty_game_get_running(game));
	SetEnabled(actions, "input-mapping", game != nullptr && !kyty_game_get_running(game));
	// Tasks 21.3, 21.5: trophies and add-ons for any selected game; compression for a directory
	// entry (task 21.7 adds the running entry's rule, 11.18).
	SetEnabled(actions, "trophies", game != nullptr);
	SetEnabled(actions, "additional-content", game != nullptr);
	SetEnabled(actions, "compress",
	           game != nullptr &&
	               kyty_game_get_entry(game).kind == core::GameEntry::Kind::Directory);
	// 10.4, LFI-11: Run for no game while one runs (or an update installs), Stop only then.
	SetEnabled(actions, "run",
	           game != nullptr && state.context->run_state.CanRun(kyty_game_get_entry(game).path));
	SetEnabled(actions, "stop", state.context->run_state.CanStop());
}

void ShowArt(KytyMainWindow* self, GdkTexture* art) {
	gtk_picture_set_paintable(self->background_art, art != nullptr ? GDK_PAINTABLE(art) : nullptr);
	gtk_widget_set_visible(GTK_WIDGET(self->background_art), art != nullptr ? TRUE : FALSE);
	gtk_widget_set_visible(self->art_scrim, art != nullptr ? TRUE : FALSE);
	if (art != nullptr) {
		gtk_widget_add_css_class(self->library_overlay, "with-art");
	} else {
		gtk_widget_remove_css_class(self->library_overlay, "with-art");
	}
}

void ShowView(KytyMainWindow* self, bool grid) {
	gtk_stack_set_visible_child_name(self->views_stack, grid ? "grid" : "list");
	gtk_widget_set_visible(GTK_WIDGET(self->icon_width_scale), grid ? TRUE : FALSE);
}

// ---- Settings windows

void OnGlobalSettingsSaved(GtkWindow* /*window*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	self->state->library->SettingsSaved();
	UpdateContent(self);
	UpdateSettingsBanner(self);
	UpdateActions(self);
}

void OnGameSettingsSaved(GtkWindow* /*window*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		UpdateSettingsBanner(self);
		UpdateActions(self);
	}
}

void OpenGlobalSettings(KytyMainWindow* self, const char* page) {
	KytySettingsWindow* window =
	    kyty_settings_window_new_global(*self->state->context, GTK_WINDOW(self));
	g_signal_connect_object(window, "saved", G_CALLBACK(&OnGlobalSettingsSaved), self,
	                        GConnectFlags(0));
	if (page != nullptr) {
		kyty_settings_window_show_page(window, page);
	}
	gtk_window_present(GTK_WINDOW(window));
}

// ---- Actions

void OnRefresh(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		self->state->library->Rescan();
	}
}

void OnSearch(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
	}
}

void OnToggleGrid(GSimpleAction* action, GVariant* value, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	g_simple_action_set_state(action, value);
	const bool grid = g_variant_get_boolean(value) != FALSE;
	ShowView(self, grid);
	auto next = self->state->context->settings.loaded.settings;
	if (next.grid_view != grid) {
		next.grid_view = grid;
		SaveAutomatically(self, std::move(next));
	}
}

void OnGlobalSettings(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		OpenGlobalSettings(self, nullptr);
	}
}

// LFI-36: the global settings on their Folders page; Save applies the folders (7.1).
void OnAddGameFolder(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		OpenGlobalSettings(self, "folders");
	}
}

// Without an application (registry, harness): quit closes the window.
void OnCloseWindow(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	gtk_window_close(GTK_WINDOW(data));
}

void OnEditConfig(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	KytyGame* game = self->state->context->library.Selected();
	if (game == nullptr || kyty_game_get_running(game)) {
		return;
	}
	KytySettingsWindow* window = kyty_settings_window_new_for_game(
	    *self->state->context, GTK_WINDOW(self), kyty_game_get_entry(game));
	g_signal_connect_object(window, "saved", G_CALLBACK(&OnGameSettingsSaved), self,
	                        GConnectFlags(0));
	gtk_window_present(GTK_WINDOW(window));
}

// ---- Input mapping (task 21.2): the global mapping from the header bar, a game's from its menu

void PresentInputMapping(KytyMainWindow* self, KytyInputMappingWindow* window) {
	g_signal_connect_object(window, "saved", G_CALLBACK(&OnGameSettingsSaved), self,
	                        GConnectFlags(0));
	gtk_window_present(GTK_WINDOW(window));
}

void OnInputMapping(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		PresentInputMapping(
		    self, kyty_input_mapping_window_new_global(*self->state->context, GTK_WINDOW(self)));
	}
}

void OnGameInputMapping(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	KytyGame* game = self->state->context->library.Selected();
	if (game == nullptr || kyty_game_get_running(game)) {
		return;
	}
	PresentInputMapping(self, kyty_input_mapping_window_new_for_game(*self->state->context,
	                                                                 GTK_WINDOW(self),
	                                                                 kyty_game_get_entry(game)));
}

// ---- Trophies, add-ons, and compression (tasks 21.3, 21.5)

void OnTrophyOverview(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		gtk_window_present(
		    GTK_WINDOW(kyty_trophy_overview_window_new(*self->state->context, GTK_WINDOW(self))));
	}
}

void OnTrophies(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (KytyGame* game = Alive(self) ? self->state->context->library.Selected() : nullptr;
	    game != nullptr) {
		gtk_window_present(GTK_WINDOW(kyty_trophy_window_new(
		    *self->state->context, GTK_WINDOW(self), kyty_game_get_entry(game))));
	}
}

void OnAdditionalContent(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (KytyGame* game = Alive(self) ? self->state->context->library.Selected() : nullptr;
	    game != nullptr) {
		gtk_window_present(GTK_WINDOW(kyty_additional_content_window_new(
		    *self->state->context, GTK_WINDOW(self), kyty_game_get_entry(game))));
	}
}

// Requirement 11.11: the new archive appears in the list after a rescan.
void OnArchiveCreated(GtkWindow* /*dialog*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		self->state->library->Rescan();
	}
}

void OnCompress(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto*     self = KYTY_MAIN_WINDOW(data);
	KytyGame* game = Alive(self) ? self->state->context->library.Selected() : nullptr;
	if (game == nullptr || kyty_game_get_entry(game).kind != core::GameEntry::Kind::Directory) {
		return;
	}
	KytyCompressionDialog* dialog =
	    kyty_compression_dialog_new(GTK_WINDOW(self), kyty_game_get_entry(game));
	g_signal_connect_object(dialog, "archive-created", G_CALLBACK(&OnArchiveCreated), self,
	                        GConnectFlags(0));
	gtk_window_present(GTK_WINDOW(dialog));
}

// ---- Run, stop, and the log (task 20.5)

void OnRun(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (KytyGame* game = Alive(self) ? self->state->context->library.Selected() : nullptr;
	    game != nullptr) {
		self->state->run->Run(game);
	}
}

void OnStop(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		self->state->run->Stop();
	}
}

void ShowLog(KytyMainWindow* self, bool shown) {
	gtk_widget_set_visible(self->log_area, shown ? TRUE : FALSE);
}

void OnToggleLog(GSimpleAction* action, GVariant* value, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	g_simple_action_set_state(action, value);
	ShowLog(self, g_variant_get_boolean(value) != FALSE);
}

// Requirements 10.8, 10.13: the rows of the log view, one line each, to the chosen file; a failure
// names the file and the reason and changes nothing.
void OnSaveLog(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	self->state->context->services.file_chooser.SaveFile(
	    GTK_WINDOW(self), _("Save log"), "kyty.log",
	    [self](std::optional<std::filesystem::path> file, const std::string& error) {
		    if (!Alive(self)) {
			    return;
		    }
		    auto& state = *self->state;
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The log could not be saved"), error,
			                         state.cancellable.get());
			    return;
		    }
		    if (!file) {
			    return;
		    }
		    if (auto saved = kyty_log_model_save(state.log.get(), *file); !saved) {
			    g_message("save log: %s", saved.error().message.c_str());
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The log could not be saved"),
			                         Format(_("Cannot write %s: %s"), file->c_str(),
			                                g_strerror(saved.error().error_number)),
			                         state.cancellable.get());
			    return;
		    }
		    g_debug("save log: %u line(s) written to %s",
		            g_list_model_get_n_items(G_LIST_MODEL(state.log.get())), file->c_str());
	    },
	    self->state->cancellable.get());
}

// ---- Quitting while a game runs (D34)

gboolean OnQuitIdle(gpointer data) {
	auto* self                 = KYTY_MAIN_WINDOW(data);
	auto  done                 = std::move(self->state->quit_done);
	self->state->quit_done     = nullptr;
	self->state->quit_stopping = false;
	if (done) {
		done(); // may close and destroy the window
	}
	return G_SOURCE_REMOVE;
}

// The run state changed: the actions follow, and a quit that waited for the game goes on, from
// the main loop (it may destroy the window, which owns the run controller that calls this).
void OnRunChanged(KytyMainWindow* self) {
	if (!Alive(self)) {
		return;
	}
	UpdateActions(self);
	auto& state = *self->state;
	if (state.quit_stopping && !state.run->Running() && !state.quit_idle.Active()) {
		state.quit_idle = kyty::gtk::SourceId(g_idle_add(&OnQuitIdle, self));
	}
}

void StopForQuit(KytyMainWindow* self) {
	auto& state = *self->state;
	if (!state.run->Running()) {
		state.quit_stopping = true;
		OnRunChanged(self);
		return;
	}
	state.quit_stopping = true;
	state.run->Stop();
}

gboolean OnCloseRequest(GtkWindow* window, gpointer /*data*/) {
	auto* self = KYTY_MAIN_WINDOW(window);
	if (Alive(self) && self->state->run->Running()) {
		kyty::gtk::MainWindowRequestQuit(self, true,
		                                 [self]() { gtk_window_close(GTK_WINDOW(self)); });
		return TRUE;
	}
	kyty_main_window_save_state(self);
	return FALSE;
}

constexpr GActionEntry kAppActions[] = {
    {"refresh", OnRefresh, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"search", OnSearch, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"toggle-grid", nullptr, nullptr, "false", OnToggleGrid, {0, 0, 0}},
    {"global-settings", OnGlobalSettings, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"add-game-folder", OnAddGameFolder, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"input-mapping", OnInputMapping, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"trophy-overview", OnTrophyOverview, nullptr, nullptr, nullptr, {0, 0, 0}},
    {"toggle-log", nullptr, nullptr, "false", OnToggleLog, {0, 0, 0}},
    {"save-log", OnSaveLog, nullptr, nullptr, nullptr, {0, 0, 0}},
};
constexpr GActionEntry kOfficialActions[] = {
    {"check-updates", nullptr, nullptr, nullptr, nullptr, {0, 0, 0}},
};
constexpr GActionEntry kOtherActions[] = {
    {"open-releases-page", nullptr, nullptr, nullptr, nullptr, {0, 0, 0}},
};
constexpr GActionEntry kOwnQuitAction[] = {
    {"quit", OnCloseWindow, nullptr, nullptr, nullptr, {0, 0, 0}},
};

void AddAppActions(KytyMainWindow* self, const GActionEntry* entries, int count) {
	auto& state = *self->state;
	g_action_map_add_action_entries(state.app_actions, entries, count, self);
	for (int i = 0; i < count; i++) {
		state.added_app_actions.emplace_back(entries[i].name);
	}
}

void InstallActions(KytyMainWindow* self, GtkApplication* application) {
	auto& state = *self->state;
	if (application != nullptr) {
		state.application = kyty::gtk::GObjectPtr<GApplication>::Ref(G_APPLICATION(application));
		state.app_actions = G_ACTION_MAP(application);
	} else {
		state.own_app_actions =
		    kyty::gtk::GObjectPtr<GSimpleActionGroup>::Adopt(g_simple_action_group_new());
		state.app_actions = G_ACTION_MAP(state.own_app_actions.get());
		g_action_map_add_action_entries(state.app_actions, kOwnQuitAction,
		                                G_N_ELEMENTS(kOwnQuitAction), self);
		gtk_widget_insert_action_group(GTK_WIDGET(self), "app",
		                               G_ACTION_GROUP(state.own_app_actions.get()));
	}
	AddAppActions(self, kAppActions, G_N_ELEMENTS(kAppActions));
	switch (state.context->update_mode) {
		case kyty::updater::UpdateUiMode::Official:
			AddAppActions(self, kOfficialActions, G_N_ELEMENTS(kOfficialActions));
			break;
		case kyty::updater::UpdateUiMode::Other:
			AddAppActions(self, kOtherActions, G_N_ELEMENTS(kOtherActions));
			break;
		case kyty::updater::UpdateUiMode::Flatpak: break;
	}
	for (const char* name: kLaterAppActions) {
		SetEnabled(state.app_actions, name, false);
	}
	if (GSimpleAction* grid = FindAction(state.app_actions, "toggle-grid"); grid != nullptr) {
		g_simple_action_set_state(
		    grid, g_variant_new_boolean(state.context->settings.loaded.settings.grid_view));
	}

	state.game_actions =
	    kyty::gtk::GObjectPtr<GSimpleActionGroup>::Adopt(g_simple_action_group_new());
	for (const char* name: kGameActions) {
		auto action =
		    kyty::gtk::GObjectPtr<GSimpleAction>::Adopt(g_simple_action_new(name, nullptr));
		g_simple_action_set_enabled(action.get(), FALSE);
		g_action_map_add_action(G_ACTION_MAP(state.game_actions.get()), G_ACTION(action.get()));
	}
	const auto connect = [&state, self](const char* name, GCallback callback) {
		state.signals.push_back(kyty::gtk::SignalScope::Connect(
		    FindAction(G_ACTION_MAP(state.game_actions.get()), name), "activate", callback, self));
	};
	connect("edit-config", G_CALLBACK(&OnEditConfig));
	connect("input-mapping", G_CALLBACK(&OnGameInputMapping));
	connect("trophies", G_CALLBACK(&OnTrophies));
	connect("additional-content", G_CALLBACK(&OnAdditionalContent));
	connect("compress", G_CALLBACK(&OnCompress));
	connect("run", G_CALLBACK(&OnRun));
	connect("stop", G_CALLBACK(&OnStop));
	gtk_widget_insert_action_group(GTK_WIDGET(self), "game",
	                               G_ACTION_GROUP(state.game_actions.get()));
}

// ---- Signals

void OnSearchChanged(GtkSearchEntry* entry, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (Alive(self)) {
		self->state->context->library.SetSearchText(gtk_editable_get_text(GTK_EDITABLE(entry)));
	}
}

// LFI-05: Escape clears the search.
void OnStopSearch(GtkSearchEntry* entry, gpointer /*data*/) {
	gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

gboolean OnSaveIconWidth(gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	auto  next = self->state->context->settings.loaded.settings;
	if (const int width = self->state->grid_view->IconWidth(); next.grid_icon_width != width) {
		next.grid_icon_width = width;
		SaveAutomatically(self, std::move(next));
	}
	return G_SOURCE_REMOVE;
}

void OnIconWidthChanged(GtkRange* range, gpointer data) {
	auto* self = KYTY_MAIN_WINDOW(data);
	if (!Alive(self) || self->state->loading) {
		return;
	}
	const int value   = static_cast<int>(std::lround(gtk_range_get_value(range)));
	const int snapped = kyty::gtk::GameGridView::SnapIconWidth(value);
	if (static_cast<double>(snapped) != gtk_range_get_value(range)) {
		// Comes back with the snapped value.
		gtk_range_set_value(range, snapped);
		return;
	}
	self->state->grid_view->SetIconWidth(snapped);
	self->state->icon_width_save = kyty::gtk::SourceId(g_timeout_add(500, &OnSaveIconWidth, self));
}

void OnAllowSaving(AdwBanner* /*banner*/, gpointer data) {
	auto*       self = KYTY_MAIN_WINDOW(data);
	const auto& path = self->state->context->settings.path;
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Allow saving the settings?"),
	    Format(_("The next save replaces %s. What the launcher could not load from it is lost."),
	           path.c_str()),
	    _("Allow saving"), _("Cancel"), true,
	    [self](bool accepted) {
		    if (!accepted || !Alive(self)) {
			    return;
		    }
		    core::ConfirmOverwrite(self->state->context->settings);
		    UpdateSettingsBanner(self);
	    },
	    self->state->cancellable.get());
}

// D33: every path that could not be read, with its reason (Requirement 7.10).
void OnScanDetails(AdwBanner* /*banner*/, gpointer data) {
	auto*       self = KYTY_MAIN_WINDOW(data);
	std::string body;
	for (const auto& warning: self->state->library->Warnings()) {
		body += (body.empty() ? "" : "\n") + warning.path.string() + ": " + warning.reason;
	}
	kyty::gtk::ShowError(GTK_WINDOW(self), _("Folders that could not be read"), body,
	                     self->state->cancellable.get());
}

void BuildViews(KytyMainWindow* self) {
	auto&                              state    = *self->state;
	auto&                              context  = *state.context;
	const auto&                        settings = context.settings.loaded.settings;
	kyty::gtk::GameListView::Callbacks callbacks;
	callbacks.status_edited = [self](KytyGame* game, core::CompatibilityStatus status) {
		if (Alive(self)) {
			self->state->library->StatusEdited(game, status);
		}
	};
	callbacks.comment_edited = [self](KytyGame* game, const std::string& comment) {
		if (Alive(self)) {
			self->state->library->CommentEdited(game, comment);
		}
	};
	state.list_view = std::make_unique<kyty::gtk::GameListView>(context.library, context.local,
	                                                            std::move(callbacks));
	state.grid_view = std::make_unique<kyty::gtk::GameGridView>(context.library);
	gtk_stack_add_named(self->views_stack, state.list_view->Widget(), "list");
	gtk_stack_add_named(self->views_stack, state.grid_view->Widget(), "grid");
	// LFI-13: global settings from the context menu on empty space.
	state.list_view->SetEmptySpaceMenu(self->empty_space_menu);
	state.grid_view->SetEmptySpaceMenu(self->empty_space_menu);
	state.grid_view->SetIconWidth(settings.grid_icon_width);
	state.loading = true;
	gtk_range_set_value(GTK_RANGE(self->icon_width_scale), state.grid_view->IconWidth());
	state.loading = false;
	ShowView(self, settings.grid_view);
	auto game_menu = kyty::gtk::LoadGameMenu();
	gtk_menu_button_set_menu_model(self->game_menu_button, game_menu.get());
}

} // namespace

static void kyty_main_window_init(KytyMainWindow* self) {
	self->state = new kyty::gtk::MainWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_main_window_dispose(GObject* object) {
	auto* self = KYTY_MAIN_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		g_cancellable_cancel(state->cancellable.get());
		state->icon_width_save.Remove();
		state->quit_idle.Remove();
		state->quit_done = nullptr;
		state->signals.clear();
		// The run controller uses the library's games and the log model.
		state->run.reset();
		state->log_view.reset();
		state->log.reset();
		state->library.reset();
		// The views call the model (the context's library) and must go first.
		state->list_view.reset();
		state->grid_view.reset();
		if (state->application) {
			for (const auto& name: state->added_app_actions) {
				g_action_map_remove_action(state->app_actions, name.c_str());
			}
		}
		state->added_app_actions.clear();
		state->app_actions = nullptr;
		gtk_widget_insert_action_group(GTK_WIDGET(self), "game", nullptr);
		if (state->own_app_actions) {
			gtk_widget_insert_action_group(GTK_WIDGET(self), "app", nullptr);
		}
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_MAIN_WINDOW);
	G_OBJECT_CLASS(kyty_main_window_parent_class)->dispose(object);
}

static void kyty_main_window_finalize(GObject* object) {
	auto* self = KYTY_MAIN_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_main_window_parent_class)->finalize(object);
}

static void kyty_main_window_class_init(KytyMainWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_main_window_dispose;
	object_class->finalize = kyty_main_window_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, toast_overlay);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, icon_width_scale);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, search_entry);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, game_menu_button);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, empty_space_menu);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, settings_banner);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, emulator_banner);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, scan_banner);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, log_paned);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, log_area);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, content_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, library_overlay);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, background_art);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, art_scrim);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, views_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, settings_label);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, emulator_label);
	gtk_widget_class_bind_template_child(widget_class, KytyMainWindow, version_label);
}

KytyMainWindow* kyty_main_window_new(kyty::gtk::LauncherContext& context,
                                     GtkApplication*             application) {
	auto* self =
	    KYTY_MAIN_WINDOW(g_object_new(KYTY_TYPE_MAIN_WINDOW, "application", application, nullptr));
	auto& state          = *self->state;
	state.context        = &context;
	const auto& settings = context.settings.loaded.settings;
	gtk_window_set_default_size(GTK_WINDOW(self), settings.window.width, settings.window.height);
	if (settings.window.maximized) {
		gtk_window_maximize(GTK_WINDOW(self));
	}

	InstallActions(self, application);
	BuildViews(self);

	using kyty::gtk::SignalScope;
	state.signals.push_back(SignalScope::Connect(self->search_entry, "search-changed",
	                                             G_CALLBACK(&OnSearchChanged), self));
	state.signals.push_back(
	    SignalScope::Connect(self->search_entry, "stop-search", G_CALLBACK(&OnStopSearch), self));
	state.signals.push_back(SignalScope::Connect(self->icon_width_scale, "value-changed",
	                                             G_CALLBACK(&OnIconWidthChanged), self));
	state.signals.push_back(SignalScope::Connect(self->settings_banner, "button-clicked",
	                                             G_CALLBACK(&OnAllowSaving), self));
	state.signals.push_back(SignalScope::Connect(self->scan_banner, "button-clicked",
	                                             G_CALLBACK(&OnScanDetails), self));
	state.signals.push_back(
	    SignalScope::Connect(self, "close-request", G_CALLBACK(&OnCloseRequest), nullptr));

	if (!context.emulator.found) {
		adw_banner_set_title(
		    self->emulator_banner,
		    Format(_("Emulator not found at %s"), context.emulator.executable.c_str()).c_str());
		adw_banner_set_revealed(self->emulator_banner, TRUE);
	}
	UpdateSettingsBanner(self);
	kyty_main_window_update_footer(self);
	UpdateContent(self);

	kyty::gtk::LibraryController::Callbacks callbacks;
	callbacks.scanned = [self](const std::vector<core::ScanWarning>& warnings) {
		UpdateScanBanner(self, warnings);
	};
	callbacks.selection_changed = [self](KytyGame* /*game*/) { UpdateActions(self); };
	callbacks.background_art    = [self](GdkTexture* art) { ShowArt(self, art); };
	callbacks.notify            = [self](const std::string& heading, const std::string& details) {
		Toast(self, heading, details);
	};
	state.library = std::make_unique<kyty::gtk::LibraryController>(context, std::move(callbacks));

	// The log pane (D35), hidden until app.toggle-log shows it, and the run flow.
	state.log      = kyty::gtk::GObjectPtr<KytyLogModel>::Adopt(kyty_log_model_new());
	state.log_view = std::make_unique<kyty::gtk::LogView>(state.log.get());
	gtk_box_append(GTK_BOX(self->log_area), state.log_view->Widget());
	ShowLog(self, false);
	kyty::gtk::RunController::Callbacks run_callbacks;
	run_callbacks.changed = [self]() { OnRunChanged(self); };
	run_callbacks.notify  = [self](const std::string& heading, const std::string& details) {
		Toast(self, heading, details);
	};
	run_callbacks.error = [self](const std::string& heading, const std::string& body) {
		kyty::gtk::ShowError(GTK_WINDOW(self), heading, body, self->state->cancellable.get());
	};
	state.run = std::make_unique<kyty::gtk::RunController>(context, state.log.get(),
	                                                       std::move(run_callbacks));

	state.library->Start();
	UpdateActions(self);
	g_debug("main window: %s view, icon width %d, size %dx%d%s",
	        settings.grid_view ? "grid" : "list", state.grid_view->IconWidth(),
	        settings.window.width, settings.window.height,
	        settings.window.maximized ? ", maximized" : "");
	return self;
}

void kyty_main_window_update_footer(KytyMainWindow* self) {
	g_return_if_fail(KYTY_IS_MAIN_WINDOW(self));
	const auto& context  = *self->state->context;
	const auto  settings = Format(_("Settings file: %s"), context.paths.settings_file.c_str());
	const auto  emulator = Format(_("Emulator: %s"), context.emulator.executable.c_str());
	gtk_label_set_text(self->settings_label, settings.c_str());
	gtk_widget_set_tooltip_text(GTK_WIDGET(self->settings_label), settings.c_str());
	gtk_label_set_text(self->emulator_label, emulator.c_str());
	gtk_widget_set_tooltip_text(GTK_WIDGET(self->emulator_label), emulator.c_str());
	if (!context.emulator_version.empty()) {
		const auto version = Format(_("Version: %s"), context.emulator_version.c_str());
		gtk_label_set_text(self->version_label, version.c_str());
		gtk_widget_set_tooltip_text(GTK_WIDGET(self->version_label), version.c_str());
	}
	gtk_widget_set_visible(GTK_WIDGET(self->version_label),
	                       context.emulator_version.empty() ? FALSE : TRUE);
}

void kyty_main_window_save_state(KytyMainWindow* self) {
	g_return_if_fail(KYTY_IS_MAIN_WINDOW(self));
	if (!Alive(self)) {
		return;
	}
	auto& state = *self->state;
	auto  next  = state.context->settings.loaded.settings;
	int   width = 0, height = 0;
	gtk_window_get_default_size(GTK_WINDOW(self), &width, &height);
	if (width >= 1 && height >= 1) {
		next.window.width  = width;
		next.window.height = height;
	}
	next.window.maximized = gtk_window_is_maximized(GTK_WINDOW(self)) != FALSE;
	// A slider change that has not been saved yet.
	state.icon_width_save.Remove();
	next.grid_icon_width = state.grid_view->IconWidth();
	// Compared with what the Settings_Store holds, not with the in-memory settings: a change that
	// is only in memory (a skipped or failed automatic save, the settings window's size) is
	// written now; a session that changed nothing writes nothing.
	if (next == state.context->stored_settings) {
		return;
	}
	g_debug("main window: saving size %dx%d%s", next.window.width, next.window.height,
	        next.window.maximized ? ", maximized" : "");
	SaveAutomatically(self, std::move(next));
}

namespace kyty::gtk {

LibraryController& MainWindowLibrary(KytyMainWindow* window) {
	return *window->state->library;
}

GActionGroup* MainWindowGameActions(KytyMainWindow* window) {
	return G_ACTION_GROUP(window->state->game_actions.get());
}

RunController& MainWindowRun(KytyMainWindow* window) {
	return *window->state->run;
}

KytyLogModel* MainWindowLog(KytyMainWindow* window) {
	return window->state->log.get();
}

LogView& MainWindowLogView(KytyMainWindow* window) {
	return *window->state->log_view;
}

bool MainWindowGameRunning(KytyMainWindow* window) {
	g_return_val_if_fail(KYTY_IS_MAIN_WINDOW(window), false);
	return Alive(window) && window->state->run->Running();
}

void MainWindowRequestQuit(KytyMainWindow* window, bool ask, std::function<void()> done) {
	g_return_if_fail(KYTY_IS_MAIN_WINDOW(window));
	if (!Alive(window)) {
		return;
	}
	auto& state = *window->state;
	if (!state.run->Running() && !state.quit_stopping) {
		done();
		return;
	}
	state.quit_done = std::move(done);
	if (!ask || state.quit_stopping) {
		// SIGINT or SIGTERM, or a second request after the user agreed: no question (again).
		StopForQuit(window);
		return;
	}
	if (state.quit_asking) {
		return; // the confirmation is open
	}
	state.quit_asking = true;
	Confirm(
	    GTK_WINDOW(window), _("Stop the running game and quit?"),
	    _("The emulator is asked to close. If it is still running 5 seconds later, it is ended."),
	    _("Stop and quit"), _("Cancel"), true,
	    [window](bool accepted) {
		    if (!Alive(window)) {
			    return;
		    }
		    auto& asked       = *window->state;
		    asked.quit_asking = false;
		    if (accepted) {
			    StopForQuit(window);
		    } else if (!asked.quit_stopping) {
			    asked.quit_done = nullptr;
		    }
	    },
	    state.cancellable.get());
}

void SetMainWindowAccelerators(GtkApplication* application) {
	for (const auto& accelerator: kAccelerators) {
		gtk_application_set_accels_for_action(application, accelerator.action, accelerator.keys);
	}
}

} // namespace kyty::gtk
