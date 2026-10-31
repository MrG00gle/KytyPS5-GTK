#include "launcher/gtk/dialogRegistry.h"

#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gamePickerDialog.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/settingsWindow.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace kyty::gtk {
namespace {

// The alert types show a representative text: only the UI tests and the screenshot harness open
// them through the registry; the launcher's own confirmations and errors call alerts.h directly.

DialogHandle OpenConfirmationAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                   const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	// TRANSLATORS: a sample confirmation that only the UI tests and screenshots show.
	const char* heading = C_("registry sample", "Do you want to continue?");
	const char* body    = C_("registry sample", "The action cannot be undone.");
	const char* accept  = C_("registry sample", "Continue");
	handle.window       = Confirm(
	    parent, heading, body, accept, _("Cancel"), true, [](bool /*accepted*/) {},
	    handle.cancellable);
	return handle;
}

DialogHandle OpenErrorAlert(LauncherContext& /*context*/, GtkWindow* parent,
                            const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	// TRANSLATORS: a sample error that only the UI tests and screenshots show.
	const char* heading = C_("registry sample", "The action failed");
	const char* body =
	    C_("registry sample", "The file could not be written: No space left on device.");
	handle.window = ShowError(parent, heading, body, handle.cancellable);
	return handle;
}

void CloseAlert(DialogHandle& handle) {
	if (handle.cancellable != nullptr) {
		g_cancellable_cancel(handle.cancellable);
		g_object_unref(handle.cancellable);
	}
	handle = {};
}

DialogHandle Presented(GtkWindow* window) {
	gtk_window_present(window);
	return {window, nullptr};
}

DialogHandle OpenSettingsWindow(LauncherContext& context, GtkWindow* parent,
                                const DialogArgs& /*args*/) {
	return Presented(GTK_WINDOW(kyty_settings_window_new_global(context, parent)));
}

DialogHandle OpenGameSettingsWindow(LauncherContext& context, GtkWindow* parent,
                                    const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(kyty_settings_window_new_for_game(context, parent, *args.entry)));
}

// The export picker over the library, with a callback that does nothing (only the UI tests and
// the screenshot harness open it through the registry).
DialogHandle OpenGamePickerDialog(LauncherContext& context, GtkWindow* parent,
                                  const DialogArgs& /*args*/) {
	std::vector<core::GameEntry> entries;
	GListModel*                  games = context.library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		entries.push_back(kyty_game_get_entry(game.get()));
	}
	return Presented(GTK_WINDOW(
	    kyty_game_picker_dialog_new(parent, entries, _("Export game config"),
	                                _("Choose the game whose config to write to a file."),
	                                _("Export"), [](const core::GameEntry& /*entry*/) {})));
}

// Destroys the window unless it is gone already (a window may close itself, for example after
// Save).
void CloseWindow(DialogHandle& handle) {
	if (handle.window != nullptr) {
		GListModel* toplevels = gtk_window_get_toplevels();
		for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
			auto window = GObjectPtr<GtkWindow>::Adopt(
			    static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
			if (window.get() == handle.window) {
				gtk_window_destroy(handle.window);
				break;
			}
		}
	}
	handle = {};
}

constexpr DialogType kDialogTypes[] = {
    {"confirmation-alert", OpenConfirmationAlert, CloseAlert},
    {"error-alert", OpenErrorAlert, CloseAlert},
    {"settings-window", OpenSettingsWindow, CloseWindow},
    {"game-settings-window", OpenGameSettingsWindow, CloseWindow},
    {"game-picker-dialog", OpenGamePickerDialog, CloseWindow},
};

} // namespace

std::span<const DialogType> DialogTypes() {
	return kDialogTypes;
}

const DialogType* FindDialogType(std::string_view id) {
	const auto* found = std::find_if(std::begin(kDialogTypes), std::end(kDialogTypes),
	                                 [id](const DialogType& type) { return type.id == id; });
	return found != std::end(kDialogTypes) ? found : nullptr;
}

} // namespace kyty::gtk
