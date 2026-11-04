// UI tests of the settings editors (task 22.5; Requirements 7.1, 7.2, 9.1, 9.2, 9.4, 9.5, 9.8-9.10,
// 9.13-9.15, 11.15, 12.5; design.md LFI-17, LFI-18, LFI-19, LFI-22, LFI-23, LFI-47; HC-17), each on
// Wayland and on X11. Most cases run the real KytyApplication (UiContext::RunApplication) over the
// fixture games, open the global editor (app.global-settings) or a game's editor
// (game.edit-config), change it through its rows the way the user would (AdwSwitchRow, AdwComboRow,
// AdwSpinRow, AdwEntryRow, the scales, the color button, and the scripted file chooser), and check
// what Kyty.ini holds afterwards; settings_geometry and settings_load_error_overwrite set the
// Settings_Store up before the editor exists.
//
// - settings_global_fields (9.1): every Global_Configuration field of design.md's list has its row
//   on its page and every row's value round-trips through Save into Kyty.ini and back into the
//   editor; the global-only fields (controller, Game_Folders, add-ons folder) too. It and
//   settings_per_game check 9.5: each editor lists Automatic and every Vulkan device, and None and
//   every recording device, as read when it opened.
// - settings_validation (9.4): each invalid value names its field as "<Label> (<key>): <rule>",
//   keeps the editor open on the field's page with the field marked, and leaves Kyty.ini
//   byte-identical; the spin rows keep the ranged fields inside their ranges.
// - settings_dependent_fields (LFI-19): the output path rows follow their direction or switch.
// - settings_close_without_saving (9.14): Cancel, Escape, and closing the window write nothing.
// - settings_reset (LFI-18), settings_per_game (9.2, 9.13), settings_geometry (LFI-47),
//   settings_paths_display (12.5), settings_game_configs_import_export (LFI-17, 9.8-9.10, 9.15),
//   settings_folders (7.1, 7.2), settings_load_error_overwrite (HC-17), and
//   settings_controller_preview (11.15, LFI-22, LFI-23).

#include "launcher/core/atomicFile.h"
#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gamePickerDialog.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/settingsWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"
#include "ui/support/uiWindows.h"

#include <adwaita.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using KytyUi::Check;
using KytyUi::kAlphaTitle;
using KytyUi::kBetaTitle;

// ---- The editor's rows

// A Global_Configuration field that the editor shows (design.md "Global_Configuration fields
// exposed by the Qt_Launcher's global editor"): every field but elf, input_mapping_custom, and
// host_input_mapping, with its row title and page.
struct ShownField {
	std::string_view key;
	const char*      label;
	const char*      page;
};

constexpr ShownField kShownFields[] = {
    {"screen_resolution", "Screen resolution", "general"},
    {"user_name", "User name", "general"},
    {"user_id", "User ID", "general"},
    {"audio_input_device", "Microphone", "general"},
    {"present_mode", "Present mode", "general"},
    {"gpu_index", "GPU", "general"},
    {"fullscreen_enabled", "Run in fullscreen", "general"},
    {"hdr_enabled", "Enable HDR display support", "general"},
    {"hide_cursor_enabled", "Auto-hide cursor", "general"},
    {"readback_linear_images", "Enable readback", "general"},
    {"readback_tiled_images", "Enable tiled readback", "general"},
    {"sync_raw_image_buffers", "Accurate texture synchronization", "debug"},
    {"tessellation_enabled", "Enable tessellation support (experimental)", "general"},
    {"trophy_enabled", "Enable trophy notifications", "general"},
    {"trophy_duration_seconds", "Trophy notification duration (seconds)", "general"},
    {"perf_overlay_enabled", "Show performance panel", "general"},
    {"skip_notice_screen", "Skip boot logos", "general"},
    {"network_connected", "Enable network connection", "network"},
    {"vblank_frequency", "Vblank frequency", "general"},
    {"console_language", "Console language", "general"},
    {"vulkan_validation_enabled", "Vulkan validation", "debug"},
    {"shader_validation_enabled", "Shader validation", "debug"},
    {"shader_optimization_type", "Shader optimization", "general"},
    {"shader_log_direction", "Shader logging", "debug"},
    {"shader_log_folder", "Shader log folder", "debug"},
    {"command_buffer_dump_enabled", "Command buffer dump", "debug"},
    {"command_buffer_dump_folder", "Command buffer folder", "debug"},
    {"printf_direction", "Printf output", "debug"},
    {"printf_output_file", "Printf output file", "debug"},
    {"profiler_enabled", "Tracy profiler", "debug"},
    {"renderdoc_enabled", "RenderDoc capture", "debug"},
    {"amd_cpu_enabled", "AMD CPU patch (experimental)", "general"},
};

// The fields the editors keep without a row (design.md).
constexpr std::string_view kHiddenFields[] = {"elf", "input_mapping_custom", "host_input_mapping"};

// The fake devices of the GPU and microphone rows (Requirement 9.5).
const std::vector<std::string> kGpus        = {"Fake GPU A", "Fake GPU B"};
const std::vector<std::string> kMicrophones = {"Fake Microphone"};

void UseFakeDevices(KytyUi::UiContext& ui) {
	ui.Services().device_lists.vulkan.names    = kGpus;
	ui.Services().device_lists.recording.names = kMicrophones;
}

std::string Describe(const core::FieldValue& value) {
	if (const auto* flag = std::get_if<bool>(&value)) {
		return *flag ? "true" : "false";
	}
	if (const auto* number = std::get_if<int64_t>(&value)) {
		return std::to_string(*number);
	}
	if (const auto* text = std::get_if<std::string>(&value)) {
		return "\"" + *text + "\"";
	}
	return "(a list)";
}

guint ComboItems(GtkWidget* row) {
	GListModel* model = adw_combo_row_get_model(ADW_COMBO_ROW(row));
	return model != nullptr ? g_list_model_get_n_items(model) : 0;
}

std::string ComboItem(GtkWidget* row, guint index) {
	GListModel* model = adw_combo_row_get_model(ADW_COMBO_ROW(row));
	const char* text  = model != nullptr && GTK_IS_STRING_LIST(model)
	                        ? gtk_string_list_get_string(GTK_STRING_LIST(model), index)
	                        : nullptr;
	return text != nullptr ? text : "";
}

// Every item of a combo row, in the order the row lists them.
std::vector<std::string> ComboTexts(GtkWidget* row) {
	std::vector<std::string> items;
	for (guint i = 0; i < ComboItems(row); i++) {
		items.push_back(ComboItem(row, i));
	}
	return items;
}

std::string Joined(const std::vector<std::string>& items) {
	std::string text;
	for (const auto& item: items) {
		text += (text.empty() ? "\"" : ", \"") + item + "\"";
	}
	return text;
}

// The device list reads so far (Vulkan, recording), as FakeDeviceLists counts them.
std::pair<int, int> DeviceReads(KytyUi::UiContext& ui) {
	const auto& lists = ui.Services().device_lists;
	return {lists.vulkan_calls, lists.recording_calls};
}

// Requirement 9.5 in the editor root that opened since the device reads before: the editor read
// both lists once as it opened, its GPU row lists Automatic and then every fake Vulkan device in
// order, and its microphone row lists None and then every fake recording device in order.
void CheckDeviceRows(KytyUi::UiContext& ui, GtkWidget* root, std::pair<int, int> before,
                     const std::string& editor) {
	const auto reads = DeviceReads(ui);
	Check(reads.first == before.first + 1 && reads.second == before.second + 1,
	      editor + " reads the Vulkan and the recording devices once as it opens (" +
	          std::to_string(reads.first - before.first) + ", " +
	          std::to_string(reads.second - before.second) + ")");
	std::vector<std::string> gpus = {"Automatic"};
	gpus.insert(gpus.end(), kGpus.begin(), kGpus.end());
	std::vector<std::string> microphones = {"None"};
	microphones.insert(microphones.end(), kMicrophones.begin(), kMicrophones.end());
	GtkWidget* gpu = KytyUi::RowTitled(root, "GPU");
	if (Check(gpu != nullptr && ADW_IS_COMBO_ROW(gpu), editor + " has the GPU choice")) {
		Check(ComboTexts(gpu) == gpus, editor + ": the GPU row lists " + Joined(gpus) + " (" +
		                                   Joined(ComboTexts(gpu)) + ")");
	}
	GtkWidget* microphone = KytyUi::RowTitled(root, "Microphone");
	if (Check(microphone != nullptr && ADW_IS_COMBO_ROW(microphone),
	          editor + " has the Microphone choice")) {
		Check(ComboTexts(microphone) == microphones, editor + ": the Microphone row lists " +
		                                                 Joined(microphones) + " (" +
		                                                 Joined(ComboTexts(microphone)) + ")");
	}
}

// The value a row shows, as the field's value: a switch's state, a spin row's number, an entry's
// text, and a choice's index; the GPU row's device index (-1 for Automatic; the fake devices are
// listed in their index order) and the microphone row's device name ("" for None).
core::FieldValue RowValue(GtkWidget* row, std::string_view key) {
	if (ADW_IS_SWITCH_ROW(row)) {
		return adw_switch_row_get_active(ADW_SWITCH_ROW(row)) != FALSE;
	}
	if (ADW_IS_SPIN_ROW(row)) {
		adw_spin_row_update(ADW_SPIN_ROW(row));
		return static_cast<int64_t>(std::llround(adw_spin_row_get_value(ADW_SPIN_ROW(row))));
	}
	if (ADW_IS_COMBO_ROW(row)) {
		const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
		if (key == "gpu_index") {
			return selected == 0 ? int64_t {-1} : static_cast<int64_t>(selected) - 1;
		}
		if (key == "audio_input_device") {
			return selected == 0 ? std::string() : ComboItem(row, selected);
		}
		return static_cast<int64_t>(selected);
	}
	if (ADW_IS_ENTRY_ROW(row)) {
		return std::string(gtk_editable_get_text(GTK_EDITABLE(row)));
	}
	return std::string("(not an editor row)");
}

// Changes what the row shows to another valid value (a switch flips, a spin row takes its lowest
// value plus 1 or 2, a choice its last item or its first, an entry a new text) and returns the new
// value.
core::FieldValue ChangeRow(GtkWidget* row, std::string_view key) {
	if (ADW_IS_SWITCH_ROW(row)) {
		adw_switch_row_set_active(ADW_SWITCH_ROW(row),
		                          adw_switch_row_get_active(ADW_SWITCH_ROW(row)) != FALSE ? FALSE
		                                                                                  : TRUE);
	} else if (ADW_IS_SPIN_ROW(row)) {
		const double lower =
		    gtk_adjustment_get_lower(adw_spin_row_get_adjustment(ADW_SPIN_ROW(row)));
		const double now = adw_spin_row_get_value(ADW_SPIN_ROW(row));
		adw_spin_row_set_value(ADW_SPIN_ROW(row), now == lower + 1 ? lower + 2 : lower + 1);
	} else if (ADW_IS_COMBO_ROW(row)) {
		const guint last = ComboItems(row) - 1;
		adw_combo_row_set_selected(
		    ADW_COMBO_ROW(row), adw_combo_row_get_selected(ADW_COMBO_ROW(row)) == last ? 0 : last);
	} else if (ADW_IS_ENTRY_ROW(row)) {
		const std::string text = key == "user_name" ? "Field Tester" : "ui-" + std::string(key);
		gtk_editable_set_text(GTK_EDITABLE(row), text.c_str());
	}
	return RowValue(row, key);
}

// Every shown field's row of the editor (nullptr where one is missing, after a Fail).
std::vector<GtkWidget*> FieldRows(GtkWidget* root) {
	std::vector<GtkWidget*> rows;
	for (const auto& field: kShownFields) {
		GtkWidget* row = KytyUi::RowTitled(root, field.label);
		Check(row != nullptr, std::string("the editor has the row ") + field.label + " (" +
		                          std::string(field.key) + ")");
		rows.push_back(row);
	}
	return rows;
}

// The fields the rows show differ from configuration's in what (none expected).
std::string RowDifferences(GtkWidget* root, const core::GameConfiguration& configuration) {
	std::string differences;
	for (const auto& field: kShownFields) {
		GtkWidget*  row        = KytyUi::RowTitled(root, field.label);
		const auto* descriptor = core::FindGameConfigurationField(field.key);
		if (row == nullptr || descriptor == nullptr) {
			differences += " " + std::string(field.key) + " (no row)";
			continue;
		}
		const auto shown    = RowValue(row, field.key);
		const auto expected = descriptor->get(configuration);
		if (shown != expected) {
			differences += " " + std::string(field.key) + " shows " + Describe(shown) +
			               " instead of " + Describe(expected);
		}
	}
	return differences;
}

// ---- The editors in the application

GtkWindow* OpenGlobalSettings(KytyApplication* application) {
	g_action_group_activate_action(G_ACTION_GROUP(application), "global-settings", nullptr);
	return KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the global settings");
}

// The editor of the listed game titled title (game.edit-config, as Ctrl+E does).
GtkWindow* OpenGameSettings(KytyApplication* application, KytyMainWindow* window,
                            std::string_view title) {
	if (!KytyUi::SelectTitle(KytyUi::ContextOf(application).library, title) ||
	    !KytyUi::ActivateGameAction(window, "edit-config")) {
		return nullptr;
	}
	return KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW,
	                             "the game settings of " + std::string(title));
}

bool ClickButton(GtkWindow* window, const char* id) {
	return Check(KytyUi::Activate(KytyUi::FindByBuildableId(GTK_WIDGET(window), id)),
	             std::string("a click on ") + id);
}

// Save, and the editor closes.
bool SaveAndClose(GtkWindow* window, const std::string& what) {
	return ClickButton(window, "save_button") &&
	       KytyUi::WaitClosed(window, "Save to close " + what);
}

bool CancelAndClose(GtkWindow* window, const std::string& what) {
	return ClickButton(window, "cancel_button") &&
	       KytyUi::WaitClosed(window, "Cancel to close " + what);
}

// Save, then the error alert "The settings could not be saved": its text, after the alert was
// closed with its button ("" after a Fail).
std::string SaveError(GtkWindow* window) {
	if (!ClickButton(window, "save_button")) {
		return "";
	}
	GtkWindow* alert = KytyUi::FindAlert("The settings could not be saved");
	if (alert == nullptr) {
		return "";
	}
	const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
	KytyUi::CloseError(alert);
	return text;
}

std::string StoredUserName(KytyUi::UiContext& ui, const core::GameEntry& entry) {
	return core::EnableCustomSettings(KytyUi::StoredSettings(ui), entry).user_name;
}

} // namespace

// ---- 9.1: every field

KYTY_UI_CASE(settings_global_fields) {
	KytyUi::UiContext ui;
	UseFakeDevices(ui);
	const auto games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	// The editor's field list is the core's, less the three fields it keeps without a row.
	std::size_t core_fields = 0;
	for (const auto& field: core::GameConfigurationFields()) {
		const bool hidden = std::find(std::begin(kHiddenFields), std::end(kHiddenFields),
		                              field.name) != std::end(kHiddenFields);
		const bool shown  = std::find_if(std::begin(kShownFields), std::end(kShownFields),
		                                 [&field](const ShownField& s) {
			                                return s.key == field.name;
		                                 }) != std::end(kShownFields);
		Check(hidden != shown, "the field " + std::string(field.name) +
		                           " is either shown (with a row) or one of the hidden three");
		core_fields++;
	}
	Check(core_fields == std::size(kShownFields) + std::size(kHiddenFields),
	      "32 shown and 3 hidden fields are the core's " + std::to_string(core_fields));

	const fs::path  more_games = ui.Root() / "more-games";
	const fs::path  addons     = ui.Root() / "addons";
	std::error_code ec;
	fs::create_directories(more_games, ec);
	fs::create_directories(addons, ec);
	core::GameConfiguration expected = KytyUi::StoredSettings(ui).global;
	std::vector<std::pair<std::string, core::FieldValue>> chosen;
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* /*window*/) {
		auto       reads    = DeviceReads(ui);
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(settings);
		CheckDeviceRows(ui, root, reads, "the global editor");
		auto*      stack = ADW_VIEW_STACK(KytyUi::FindByBuildableId(root, "stack"));
		const auto rows  = FieldRows(root);
		for (std::size_t i = 0; i < rows.size(); i++) {
			const auto& field = kShownFields[i];
			GtkWidget*  row   = rows[i];
			if (row == nullptr) {
				continue;
			}
			GtkWidget* page = adw_view_stack_get_child_by_name(stack, field.page);
			Check(page != nullptr && gtk_widget_is_ancestor(row, page) != FALSE,
			      std::string(field.label) + " is on the page " + field.page);
			const auto* descriptor = core::FindGameConfigurationField(field.key);
			Check(RowValue(row, field.key) == descriptor->get(expected),
			      std::string(field.label) + " shows the stored value");
			const auto value = ChangeRow(row, field.key);
			Check(descriptor->set(expected, value),
			      std::string(field.label) + " takes " + Describe(value));
			chosen.emplace_back(std::string(field.key), value);
		}
		Check(RowDifferences(root, expected).empty(),
		      "the rows show every new value:" + RowDifferences(root, expected));
		// The global-only fields: the Controller page and the Folders page.
		kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "controller");
		GtkWidget* color  = KytyUi::FindByType(root, GTK_TYPE_COLOR_DIALOG_BUTTON);
		GtkRange*  shake  = KytyUi::ScaleNamed(root, "Vibration intensity");
		GtkRange*  volume = KytyUi::ScaleNamed(root, "Speaker volume");
		GtkWidget* player = KytyUi::RowTitled(root, "Keyboard player");
		if (Check(color != nullptr && shake != nullptr && volume != nullptr && player != nullptr,
		          "the Controller page has the lightbar color, vibration, speaker, and keyboard "
		          "player")) {
			GdkRGBA green {};
			gdk_rgba_parse(&green, "#00ff00");
			gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color), &green);
			gtk_range_set_value(shake, 30);
			gtk_range_set_value(volume, 70);
			adw_combo_row_set_selected(ADW_COMBO_ROW(player), 3);
		}
		kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "folders");
		ui.Services().file_chooser.folder_answers.push_back({more_games});
		Check(KytyUi::Activate(KytyUi::ButtonLabelled(root, {"Add folders…"})),
		      "a click on Add folders…");
		KytyUi::WaitFor(
		    [&]() { return KytyUi::AllText(root).find(more_games.string()) != std::string::npos; },
		    3000, "the chosen folder to be listed");
		GtkWidget* addons_row = KytyUi::RowTitled(root, "Add-ons folder");
		if (Check(addons_row != nullptr, "the Folders page has the add-ons folder")) {
			gtk_editable_set_text(GTK_EDITABLE(addons_row), addons.c_str());
		}
		Check(KytyUi::Capture(settings, "folders"), "the Folders page renders");
		SaveAndClose(settings, "the global settings");

		// Back into the editor: every row shows what was saved.
		reads    = DeviceReads(ui);
		settings = OpenGlobalSettings(application);
		if (settings != nullptr) {
			CheckDeviceRows(ui, GTK_WIDGET(settings), reads, "the reopened global editor");
			const std::string differences = RowDifferences(GTK_WIDGET(settings), expected);
			Check(differences.empty(), "the editor opens with every saved value:" + differences);
			CancelAndClose(settings, "the global settings");
		}
	});
	const auto stored = KytyUi::StoredSettings(ui);
	for (const auto& [key, value]: chosen) {
		const auto* descriptor = core::FindGameConfigurationField(key);
		Check(descriptor->get(stored.global) == value,
		      "Kyty.ini has " + key + " = " + Describe(value) + " (" +
		          Describe(descriptor->get(stored.global)) + ")");
	}
	Check(stored.global == expected, "Kyty.ini's Global_Configuration is the edited one");
	Check(g_ascii_strcasecmp(stored.controller.color.c_str(), "#00ff00") == 0 &&
	          stored.controller.vibration_intensity == 30 &&
	          stored.controller.speaker_volume == 70 && stored.controller.keyboard_player == 3,
	      "Kyty.ini has the controller settings (" + stored.controller.color + ", " +
	          std::to_string(stored.controller.vibration_intensity) + ", " +
	          std::to_string(stored.controller.speaker_volume) + ", " +
	          std::to_string(stored.controller.keyboard_player) + ")");
	Check(stored.game_folders == std::vector<std::string> {games.string(), more_games.string()},
	      "Kyty.ini has both Game_Folders");
	Check(stored.addons_dir == addons.string(), "Kyty.ini has the add-ons folder");
}

// ---- 9.4: validation

KYTY_UI_CASE(settings_validation) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const std::string before = KytyUi::ReadText(ui.SettingsFile());
	const std::string user_name_rule =
	    "User name (user_name): The user name must contain 1 to 16 UTF-8 bytes after leading and "
	    "trailing spaces are removed.";
	const std::string user_id_rule = "User ID (user_id): The user ID must be between 0 and "
	                                 "2147482879 and must not be 254 (everyone) or 255 (system).";
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(settings);
		GtkWidget* name = KytyUi::RowTitled(root, "User name");
		GtkWidget* id   = KytyUi::RowTitled(root, "User ID");
		if (!Check(name != nullptr && id != nullptr, "the User name and User ID rows")) {
			return;
		}
		// One invalid value at a time: the error names the field, the editor stays open on the
		// field's page with the field marked, and Kyty.ini is not touched.
		struct Invalid {
			const char*           what;
			const char*           label; // the row that gets the error mark
			const char*           page;
			std::string           message;
			std::function<void()> apply;
			std::function<void()> undo;
		};
		const auto set_text = [root](const char* label, const char* text) {
			return [root, label, text]() {
				gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(root, label)), text);
			};
		};
		// A choice or a switch takes the focus first, as when the user picks it: changing it can
		// turn its output path row off, and a focused entry in a row that turns insensitive gets
		// no focus-out (its cursor blink then warns, GtkText "did not receive a focus-out event").
		const auto choose = [root](const char* label, guint index) {
			return [root, label, index]() {
				GtkWidget* row = KytyUi::RowTitled(root, label);
				gtk_widget_grab_focus(row);
				adw_combo_row_set_selected(ADW_COMBO_ROW(row), index);
			};
		};
		const auto turn = [root](const char* label, bool on) {
			return [root, label, on]() {
				GtkWidget* row = KytyUi::RowTitled(root, label);
				gtk_widget_grab_focus(row);
				adw_switch_row_set_active(ADW_SWITCH_ROW(row), on ? TRUE : FALSE);
			};
		};
		const auto spin = [id](double value) {
			return [id, value]() { adw_spin_row_set_value(ADW_SPIN_ROW(id), value); };
		};
		const std::string shader_rule = "Shader log folder (shader_log_folder): The shader log "
		                                "folder must not be empty while shader logs go to a file.";
		const std::string dump_rule =
		    "Command buffer folder (command_buffer_dump_folder): The command buffer folder must "
		    "not be empty while command buffer dumps are enabled.";
		const std::string printf_rule =
		    "Printf output file (printf_output_file): The printf output file must not be empty "
		    "while printf output goes to a file.";
		const std::vector<Invalid> cases = {
		    {"an empty user name", "User name", "general", user_name_rule,
		     set_text("User name", ""), set_text("User name", "Kyty")},
		    {"a user name of spaces only", "User name", "general", user_name_rule,
		     set_text("User name", "   "), set_text("User name", "Kyty")},
		    {"a user name of 17 bytes", "User name", "general", user_name_rule,
		     set_text("User name", "ABCDEFGHIJKLMNOPQ"), set_text("User name", "Kyty")},
		    {"a user name of 9 two-byte characters (18 bytes)", "User name", "general",
		     user_name_rule, set_text("User name", "ÄÄÄÄÄÄÄÄÄ"), set_text("User name", "Kyty")},
		    {"the user ID 254", "User ID", "general", user_id_rule, spin(254), spin(1000)},
		    {"the user ID 255", "User ID", "general", user_id_rule, spin(255), spin(1000)},
		    {"an empty shader log folder with shader logs to a file", "Shader log folder", "debug",
		     shader_rule,
		     [=]() {
			     choose("Shader logging", 2)();
			     set_text("Shader log folder", "")();
		     },
		     choose("Shader logging", 0)},
		    {"an empty command buffer folder with dumps on", "Command buffer folder", "debug",
		     dump_rule,
		     [=]() {
			     turn("Command buffer dump", true)();
			     set_text("Command buffer folder", "")();
		     },
		     turn("Command buffer dump", false)},
		    {"an empty printf output file with printf output to a file", "Printf output file",
		     "debug", printf_rule,
		     [=]() {
			     choose("Printf output", 2)();
			     set_text("Printf output file", "")();
		     },
		     choose("Printf output", 0)},
		};
		bool captured = false;
		for (const auto& invalid: cases) {
			// Start from another page, so that the editor has to show the field's page.
			kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "folders");
			invalid.apply();
			const std::string text = SaveError(settings);
			Check(text.find(invalid.message) != std::string::npos,
			      std::string("the error for ") + invalid.what + " is \"" + invalid.message +
			          "\" (the alert says: " + text + ")");
			GtkWidget* marked = KytyUi::RowTitled(root, invalid.label);
			Check(KytyUi::IsToplevel(settings) && marked != nullptr &&
			          gtk_widget_has_css_class(marked, "error") != FALSE &&
			          KytyUi::VisiblePage(root) == invalid.page,
			      std::string("the editor stays open on the page ") + invalid.page +
			          " with the field marked, for " + invalid.what);
			Check(KytyUi::ReadText(ui.SettingsFile()) == before,
			      std::string("Kyty.ini is byte-identical after ") + invalid.what);
			if (!captured) {
				Check(KytyUi::Capture(settings, "editor-error"), "the marked field renders");
				captured = true;
			}
			invalid.undo();
		}
		// The alert as the user sees it.
		set_text("User name", "")();
		if (ClickButton(settings, "save_button")) {
			if (GtkWindow* alert = KytyUi::FindAlert("The settings could not be saved");
			    alert != nullptr) {
				Check(KytyUi::Capture(alert, "error-alert"), "the error alert renders");
				KytyUi::CloseError(alert);
			}
		}
		set_text("User name", "Kyty")();
		// The ranged fields cannot be set outside their ranges in the editor (the spin rows clamp).
		for (const auto& [label, low, high]:
		     {std::tuple<const char*, double, double> {"Vblank frequency", 30, 360},
		      std::tuple<const char*, double, double> {"Trophy notification duration (seconds)", 5,
		                                               10},
		      std::tuple<const char*, double, double> {"User ID", 0, core::kMaxUserId}}) {
			GtkWidget* row = KytyUi::RowTitled(root, label);
			if (!Check(row != nullptr && ADW_IS_SPIN_ROW(row), std::string("the row ") + label)) {
				continue;
			}
			adw_spin_row_set_value(ADW_SPIN_ROW(row), low - 1);
			Check(adw_spin_row_get_value(ADW_SPIN_ROW(row)) == low,
			      std::string(label) + " stops at " + std::to_string(low));
			adw_spin_row_set_value(ADW_SPIN_ROW(row), high + 1);
			Check(adw_spin_row_get_value(ADW_SPIN_ROW(row)) == high,
			      std::string(label) + " stops at " + std::to_string(high));
		}
		CancelAndClose(settings, "the global settings");

		// The game's editor validates the same way and creates no custom settings.
		GtkWindow* game = OpenGameSettings(application, window, kAlphaTitle);
		if (game != nullptr) {
			gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(GTK_WIDGET(game), "User name")),
			                      "");
			const std::string text = SaveError(game);
			Check(text.find(user_name_rule) != std::string::npos,
			      "the game's editor names the field too (" + text + ")");
			Check(KytyUi::IsToplevel(game), "the game's editor stays open");
			CancelAndClose(game, "the game settings");
		}
	});
	Check(KytyUi::ReadText(ui.SettingsFile()) == before,
	      "Kyty.ini is byte-identical at the end (nothing invalid was stored)");
	Check(!core::HasCustomSettings(KytyUi::StoredSettings(ui),
	                               KytyUi::FixtureEntry(games, kAlphaTitle)),
	      "Alpha Game has no custom settings");
}

// ---- LFI-19: dependent fields

KYTY_UI_CASE(settings_dependent_fields) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.global.shader_log_direction        = core::LogDirection::File;
		    settings.global.printf_direction            = core::LogDirection::Console;
		    settings.global.command_buffer_dump_enabled = true;
	    })) {
		return;
	}
	struct Dependent {
		const char* control;   // the direction or switch row
		const char* dependent; // the output path row
	};
	constexpr Dependent kDirections[] = {{"Shader logging", "Shader log folder"},
	                                     {"Printf output", "Printf output file"}};
	const auto          sensitive     = [](GtkWidget* root, const char* label) {
		GtkWidget* row = KytyUi::RowTitled(root, label);
		return row != nullptr && gtk_widget_get_sensitive(row) != FALSE;
	};
	const auto check_rows = [&](GtkWindow* window, const std::string& editor) {
		GtkWidget* root = GTK_WIDGET(window);
		for (const auto& [control, dependent]: kDirections) {
			GtkWidget* row = KytyUi::RowTitled(root, control);
			if (!Check(row != nullptr && ADW_IS_COMBO_ROW(row), editor + ": the row " + control)) {
				continue;
			}
			for (guint direction = 0; direction < 3; direction++) {
				adw_combo_row_set_selected(ADW_COMBO_ROW(row), direction);
				Check(sensitive(root, dependent) == (direction == 2),
				      editor + ": " + dependent + " is " + (direction == 2 ? "on" : "off") +
				          " with " + control + " " + ComboItem(row, direction));
			}
		}
		GtkWidget* dump = KytyUi::RowTitled(root, "Command buffer dump");
		if (Check(dump != nullptr && ADW_IS_SWITCH_ROW(dump), editor + ": the dump switch")) {
			for (const bool on: {false, true, false}) {
				adw_switch_row_set_active(ADW_SWITCH_ROW(dump), on ? TRUE : FALSE);
				Check(sensitive(root, "Command buffer folder") == on,
				      editor + ": the command buffer folder is " + (on ? "on" : "off") +
				          " with the dump " + (on ? "on" : "off"));
			}
		}
	};
	const std::string before = KytyUi::ReadText(ui.SettingsFile());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings != nullptr) {
			GtkWidget* root = GTK_WIDGET(settings);
			Check(sensitive(root, "Shader log folder") && !sensitive(root, "Printf output file") &&
			          sensitive(root, "Command buffer folder"),
			      "the editor opens with the stored directions: shader log folder on (File), "
			      "printf output file off (Console), command buffer folder on (dump on)");
			check_rows(settings, "global");
			CancelAndClose(settings, "the global settings");
		}
		GtkWindow* game = OpenGameSettings(application, window, kAlphaTitle);
		if (game != nullptr) {
			Check(sensitive(GTK_WIDGET(game), "Shader log folder") &&
			          !sensitive(GTK_WIDGET(game), "Printf output file"),
			      "the game's editor opens with the global directions (the game has no own "
			      "settings)");
			check_rows(game, "per game");
			CancelAndClose(game, "the game settings");
		}
	});
	Check(KytyUi::ReadText(ui.SettingsFile()) == before, "Kyty.ini is byte-identical");
}

// ---- 9.14: closing without saving

KYTY_UI_CASE(settings_close_without_saving) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	const auto        alpha = KytyUi::FixtureEntry(games, kAlphaTitle);
	if (games.empty() || !KytyUi::StoreSettings(ui, [&alpha](core::LauncherSettings& settings) {
		    settings.global.user_name       = "Before";
		    settings.controller.color       = "#102030";
		    auto configuration              = core::EnableCustomSettings(settings, alpha);
		    configuration.user_name         = "Alpha Before";
		    configuration.network_connected = true;
		    Check(static_cast<bool>(core::SaveCustomSettings(settings, alpha, configuration)),
		          "give Alpha Game its own settings");
	    })) {
		return;
	}
	const std::string before          = KytyUi::ReadText(ui.SettingsFile());
	const auto        before_settings = KytyUi::StoredSettings(ui);
	// Changes in every kind of row, on several pages.
	const auto edit = [&ui](GtkWindow* window, bool global) {
		GtkWidget* root = GTK_WIDGET(window);
		gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(root, "User name")), "Unsaved");
		ChangeRow(KytyUi::RowTitled(root, "Run in fullscreen"), "fullscreen_enabled");
		ChangeRow(KytyUi::RowTitled(root, "Screen resolution"), "screen_resolution");
		ChangeRow(KytyUi::RowTitled(root, "Vblank frequency"), "vblank_frequency");
		ChangeRow(KytyUi::RowTitled(root, "Enable network connection"), "network_connected");
		ChangeRow(KytyUi::RowTitled(root, "Printf output"), "printf_direction");
		if (!global) {
			return;
		}
		gtk_range_set_value(KytyUi::ScaleNamed(root, "Vibration intensity"), 10);
		GdkRGBA color {};
		gdk_rgba_parse(&color, "#abcdef");
		gtk_color_dialog_button_set_rgba(
		    GTK_COLOR_DIALOG_BUTTON(KytyUi::FindByType(root, GTK_TYPE_COLOR_DIALOG_BUTTON)),
		    &color);
		gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(root, "Add-ons folder")),
		                      (ui.Root() / "unsaved-addons").c_str());
		GtkWidget* list = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_LIST_BOX(widget) &&
			       gtk_list_box_get_selection_mode(GTK_LIST_BOX(widget)) == GTK_SELECTION_MULTIPLE;
		});
		if (list != nullptr) {
			gtk_list_box_select_all(GTK_LIST_BOX(list));
			KytyUi::Activate(KytyUi::ButtonLabelled(root, {"Remove"}));
		}
	};
	const auto unchanged = [&](KytyApplication* application, const std::string& how) {
		Check(KytyUi::ReadText(ui.SettingsFile()) == before,
		      "Kyty.ini is byte-identical after " + how);
		const auto& settings = KytyUi::ContextOf(application).settings.loaded.settings;
		Check(settings.global == before_settings.global &&
		          settings.controller == before_settings.controller &&
		          settings.game_folders == before_settings.game_folders &&
		          settings.addons_dir == before_settings.addons_dir &&
		          core::EnableCustomSettings(settings, alpha) ==
		              core::EnableCustomSettings(before_settings, alpha),
		      "the launcher's settings are unchanged after " + how);
	};
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		using Close = std::function<bool(GtkWindow*, const std::string&)>;
		const std::vector<std::pair<std::string, Close>> closes = {
		    {"Cancel", CancelAndClose},
		    {"Escape", KytyUi::CloseWithEscape},
		    {"closing the window",
		     [](GtkWindow* window, const std::string& what) {
			     gtk_window_close(window);
			     return KytyUi::WaitClosed(window, "closing " + what);
		     }},
		};
		for (const auto& [how, close]: closes) {
			GtkWindow* settings = OpenGlobalSettings(application);
			if (settings != nullptr) {
				edit(settings, true);
				close(settings, "the global settings");
				unchanged(application, how + " in the global editor");
			}
			GtkWindow* game = OpenGameSettings(application, window, kAlphaTitle);
			if (game != nullptr) {
				Check(std::string(gtk_editable_get_text(GTK_EDITABLE(
				          KytyUi::RowTitled(GTK_WIDGET(game), "User name")))) == "Alpha Before",
				      "the game's editor shows its stored user name");
				edit(game, false);
				close(game, "the game settings");
				unchanged(application, how + " in the game's editor");
			}
		}
	});
	Check(KytyUi::ReadText(ui.SettingsFile()) == before,
	      "Kyty.ini is byte-identical after the application quit");
}

// ---- LFI-18: Reset

KYTY_UI_CASE(settings_reset) {
	KytyUi::UiContext ui;
	const auto        games  = KytyUi::PrepareLibrary(ui);
	const auto        alpha  = KytyUi::FixtureEntry(games, kAlphaTitle);
	const fs::path    addons = ui.Root() / "addons";
	if (games.empty() || !KytyUi::StoreSettings(ui, [&](core::LauncherSettings& settings) {
		    settings.global.user_name               = "Global Name";
		    settings.global.fullscreen_enabled      = true;
		    settings.global.vblank_frequency        = 120;
		    settings.global.shader_log_direction    = core::LogDirection::File;
		    settings.controller.vibration_intensity = 40;
		    settings.controller.speaker_volume      = 80;
		    settings.controller.keyboard_player     = 2;
		    settings.controller.color               = "#123456";
		    settings.addons_dir                     = addons.string();
		    auto configuration                      = core::EnableCustomSettings(settings, alpha);
		    configuration.user_name                 = "Alpha Own";
		    configuration.hdr_enabled               = true;
		    Check(static_cast<bool>(core::SaveCustomSettings(settings, alpha, configuration)),
		          "give Alpha Game its own settings");
	    })) {
		return;
	}
	const auto global = KytyUi::StoredSettings(ui).global;
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto&      controller = ui.Services().controller;
		GtkWindow* settings   = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root   = GTK_WIDGET(settings);
		GtkWidget* reset  = KytyUi::FindByBuildableId(root, "reset_button");
		const auto before = KytyUi::ReadText(ui.SettingsFile());
		Check(reset != nullptr &&
		          g_strcmp0(gtk_widget_get_tooltip_text(reset), "Restore default settings") == 0,
		      "the global editor's Reset says \"Restore default settings\"");
		Check(RowDifferences(root, global).empty(), "the editor opens with the stored values");
		Check(KytyUi::Activate(reset), "a click on Reset");
		const std::string differences = RowDifferences(root, core::GameConfiguration {});
		Check(differences.empty(), "Reset shows every default value:" + differences);
		Check(gtk_range_get_value(KytyUi::ScaleNamed(root, "Vibration intensity")) == 100 &&
		          gtk_range_get_value(KytyUi::ScaleNamed(root, "Speaker volume")) == 50 &&
		          adw_combo_row_get_selected(
		              ADW_COMBO_ROW(KytyUi::RowTitled(root, "Keyboard player"))) == 0,
		      "Reset shows the controller defaults");
		Check(controller.Lightbar() == std::nullopt,
		      "Reset previews the default lightbar (the game controls it)");
		Check(KytyUi::AllText(root).find(games.string()) != std::string::npos &&
		          addons.string() == gtk_editable_get_text(
		                                 GTK_EDITABLE(KytyUi::RowTitled(root, "Add-ons folder"))),
		      "Reset keeps the Game_Folders and the add-ons folder");
		Check(KytyUi::ReadText(ui.SettingsFile()) == before, "Reset writes nothing by itself");
		SaveAndClose(settings, "the global settings");
		const auto saved = KytyUi::StoredSettings(ui);
		Check(saved.global == core::GameConfiguration {} &&
		          saved.controller == core::ControllerSettings {},
		      "Save after Reset stores the defaults");
		Check(saved.game_folders == std::vector<std::string> {games.string()} &&
		          saved.addons_dir == addons.string(),
		      "and keeps the Game_Folders and the add-ons folder");
		Check(core::EnableCustomSettings(saved, alpha).user_name == "Alpha Own",
		      "the global Reset leaves Alpha Game's own settings");

		// The game's Reset shows the Global_Configuration (now the defaults).
		GtkWindow* game = OpenGameSettings(application, window, kAlphaTitle);
		if (game == nullptr) {
			return;
		}
		GtkWidget* game_root  = GTK_WIDGET(game);
		GtkWidget* game_reset = KytyUi::FindByBuildableId(game_root, "reset_button");
		Check(game_reset != nullptr && g_strcmp0(gtk_widget_get_tooltip_text(game_reset),
		                                         "Set every field to the global settings") == 0,
		      "the game's Reset says \"Set every field to the global settings\"");
		Check(std::string(gtk_editable_get_text(
		          GTK_EDITABLE(KytyUi::RowTitled(game_root, "User name")))) == "Alpha Own",
		      "the game's editor opens with its own settings");
		Check(KytyUi::Activate(game_reset), "a click on the game's Reset");
		const std::string game_differences = RowDifferences(game_root, saved.global);
		Check(game_differences.empty(),
		      "the game's Reset shows the Global_Configuration:" + game_differences);
		SaveAndClose(game, "the game settings");
	});
	const auto stored             = KytyUi::StoredSettings(ui);
	auto       expected           = stored.global;
	expected.input_mapping_custom = false;
	Check(core::HasCustomSettings(stored, alpha) &&
	          core::EnableCustomSettings(stored, alpha) == expected,
	      "Alpha Game's own settings are the Global_Configuration after its Reset and Save");
}

// ---- 9.2, 9.13: the game's editor

KYTY_UI_CASE(settings_per_game) {
	KytyUi::UiContext ui;
	UseFakeDevices(ui);
	const auto games = KytyUi::PrepareLibrary(ui);
	const auto alpha = KytyUi::FixtureEntry(games, kAlphaTitle);
	const auto beta  = KytyUi::FixtureEntry(games, kBetaTitle);
	if (games.empty() || !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.global.user_name          = "Global Name";
		    settings.global.fullscreen_enabled = true;
		    settings.global.vblank_frequency   = 120;
		    settings.global.console_language   = 4;
	    })) {
		return;
	}
	const auto global = KytyUi::StoredSettings(ui).global;
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		GActionGroup* actions = kyty::gtk::MainWindowGameActions(window);
		KytyUi::SelectTitle(KytyUi::ContextOf(application).library, kAlphaTitle);
		Check(g_action_group_get_action_enabled(actions, "clear-config") == FALSE,
		      "Clear game config is off for a game without its own settings");
		auto       reads = DeviceReads(ui);
		GtkWindow* game  = OpenGameSettings(application, window, kAlphaTitle);
		if (game == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(game);
		CheckDeviceRows(ui, root, reads, "the game's editor");
		Check(g_strcmp0(gtk_window_get_title(game), "Edit game config") == 0,
		      "the window is titled Edit game config");
		const std::string text = KytyUi::AllText(root);
		Check(text.find("Alpha Game (PPSA01001)") != std::string::npos &&
		          text.find("This game uses the global settings. Saving gives it its own "
		                    "settings.") != std::string::npos,
		      "the editor names the game and says that it uses the global settings");
		const std::string differences = RowDifferences(root, global);
		Check(differences.empty(),
		      "every field opens with the Global_Configuration's value (9.2):" + differences);
		Check(KytyUi::PageShown(root, "general") && KytyUi::PageShown(root, "network") &&
		          KytyUi::PageShown(root, "debug") && !KytyUi::PageShown(root, "folders") &&
		          !KytyUi::PageShown(root, "controller") && !KytyUi::PageShown(root, "updates") &&
		          !KytyUi::PageShown(root, "about"),
		      "the game's editor has the pages General, Network, and Debug only");
		GtkWidget* configs = KytyUi::FindByBuildableId(root, "game_configs_button");
		Check(configs != nullptr && gtk_widget_get_visible(configs) == FALSE,
		      "the game's editor has no Game configs menu");
		// Ctrl+Page_Down and Ctrl+Page_Up go over the visible pages only and do not wrap.
		const std::vector<std::pair<guint, const char*>> presses = {
		    {GDK_KEY_Page_Down, "network"}, {GDK_KEY_Page_Down, "debug"},
		    {GDK_KEY_Page_Down, "debug"},   {GDK_KEY_Page_Up, "network"},
		    {GDK_KEY_Page_Up, "general"},   {GDK_KEY_Page_Up, "general"}};
		for (const auto& [key, page]: presses) {
			Check(KytyUi::KeyPress(root, key, GDK_CONTROL_MASK) &&
			          KytyUi::VisiblePage(root) == page,
			      std::string(key == GDK_KEY_Page_Down ? "Ctrl+Page_Down" : "Ctrl+Page_Up") +
			          " shows the page " + page + " of the game's editor (" +
			          KytyUi::VisiblePage(root) + ")");
		}
		gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(root, "User name")), "Alpha Own");
		adw_switch_row_set_active(
		    ADW_SWITCH_ROW(KytyUi::RowTitled(root, "Enable HDR display support")), TRUE);
		SaveAndClose(game, "the game settings");
		KytyUi::WaitFor(
		    [actions]() {
			    return g_action_group_get_action_enabled(actions, "clear-config") != FALSE;
		    },
		    2000, "Clear game config to be on for the game with its own settings now");

		// Opened again: the game's own values and the note.
		reads = DeviceReads(ui);
		game  = OpenGameSettings(application, window, kAlphaTitle);
		if (game != nullptr) {
			CheckDeviceRows(ui, GTK_WIDGET(game), reads, "the reopened game's editor");
			Check(KytyUi::AllText(GTK_WIDGET(game)).find("This game has its own settings.") !=
			          std::string::npos,
			      "the editor says that the game has its own settings");
			auto own        = global;
			own.user_name   = "Alpha Own";
			own.hdr_enabled = true;
			Check(RowDifferences(GTK_WIDGET(game), own).empty(),
			      "the editor opens with the saved values:" +
			          RowDifferences(GTK_WIDGET(game), own));
			CancelAndClose(game, "the game settings");
		}
	});
	const auto stored = KytyUi::StoredSettings(ui);
	auto       own    = global;
	own.user_name     = "Alpha Own";
	own.hdr_enabled   = true;
	Check(core::HasCustomSettings(stored, alpha), "Kyty.ini marks Alpha Game as custom (9.13)");
	Check(core::EnableCustomSettings(stored, alpha) == own,
	      "Alpha Game's settings are the global ones with its two changes");
	Check(stored.global == global, "the Global_Configuration is unchanged");
	Check(!core::HasCustomSettings(stored, beta), "Beta Quest still uses the global settings");
}

// ---- LFI-47: the editor's size and maximized state

KYTY_UI_CASE(settings_geometry) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty() || !KytyUi::StoreSettings(ui, [](core::LauncherSettings& settings) {
		    settings.settings_window = {880, 660, false};
	    })) {
		return;
	}
	const auto default_size = [](GtkWindow* window) {
		int width  = 0;
		int height = 0;
		gtk_window_get_default_size(window, &width, &height);
		return std::pair<int, int> {width, height};
	};
	const std::string before  = KytyUi::ReadText(ui.SettingsFile());
	auto&             context = ui.Context();
	GtkWindow*        parent  = ui.ParentWindow();
	const auto        present = [](GtkWindow* window, const std::string& what) {
		gtk_window_present(window);
		return KytyUi::WaitFor(
		           [window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; }, 5000,
		           what + " to be mapped") &&
		       KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
	};

	// The stored size; a new size is kept in memory when the editor closes, written by the next
	// save (closing writes nothing, 9.14), and shared by the game's editor.
	auto* global = GTK_WINDOW(kyty_settings_window_new_global(context, parent));
	Check(default_size(global) == std::pair<int, int> {880, 660} &&
	          gtk_window_is_maximized(global) == FALSE,
	      "the editor opens at the stored 880x660, not maximized");
	if (present(global, "the global settings")) {
		gtk_window_set_default_size(global, 960, 720);
		KytyUi::WaitFor([&]() { return default_size(global).first == 960; }, 2000,
		                "the editor's new size");
		CancelAndClose(global, "the global settings");
	}
	Check(context.settings.loaded.settings.settings_window ==
	          core::LauncherWindowState {960, 720, false},
	      "closing keeps the new size in the launcher's settings");
	Check(KytyUi::ReadText(ui.SettingsFile()) == before, "closing writes nothing");
	auto* game = GTK_WINDOW(kyty_settings_window_new_for_game(
	    context, parent, KytyUi::FixtureEntry(games, kAlphaTitle)));
	Check(default_size(game) == std::pair<int, int> {960, 720},
	      "the game's editor opens at the new 960x720 too");
	if (present(game, "the game settings")) {
		SaveAndClose(game, "the game settings");
	}
	Check(KytyUi::StoredSettings(ui).settings_window == core::LauncherWindowState {960, 720, false},
	      "the next save writes [SettingsWindow] 960x720");

	// The stored maximized state: the editor asks for it before it is shown, and the state it
	// closes in is kept.
	context.settings.loaded.settings.settings_window = {900, 700, true};
	global = GTK_WINDOW(kyty_settings_window_new_global(context, parent));
	Check(gtk_window_is_maximized(global) != FALSE,
	      "a stored maximized editor asks to be maximized before it is shown");
	if (present(global, "the maximized global settings")) {
		KytyUi::Pump(200);
		const bool maximized = gtk_window_is_maximized(global) != FALSE;
		g_print("settings_geometry: %s: maximized after mapping: %s\n",
		        g_getenv("GDK_BACKEND") != nullptr ? g_getenv("GDK_BACKEND") : "default",
		        maximized ? "yes" : "no");
		CancelAndClose(global, "the maximized global settings");
		Check(context.settings.loaded.settings.settings_window.maximized == maximized,
		      "closing keeps the maximized state the editor had");
	}
}

// ---- 12.5: the paths in use

KYTY_UI_CASE(settings_paths_display) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	const auto subtitle = [](GtkWidget* root, const char* title) -> std::string {
		GtkWidget* row = KytyUi::RowTitled(root, title);
		if (row == nullptr || !ADW_IS_ACTION_ROW(row)) {
			return "(no row " + std::string(title) + ")";
		}
		const char* text = adw_action_row_get_subtitle(ADW_ACTION_ROW(row));
		return text != nullptr ? text : "";
	};
	const auto check_about = [&](KytyApplication* application, const fs::path& settings_file,
	                             const std::string& when, const char* capture) {
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "about");
		GtkWidget*  root    = GTK_WIDGET(settings);
		const auto& context = KytyUi::ContextOf(application);
		Check(subtitle(root, "Settings file") == settings_file.string() &&
		          context.paths.settings_file == settings_file,
		      when + ": About this installation shows the Settings_Store " +
		          settings_file.string() + " (" + subtitle(root, "Settings file") + ")");
		Check(subtitle(root, "Data directory") == ui.AppDirectory().string() &&
		          context.paths.data_dir == ui.AppDirectory(),
		      when + ": it shows the Data_Directory " + ui.AppDirectory().string() + " (" +
		          subtitle(root, "Data directory") + ")");
		Check(subtitle(root, "Emulator") == (ui.AppDirectory() / "kyty_emulator").string(),
		      when + ": it shows the Emulator's path");
		for (const char* title: {"Settings file", "Data directory"}) {
			const std::string path = subtitle(root, title);
			Check(fs::path(path).is_absolute() && path.find('$') == std::string::npos &&
			          path.find('~') == std::string::npos,
			      when + ": " + title + " is absolute with every variable expanded (" + path + ")");
			GtkWidget* row = KytyUi::RowTitled(root, title);
			Check(row != nullptr &&
			          adw_action_row_get_subtitle_selectable(ADW_ACTION_ROW(row)) != FALSE,
			      when + ": the " + title + " path can be selected and copied");
		}
		Check(KytyUi::Capture(settings, capture), "the About page renders");
		// The session's pointer never moves (weston's fake seat, Xvfb's screen center): a widget
		// under it shows its hover state in the captures, so the log says where it is.
		GdkSeat*    seat    = gdk_display_get_default_seat(gtk_widget_get_display(root));
		GdkDevice*  pointer = seat != nullptr ? gdk_seat_get_pointer(seat) : nullptr;
		GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(settings));
		double      x       = 0;
		double      y       = 0;
		if (pointer != nullptr && surface != nullptr &&
		    gdk_surface_get_device_position(surface, pointer, &x, &y, nullptr) != FALSE) {
			double dx = 0;
			double dy = 0;
			gtk_native_get_surface_transform(GTK_NATIVE(settings), &dx, &dy);
			GtkWidget* hovered = gtk_widget_pick(root, x - dx, y - dy, GTK_PICK_DEFAULT);
			g_print("settings_paths_display: %s: the pointer is in the editor at %.0f,%.0f, over "
			        "%s\n",
			        when.c_str(), x - dx, y - dy,
			        hovered != nullptr ? KytyUi::WidgetPath(hovered).c_str() : "nothing");
		} else {
			g_print("settings_paths_display: %s: the pointer is not in the editor\n", when.c_str());
		}
		CancelAndClose(settings, "the global settings");
	};
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		Check(KytyUi::WidgetText(KytyUi::FindByBuildableId(GTK_WIDGET(window), "settings_label")) ==
		          "Settings file: " + ui.SettingsFile().string(),
		      "the footer names the Settings_Store");
		check_about(application, ui.SettingsFile(), "XDG_CONFIG_HOME set", "about");
	});
	// Without XDG_CONFIG_HOME the Settings_Store is $HOME/.config/Kyty/Kyty.ini, expanded.
	ui.Services().environment.Unset("XDG_CONFIG_HOME");
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow*) {
		check_about(application, ui.Root() / "home" / ".config" / "Kyty" / "Kyty.ini",
		            "XDG_CONFIG_HOME unset", "about-home-config");
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the second run ends with status 0");
}

// ---- LFI-17, 9.8-9.10, 9.15: Game configs

KYTY_UI_CASE(settings_game_configs_import_export) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	const auto        alpha = KytyUi::FixtureEntry(games, kAlphaTitle);
	const auto        beta  = KytyUi::FixtureEntry(games, kBetaTitle);
	if (games.empty() || !KytyUi::StoreSettings(ui, [&](core::LauncherSettings& settings) {
		    auto configuration               = core::EnableCustomSettings(settings, alpha);
		    configuration.user_name          = "Alpha Own";
		    configuration.fullscreen_enabled = true;
		    Check(static_cast<bool>(core::SaveCustomSettings(settings, alpha, configuration)),
		          "give Alpha Game its own settings");
		    configuration                  = core::EnableCustomSettings(settings, beta);
		    configuration.user_name        = "Beta Own";
		    configuration.vblank_frequency = 90;
		    Check(static_cast<bool>(core::SaveCustomSettings(settings, beta, configuration)),
		          "give Beta Quest its own settings");
	    })) {
		return;
	}
	const fs::path exported = ui.Root() / "export" / "PPSA01002.json";
	const fs::path kept     = ui.Root() / "export" / "kept.json";
	KytyUi::WriteText(kept, "previous content\n");
	const auto write = [&ui](const char* name, std::string_view text) {
		const fs::path path = ui.Root() / name;
		KytyUi::WriteText(path, text);
		return path;
	};
	const fs::path not_json = write("not-json.json", "{ this is not JSON");
	const fs::path unknown  = write("unknown.json", "{\"PPSA01001\": {\"no_such_field\": 1}}");
	const fs::path invalid  = write("invalid.json", "{\"PPSA01001\": {\"user_id\": 254}}");
	const fs::path no_match =
	    write("no-match.json", "{\"PPSA09999\": {\"user_name\": \"Nobody\"}}");
	const fs::path good = write("good.json", "{\"PPSA01001\": {\"user_name\": \"Imported\"}, "
	                                         "\"PPSA01002\": {\"user_name\": \"Imported\"}}");
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* /*window*/) {
		auto&      chooser  = ui.Services().file_chooser;
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(settings);
		GtkWidget* menu = KytyUi::FindByBuildableId(root, "game_configs_button");
		if (!Check(menu != nullptr && GTK_IS_MENU_BUTTON(menu), "the Game configs menu")) {
			return;
		}
		// Export (9.8): the picker, the file with the title ID and every field.
		const auto pick_beta = [&]() {
			GtkWindow* picker = KytyUi::WaitForWindow(KYTY_TYPE_GAME_PICKER_DIALOG,
			                                          "Export… to open the game picker");
			if (picker == nullptr) {
				return;
			}
			GtkWidget* row = KytyUi::FindFirst(GTK_WIDGET(picker), [](GtkWidget* widget) {
				return GTK_IS_LIST_BOX_ROW(widget) &&
				       KytyUi::AllText(widget).find("PPSA01002") != std::string::npos;
			});
			if (Check(row != nullptr, "the picker lists Beta Quest")) {
				gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(row)),
				                        GTK_LIST_BOX_ROW(row));
				Check(KytyUi::Activate(
				          KytyUi::FindByBuildableId(GTK_WIDGET(picker), "accept_button")),
				      "a click on Export");
			}
		};
		chooser.file_answers.push_back(exported);
		std::error_code ec;
		fs::create_directories(exported.parent_path(), ec);
		Check(KytyUi::ClickMenuItem(menu, "Export…"), "Game configs, Export…");
		pick_beta();
		KytyUi::WaitFor([&]() { return KytyUi::Exists(exported); }, 3000,
		                "the export to be written");
		const auto                          read  = core::ReadGameSettingsFile(exported);
		const core::GameConfigurationPatch* patch = nullptr;
		if (read) {
			if (const auto found = read->by_title.find("PPSA01002");
			    found != read->by_title.end()) {
				patch = &found->second;
			}
		}
		if (Check(read && read->by_title.size() == 1 && patch != nullptr,
		          "the file holds Beta Quest's config under its title ID")) {
			Check(patch->fields.size() == core::GameConfigurationFields().size(),
			      "it has every field (" + std::to_string(patch->fields.size()) + ")");
			Check(core::ApplyPatch(core::GameConfiguration {}, *patch) ==
			          core::EnableCustomSettings(KytyUi::StoredSettings(ui), beta),
			      "with the values Beta Quest runs with");
		}
		// An export that cannot be written (an injected write failure) names the path and leaves
		// the file that was there (9.15).
		chooser.file_answers.push_back(kept);
		core::SetAtomicFileFaultForTesting([](core::AtomicFileStep step) {
			return step == core::AtomicFileStep::Write ? ENOSPC : 0;
		});
		Check(KytyUi::ClickMenuItem(menu, "Export…"), "Game configs, Export… again");
		pick_beta();
		if (GtkWindow* alert = KytyUi::FindAlert("The game config could not be exported");
		    alert != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
			Check(text.find(kept.string()) != std::string::npos &&
			          text.find(g_strerror(ENOSPC)) != std::string::npos,
			      "the error names the path and the reason (" + text + ")");
			KytyUi::CloseError(alert);
		}
		core::SetAtomicFileFaultForTesting({});
		Check(KytyUi::ReadText(kept) == "previous content\n", "the file there is unchanged");

		// Rejected imports (9.10) name the setting or say why, and change nothing.
		const std::string before = KytyUi::ReadText(ui.SettingsFile());
		for (const auto& [file, heading, message]:
		     {std::tuple<fs::path, const char*, std::string> {
		          not_json, "The game configs could not be imported",
		          "The file is not valid JSON."},
		      std::tuple<fs::path, const char*, std::string> {
		          unknown, "The game configs could not be imported",
		          "The game config of PPSA01001 contains the unknown setting “no_such_field”."},
		      std::tuple<fs::path, const char*, std::string> {
		          invalid, "The game configs could not be imported",
		          "The setting User ID (user_id) in the game config of PPSA01001 has an invalid "
		          "value. The user ID must be between 0 and 2147482879 and must not be 254 "
		          "(everyone) or 255 (system)."},
		      std::tuple<fs::path, const char*, std::string> {
		          no_match, "No matching games",
		          "No game in the library has a title ID of the file: PPSA09999"}}) {
			chooser.file_answers.push_back(file);
			Check(KytyUi::ClickMenuItem(menu, "Import…"),
			      "Game configs, Import… " + file.filename().string());
			if (GtkWindow* alert = KytyUi::FindAlert(heading); alert != nullptr) {
				const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
				Check(text.find(message) != std::string::npos,
				      file.filename().string() + ": \"" + message + "\" (the alert says: " + text +
				          ")");
				KytyUi::CloseError(alert);
			}
			Check(KytyUi::ReadText(ui.SettingsFile()) == before,
			      file.filename().string() + " leaves Kyty.ini unchanged");
		}
		// A good file: Cancel at the confirmation changes nothing; Import applies the file's
		// fields to every matching game and keeps the others (9.9).
		chooser.file_answers.push_back(good);
		Check(KytyUi::ClickMenuItem(menu, "Import…"), "Game configs, Import… good.json");
		if (GtkWindow* alert = KytyUi::FindAlert("Import game configs?"); alert != nullptr) {
			KytyUi::CloseAlert(alert, "Cancel");
		}
		Check(KytyUi::ReadText(ui.SettingsFile()) == before, "Cancel imports nothing");
		chooser.file_answers.push_back(good);
		Check(KytyUi::ClickMenuItem(menu, "Import…"), "Game configs, Import… good.json again");
		if (GtkWindow* alert = KytyUi::FindAlert("Import game configs?"); alert != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
			Check(text.find("PPSA01001 — Alpha Game") != std::string::npos &&
			          text.find("PPSA01002 — Beta Quest") != std::string::npos,
			      "the confirmation lists both games (" + text + ")");
			KytyUi::CloseAlert(alert, "Import");
		}
		KytyUi::WaitFor(
		    [&]() {
			    return StoredUserName(ui, alpha) == "Imported" &&
			           StoredUserName(ui, beta) == "Imported";
		    },
		    3000, "the import to give both games the file's user name");
		CancelAndClose(settings, "the global settings");
	});
	const auto stored = KytyUi::StoredSettings(ui);
	Check(core::EnableCustomSettings(stored, alpha).fullscreen_enabled &&
	          core::EnableCustomSettings(stored, beta).vblank_frequency == 90,
	      "the fields the file does not have keep their values");
}

// ---- 7.1, 7.2: Game_Folders

KYTY_UI_CASE(settings_folders) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path  more = ui.Root() / "more-games";
	const fs::path  link = ui.Root() / "more-games-link";
	std::error_code ec;
	KytyUi::WriteGameWithParam(more / "Delta Game",
	                           KytyUi::ParamJson("Delta Game", "PPSA01005", "01.000"));
	fs::create_directory_symlink(more, link, ec);
	Check(!ec, "make the link " + link.string());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* window) {
		auto& chooser = ui.Services().file_chooser;
		auto& library = KytyUi::ContextOf(application).library;
		auto& scans   = kyty::gtk::MainWindowLibrary(window);
		Check(KytyUi::GameByTitle(library, "Delta Game") == nullptr,
		      "Delta Game is not listed at first");
		g_action_group_activate_action(G_ACTION_GROUP(application), "add-game-folder", nullptr);
		GtkWindow* settings = KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the Folders page");
		if (settings == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(settings);
		GtkWidget* list = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_LIST_BOX(widget) &&
			       gtk_list_box_get_selection_mode(GTK_LIST_BOX(widget)) == GTK_SELECTION_MULTIPLE;
		});
		// By reference: the list of the editor opened last.
		const auto rows = [&list]() {
			std::vector<std::string> titles;
			for (GtkWidget* row: KytyUi::FindAll(
			         list, [](GtkWidget* widget) { return ADW_IS_ACTION_ROW(widget); })) {
				titles.push_back(KytyUi::WidgetText(row));
			}
			return titles;
		};
		if (!Check(list != nullptr, "the Folders page has the folder list")) {
			return;
		}
		// Several folders at once; a folder and a link to it are one entry (7.1).
		chooser.folder_answers.push_back({more, link});
		Check(KytyUi::Activate(KytyUi::ButtonLabelled(root, {"Add folders…"})),
		      "a click on Add folders…");
		KytyUi::WaitFor([&]() { return rows().size() == 2; }, 3000, "the new folder to be listed");
		Check(rows() == std::vector<std::string> {games.string(), more.string()},
		      "the list has the stored folder and the new one once");
		auto applied = scans.Stats().scans_applied;
		SaveAndClose(settings, "the global settings");
		KytyUi::WaitFor(
		    [&]() {
			    return scans.Stats().scans_applied > applied &&
			           KytyUi::GameByTitle(library, "Delta Game") != nullptr;
		    },
		    8000, "the saved folders to be scanned (Delta Game listed)");
		Check(KytyUi::StoredSettings(ui).game_folders ==
		          std::vector<std::string> {games.string(), more.string()},
		      "Kyty.ini has both folders as absolute paths");
		Check(KytyUi::GameByTitle(library, kAlphaTitle) != nullptr,
		      "the first folder's games stay listed");

		// Remove the first folder (7.2): its games leave the list.
		g_action_group_activate_action(G_ACTION_GROUP(application), "add-game-folder", nullptr);
		settings = KytyUi::WaitForWindow(KYTY_TYPE_SETTINGS_WINDOW, "the Folders page again");
		if (settings == nullptr) {
			return;
		}
		root              = GTK_WIDGET(settings);
		list              = KytyUi::FindFirst(root, [](GtkWidget* widget) {
			return GTK_IS_LIST_BOX(widget) &&
			       gtk_list_box_get_selection_mode(GTK_LIST_BOX(widget)) == GTK_SELECTION_MULTIPLE;
		});
		GtkWidget* remove = KytyUi::ButtonLabelled(root, {"Remove"});
		GtkWidget* first  = KytyUi::FindFirst(list, [&games](GtkWidget* widget) {
			return ADW_IS_ACTION_ROW(widget) && KytyUi::WidgetText(widget) == games.string();
		});
		Check(remove != nullptr && gtk_widget_get_sensitive(remove) == FALSE,
		      "Remove is off without a selection");
		if (first != nullptr) {
			gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(first));
		}
		Check(KytyUi::Activate(remove), "a click on Remove");
		Check(rows() == std::vector<std::string> {more.string()}, "only the new folder is left");
		applied = scans.Stats().scans_applied;
		SaveAndClose(settings, "the global settings");
		KytyUi::WaitFor(
		    [&]() {
			    return scans.Stats().scans_applied > applied &&
			           KytyUi::GameByTitle(library, kAlphaTitle) == nullptr;
		    },
		    8000, "the removed folder's games to leave the list");
		Check(g_list_model_get_n_items(library.Games()) == 1 &&
		          KytyUi::GameByTitle(library, "Delta Game") != nullptr,
		      "the list has the remaining folder's game only");
	});
	Check(KytyUi::StoredSettings(ui).game_folders == std::vector<std::string> {more.string()},
	      "Kyty.ini has the remaining folder");
}

// ---- HC-17: a Settings_Store that could not be loaded

KYTY_UI_CASE(settings_load_error_overwrite) {
	KytyUi::UiContext ui;
	const auto        games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return;
	}
	ui.ServeCompatibility();
	// A Kyty.ini over the 16 MiB limit (a sparse file): it exists but cannot be loaded.
	const fs::path  file = ui.SettingsFile();
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	KytyUi::WriteText(file, "[Launcher]\n");
	fs::resize_file(file, core::kMaxSettingsFileBytes + 1, ec);
	Check(!ec, "make Kyty.ini larger than the limit");
	const auto size = [&file]() {
		std::error_code size_ec;
		return fs::file_size(file, size_ec);
	};
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto&      context = KytyUi::ContextOf(application);
		GtkWidget* banner  = KytyUi::FindByBuildableId(GTK_WIDGET(window), "settings_banner");
		Check(context.settings.load_error.has_value(), "the launcher could not load Kyty.ini");
		Check(banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE &&
		          g_str_has_prefix(adw_banner_get_title(ADW_BANNER(banner)),
		                           "The settings file could not be loaded (") != FALSE &&
		          g_str_has_suffix(adw_banner_get_title(ADW_BANNER(banner)),
		                           "). Nothing is saved until you allow it.") != FALSE,
		      "the banner says that nothing is saved until it is allowed");
		Check(KytyUi::Capture(GTK_WINDOW(window), "banner"), "the main window with the banner");
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		gtk_editable_set_text(GTK_EDITABLE(KytyUi::RowTitled(GTK_WIDGET(settings), "User name")),
		                      "After");
		// Save asks first; Cancel writes nothing and the editor stays. (Found by its body: the
		// main window's banner starts like the question's heading.)
		const std::string question = file.string() + " exists but could not be loaded";
		ClickButton(settings, "save_button");
		if (GtkWindow* alert = KytyUi::FindAlert(question); alert != nullptr) {
			const std::string text = KytyUi::AllText(GTK_WIDGET(alert));
			Check(text.find("The settings file could not be loaded") != std::string::npos &&
			          text.find(question + ":") != std::string::npos &&
			          text.find("Overwrite replaces it with these settings.") != std::string::npos,
			      "the question names the file and the reason (" + text + ")");
			KytyUi::CloseAlert(alert, "Cancel");
		}
		Check(size() == core::kMaxSettingsFileBytes + 1 && KytyUi::IsToplevel(settings),
		      "Cancel leaves Kyty.ini as it was and the editor open");
		// Overwrite replaces it.
		ClickButton(settings, "save_button");
		if (GtkWindow* alert = KytyUi::FindAlert(question); alert != nullptr) {
			KytyUi::CloseAlert(alert, "Overwrite");
		}
		KytyUi::WaitClosed(settings, "the editor to close after Overwrite");
		Check(KytyUi::StoredSettings(ui).global.user_name == "After" &&
		          size() < core::kMaxSettingsFileBytes,
		      "Overwrite writes the settings");
		KytyUi::WaitFor([banner]() { return adw_banner_get_revealed(ADW_BANNER(banner)) == FALSE; },
		                2000, "the banner to go away");
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the first run ends with status 0");

	// The banner's Allow saving: after the confirmation the launcher saves again by itself.
	KytyUi::WriteText(file, "[Launcher]\n");
	fs::resize_file(file, core::kMaxSettingsFileBytes + 1, ec);
	const int second = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* banner = KytyUi::FindByBuildableId(GTK_WIDGET(window), "settings_banner");
		if (!Check(banner != nullptr && adw_banner_get_revealed(ADW_BANNER(banner)) != FALSE,
		           "the banner is back")) {
			KytyUi::QuitApplication(application);
			return;
		}
		g_signal_emit_by_name(banner, "button-clicked");
		if (GtkWindow* alert = KytyUi::FindAlert("Allow saving the settings?"); alert != nullptr) {
			Check(KytyUi::AllText(GTK_WIDGET(alert)).find(file.string()) != std::string::npos,
			      "the question names the file");
			KytyUi::CloseAlert(alert, "Allow saving");
		}
		KytyUi::WaitFor([banner]() { return adw_banner_get_revealed(ADW_BANNER(banner)) == FALSE; },
		                2000, "Allow saving to hide the banner");
		// A change that is saved automatically: the view mode.
		g_action_group_change_action_state(G_ACTION_GROUP(application), "toggle-grid",
		                                   g_variant_new_boolean(TRUE));
		KytyUi::WaitFor([&]() { return size() < core::kMaxSettingsFileBytes; }, 3000,
		                "the view mode to be saved over the old file");
		KytyUi::QuitApplication(application);
	});
	Check(second == 0, "the second run ends with status 0");
	Check(KytyUi::StoredSettings(ui).grid_view, "Kyty.ini has the grid view");
}

// ---- 11.15, LFI-22, LFI-23: the lightbar preview

KYTY_UI_CASE(settings_controller_preview) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::StoreSettings(
	        ui, [](core::LauncherSettings& settings) { settings.controller.color = "#0000ff"; })) {
		return;
	}
	const std::string before = KytyUi::ReadText(ui.SettingsFile());
	KytyUi::RunWithLibrary(ui, [&](KytyApplication* application, KytyMainWindow* /*window*/) {
		auto& controller = ui.Services().controller;
		KytyUi::WaitFor(
		    [&controller]() {
			    return controller.Lightbar() == kyty::gtk::LightbarColor {0, 0, 255};
		    },
		    2000, "the stored color to light the controller (LFI-23)");
		GtkWindow* settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "controller");
		GtkWidget* root   = GTK_WIDGET(settings);
		GtkWidget* button = KytyUi::FindByType(root, GTK_TYPE_COLOR_DIALOG_BUTTON);
		GtkWidget* reset  = KytyUi::ButtonLabelled(root, {"Default"});
		if (!Check(button != nullptr && reset != nullptr,
		           "the lightbar row has its color button and Default")) {
			return;
		}
		// A new color: the swatch and the controller show it within 100 ms (11.15).
		GdkRGBA red {};
		gdk_rgba_parse(&red, "#ff0000");
		const gint64 start = g_get_monotonic_time();
		gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &red);
		const auto& calls = controller.lightbar_calls;
		const bool  lit =
		    !calls.empty() && calls.back().color == kyty::gtk::LightbarColor {255, 0, 0};
		const double ms = lit ? static_cast<double>(calls.back().time_us - start) / 1000.0 : -1.0;
		g_print("settings_controller_preview: lightbar preview after %.3f ms\n", ms);
		Check(lit && ms >= 0 && ms <= 100,
		      "the controller shows the new color within 100 ms (" + std::to_string(ms) + " ms)");
		const GdkRGBA* shown = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
		Check(shown != nullptr && std::lround(shown->red * 255) == 255 &&
		          std::lround(shown->green * 255) == 0 && std::lround(shown->blue * 255) == 0,
		      "the swatch shows the new color");
		Check(KytyUi::Capture(settings, "controller"), "the Controller page renders");
		// Default: the game controls the lightbar.
		Check(KytyUi::Activate(reset), "a click on Default");
		Check(controller.Lightbar() == std::nullopt, "Default gives the lightbar back to the game");
		// Closing without saving shows the saved color again.
		CancelAndClose(settings, "the global settings");
		Check(controller.Lightbar() == kyty::gtk::LightbarColor {0, 0, 255},
		      "Cancel restores the saved color on the controller");
		Check(KytyUi::ReadText(ui.SettingsFile()) == before, "Cancel writes nothing");
		// Save keeps the new color.
		settings = OpenGlobalSettings(application);
		if (settings == nullptr) {
			return;
		}
		kyty_settings_window_show_page(KYTY_SETTINGS_WINDOW(settings), "controller");
		GdkRGBA green {};
		gdk_rgba_parse(&green, "#00ff00");
		gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(KytyUi::FindByType(
		                                     GTK_WIDGET(settings), GTK_TYPE_COLOR_DIALOG_BUTTON)),
		                                 &green);
		SaveAndClose(settings, "the global settings");
		Check(controller.Lightbar() == kyty::gtk::LightbarColor {0, 255, 0},
		      "after Save the controller keeps the new color");
	});
	Check(g_ascii_strcasecmp(KytyUi::StoredSettings(ui).controller.color.c_str(), "#00ff00") == 0,
	      "Kyty.ini has the new color");
}
