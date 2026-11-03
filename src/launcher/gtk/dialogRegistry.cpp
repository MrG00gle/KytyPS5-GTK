#include "launcher/gtk/dialogRegistry.h"

#include "launcher/core/dataPaths.h"
#include "launcher/core/inputMapping.h"
#include "launcher/core/saveData.h"
#include "launcher/core/trophyModel.h"
#include "launcher/gtk/additionalContentWindow.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/bindingCaptureDialog.h"
#include "launcher/gtk/cheatsWindow.h"
#include "launcher/gtk/compressionDialog.h"
#include "launcher/gtk/gameActions.h"
#include "launcher/gtk/gamePickerDialog.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/inputLabels.h"
#include "launcher/gtk/inputMappingWindow.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/settingsWindow.h"
#include "launcher/gtk/trophyDetailsDialog.h"
#include "launcher/gtk/trophyOverviewWindow.h"
#include "launcher/gtk/trophyWindow.h"
#include "launcher/gtk/updateProgressWindow.h"
#include "launcher/gtk/updateUi.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace kyty::gtk {
namespace {

namespace fs = std::filesystem;

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

// The global input mapping editor, or the game's with DialogArgs::entry.
DialogHandle OpenInputMappingWindow(LauncherContext& context, GtkWindow* parent,
                                    const DialogArgs& args) {
	return Presented(GTK_WINDOW(
	    args.entry != nullptr ? kyty_input_mapping_window_new_for_game(context, parent, *args.entry)
	                          : kyty_input_mapping_window_new_global(context, parent)));
}

// A keyboard and mouse capture for the Cross binding, with a callback that does nothing (only the
// UI tests and the screenshot harness open it through the registry).
DialogHandle OpenBindingCaptureDialog(LauncherContext& context, GtkWindow* parent,
                                      const DialogArgs& /*args*/) {
	const core::InputControl* cross = core::FindInputControl("Cross");
	g_return_val_if_fail(cross != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(
	    kyty_binding_capture_dialog_new(context, parent, core::InputDevice::KeyboardMouse,
	                                    InputControlTitle(*cross, core::InputDevice::KeyboardMouse),
	                                    [](const std::optional<std::string>& /*input*/) {})));
}

// The trophies, add-ons, and compression of DialogArgs::entry (required).
DialogHandle OpenTrophyWindow(LauncherContext& context, GtkWindow* parent, const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(kyty_trophy_window_new(context, parent, *args.entry)));
}

DialogHandle OpenAdditionalContentWindow(LauncherContext& context, GtkWindow* parent,
                                         const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(kyty_additional_content_window_new(context, parent, *args.entry)));
}

DialogHandle OpenCompressionDialog(LauncherContext& /*context*/, GtkWindow* parent,
                                   const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(kyty_compression_dialog_new(parent, *args.entry)));
}

// The cheats of DialogArgs::entry (required).
DialogHandle OpenCheatsWindow(LauncherContext& context, GtkWindow* parent, const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	return Presented(GTK_WINDOW(kyty_cheats_window_new(context, parent, *args.entry)));
}

// A sample trophy, earned on 2026-10-30 12:34:56 UTC with a reward (only the UI tests and the
// screenshot harness open it through the registry; the trophy window opens the real ones).
DialogHandle OpenTrophyDetailsDialog(LauncherContext& /*context*/, GtkWindow* parent,
                                     const DialogArgs& /*args*/) {
	core::TrophyRow row;
	row.id       = 1;
	row.grade    = core::TrophyGrade::Gold;
	row.unlocked = true;
	// TRANSLATORS: a sample trophy that only the UI tests and screenshots show.
	row.name         = C_("registry sample", "First steps");
	row.display_name = row.name;
	// TRANSLATORS: a sample trophy description that only the UI tests and screenshots show.
	row.description = C_("registry sample", "Finish the first chapter.");
	row.has_reward  = true;
	// TRANSLATORS: a sample trophy reward that only the UI tests and screenshots show.
	row.reward         = C_("registry sample", "A profile avatar");
	row.earned_unix_ms = 1793363696000;
	return Presented(
	    GTK_WINDOW(kyty_trophy_details_dialog_new(parent, row, row, nullptr, nullptr)));
}

DialogHandle OpenTrophyOverviewWindow(LauncherContext& context, GtkWindow* parent,
                                      const DialogArgs& /*args*/) {
	return Presented(GTK_WINDOW(kyty_trophy_overview_window_new(context, parent)));
}

// The update windows (task 21.6) with sample releases (only the UI tests and the screenshot
// harness open them through the registry; the main window's UpdateUi opens the real ones): the
// offer with a callback that does nothing, the progress window at 40 % of a 48 MiB package, and
// the previous result of a successful update without a message (the default text).
constexpr const char* kSampleTag          = "2026.04.01-def5678";
constexpr const char* kSampleInstalledTag = "2026.03.01-abc1234";

DialogHandle OpenUpdateOfferAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                  const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window      = ShowUpdateOffer(
	    parent, kSampleTag, kSampleInstalledTag, [](UpdateOfferResponse /*response*/) {},
	    handle.cancellable);
	return handle;
}

DialogHandle OpenUpdateProgressWindow(LauncherContext& /*context*/, GtkWindow* parent,
                                      const DialogArgs& /*args*/) {
	KytyUpdateProgressWindow* window = kyty_update_progress_window_new(parent, kSampleTag);
	constexpr uint64_t        kSize  = uint64_t {48} << 20u;
	kyty_update_progress_window_set_downloaded(window, kSize * 2 / 5, kSize);
	return Presented(GTK_WINDOW(window));
}

DialogHandle OpenUpdateResultAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                   const DialogArgs& /*args*/) {
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window      = ShowPreviousUpdateResult(parent, {true, ""}, handle.cancellable);
	return handle;
}

// The alerts of the per-game actions (task 21.7, gameActions.h) for DialogArgs::entry (required),
// with callbacks that do nothing (only the UI tests and the screenshot harness open them through
// the registry; the main window's actions open the real ones): the Clear game config and Remove
// save data confirmations (the latter with the game's save data directory, whether it exists or
// not), the list of paths that could not be removed (sample failures below that directory), and
// the confirmation for a Game_Settings_File exported for another title.
fs::path SampleSaveDataDirectory(const LauncherContext& context, const core::GameEntry& entry) {
	return core::SaveDataPath(context.paths.data_dir, entry.title_id)
	    .value_or(core::SaveDataDirectory(context.paths.data_dir));
}

DialogHandle OpenClearConfigAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                  const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window =
	    ConfirmClearGameConfig(parent, *args.entry, [](bool /*accepted*/) {}, handle.cancellable);
	return handle;
}

DialogHandle OpenRemoveSaveDataAlert(LauncherContext& context, GtkWindow* parent,
                                     const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window      = ConfirmRemoveSaveData(
	    parent, *args.entry, {SampleSaveDataDirectory(context, *args.entry)},
	    [](bool /*accepted*/) {}, handle.cancellable);
	return handle;
}

// 40 failures (phase 7 plan D22: the list scrolls): 36 save files of one slot, then the
// directories above them.
DialogHandle OpenSaveDataFailuresAlert(LauncherContext& context, GtkWindow* parent,
                                       const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	const fs::path                    directory = SampleSaveDataDirectory(context, *args.entry);
	const fs::path                    slot      = directory / "1";
	std::vector<core::RemovalFailure> failures;
	for (int i = 1; i <= 36; i++) {
		failures.push_back({slot / ("savedata" + std::to_string(i) + ".bin"),
		                    std::make_error_code(std::errc::permission_denied)});
	}
	failures.push_back(
	    {slot / "sce_sys" / "param.sfo", std::make_error_code(std::errc::permission_denied)});
	failures.push_back({slot / "sce_sys", std::make_error_code(std::errc::directory_not_empty)});
	failures.push_back({slot, std::make_error_code(std::errc::directory_not_empty)});
	failures.push_back({directory, std::make_error_code(std::errc::directory_not_empty)});
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window      = ShowSaveDataFailures(parent, *args.entry, failures, handle.cancellable);
	return handle;
}

// The failure list is an AdwWindow (alerts.h ShowErrorList): closed with its Close button, the
// default widget, unless it is gone already; then its cancellable is released.
void CloseErrorList(DialogHandle& handle) {
	if (handle.window != nullptr) {
		GListModel* toplevels = gtk_window_get_toplevels();
		for (guint i = 0; i < g_list_model_get_n_items(toplevels); i++) {
			auto window = GObjectPtr<GtkWindow>::Adopt(
			    static_cast<GtkWindow*>(g_list_model_get_item(toplevels, i)));
			if (window.get() == handle.window) {
				if (GtkWidget* close = gtk_window_get_default_widget(handle.window);
				    close != nullptr) {
					gtk_widget_activate(close);
				}
				break;
			}
		}
	}
	CloseAlert(handle);
}

DialogHandle OpenImportSettingsAlert(LauncherContext& /*context*/, GtkWindow* parent,
                                     const DialogArgs& args) {
	g_return_val_if_fail(args.entry != nullptr, DialogHandle {});
	DialogHandle handle;
	handle.cancellable = g_cancellable_new();
	handle.window      = ConfirmForeignGameConfig(
	    parent, *args.entry, "PPSA09999", [](bool /*accepted*/) {}, handle.cancellable);
	return handle;
}

// The main window without an application (its app.* actions are then the window's own); a
// toplevel of its own, so parent is not used. It starts the library work on context.
DialogHandle OpenMainWindow(LauncherContext& context, GtkWindow* /*parent*/,
                            const DialogArgs& /*args*/) {
	return Presented(GTK_WINDOW(kyty_main_window_new(context, nullptr)));
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
    {"main-window", OpenMainWindow, CloseWindow},
    {"input-mapping-window", OpenInputMappingWindow, CloseWindow},
    {"binding-capture-dialog", OpenBindingCaptureDialog, CloseWindow},
    {"trophy-window", OpenTrophyWindow, CloseWindow},
    {"trophy-details-dialog", OpenTrophyDetailsDialog, CloseWindow},
    {"trophy-overview-window", OpenTrophyOverviewWindow, CloseWindow},
    {"additional-content-window", OpenAdditionalContentWindow, CloseWindow},
    {"compression-dialog", OpenCompressionDialog, CloseWindow},
    {"cheats-window", OpenCheatsWindow, CloseWindow},
    {"update-offer-alert", OpenUpdateOfferAlert, CloseAlert},
    {"update-progress-window", OpenUpdateProgressWindow, CloseWindow},
    {"update-result-alert", OpenUpdateResultAlert, CloseAlert},
    {"clear-config-alert", OpenClearConfigAlert, CloseAlert},
    {"remove-save-data-alert", OpenRemoveSaveDataAlert, CloseAlert},
    {"save-data-failures-alert", OpenSaveDataFailuresAlert, CloseErrorList},
    {"import-settings-alert", OpenImportSettingsAlert, CloseAlert},
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
