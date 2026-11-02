#include "launcher/gtk/settingsWindow.h"

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameScanner.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/scanFileSystem.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/text.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gamePickerDialog.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/services/desktopLauncher.h"
#include "launcher/gtk/services/deviceLists.h"
#include "launcher/gtk/services/fileChooser.h"
#include "launcher/gtk/settingsWrite.h"
#include "updater/core/updatePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace core = kyty::core;
namespace fs   = std::filesystem;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::LauncherContext;
using kyty::gtk::SignalScope;

constexpr const char* kTemplateResource = "/io/github/MrG00gle/KytyPS5_GTK/ui/settings-window.xml";

// The lightbar color the color chooser starts with while no color is set (the Qt launcher's).
constexpr const char* kDefaultPreviewColor = "#0070d1";
// The swatch holds kDefaultPreviewColor with this alpha. GtkColorDialogButton reports a choice
// (notify::rgba) only when the chosen color differs from the one it holds, and the chooser returns
// every color with alpha 1 (with-alpha is off), so choosing the start color as it is still counts,
// as in the Qt launcher (Requirement 11.15). An alpha this close to 1 is drawn opaque. The start
// color keeps this alpha also while it is the chosen color, so that the chooser always gets it the
// same way and finds it among its custom colors again.
constexpr float kStartColorAlpha = 0.999F;

// ---- The field table (plan D36): every Global_Configuration field that the Qt launcher's editor
// shows, in Qt's groups. elf, input_mapping_custom, and host_input_mapping are not shown; the
// editor keeps their values.

enum class Group {
	UserProfile,
	Graphics,
	Audio,
	Notifications,
	General,
	Compatibility,
	Network,
	Debugging,
	Rendering,
};

enum class RowKind {
	Switch,     // Bool: AdwSwitchRow
	Choice,     // Enum, or an Int with named values: AdwComboRow over choices
	Number,     // Int: AdwSpinRow from minimum to maximum
	Text,       // String: AdwEntryRow
	Gpu,        // gpu_index: AdwComboRow with "Automatic" and the Vulkan devices
	Microphone, // audio_input_device: AdwComboRow with "None" and the recording devices
};

struct FieldSpec {
	std::string_view             key;               // the GameConfigurationFields() name
	const char*                  label   = nullptr; // N_()
	const char*                  tooltip = nullptr; // N_(), the Qt launcher's tooltip
	Group                        group   = Group::General;
	RowKind                      kind    = RowKind::Switch;
	int64_t                      minimum = 0;
	int64_t                      maximum = 0;
	std::span<const char* const> choices = {}; // Choice: the labels in value order (N_())
	// The row is enabled only while the row of enabled_by has the value enabled_when (a switch
	// counts as 1 when on; a choice is its index), LFI-19.
	std::string_view enabled_by   = {};
	int64_t          enabled_when = 0;
};

// TRANSLATORS: screen resolutions, width × height in pixels.
constexpr const char* kResolutions[] = {N_("1280 × 720"), N_("1920 × 1080"), N_("2560 × 1440"),
                                        N_("3840 × 2160")};
// TRANSLATORS: Vulkan presentation modes.
constexpr const char* kPresentModes[]        = {N_("Fifo"), N_("Mailbox"), N_("Immediate")};
constexpr const char* kShaderOptimizations[] = {N_("None"), N_("Size"), N_("Performance")};
// TRANSLATORS: where shader logs and printf output go.
constexpr const char* kLogDirections[] = {N_("Silent"), N_("Console"), N_("File")};
// The console languages in the order of their values (0-29), as the Qt launcher named them.
constexpr const char* kConsoleLanguages[] = {
    N_("Japanese"),
    N_("English (United States)"),
    N_("French (France)"),
    N_("Spanish (Spain)"),
    N_("German"),
    N_("Italian"),
    N_("Dutch"),
    N_("Portuguese (Portugal)"),
    N_("Russian"),
    N_("Korean"),
    N_("Chinese (Traditional)"),
    N_("Chinese (Simplified)"),
    N_("Finnish"),
    N_("Swedish"),
    N_("Danish"),
    N_("Norwegian"),
    N_("Polish"),
    N_("Portuguese (Brazil)"),
    N_("English (United Kingdom)"),
    N_("Turkish"),
    N_("Spanish (Latin America)"),
    N_("Arabic"),
    N_("French (Canada)"),
    N_("Czech"),
    N_("Hungarian"),
    N_("Greek"),
    N_("Romanian"),
    N_("Thai"),
    N_("Vietnamese"),
    N_("Indonesian"),
};
static_assert(std::size(kConsoleLanguages) ==
              static_cast<std::size_t>(core::kMaxConsoleLanguage) + 1);

constexpr int64_t kFileDirection = static_cast<int64_t>(core::LogDirection::File);

const FieldSpec kFields[] = {
    {.key     = "user_name",
     .label   = N_("User name"),
     .tooltip = N_("Local user name exposed to games"),
     .group   = Group::UserProfile,
     .kind    = RowKind::Text},
    {.key     = "user_id",
     .label   = N_("User ID"),
     .tooltip = N_("Local user ID exposed to games"),
     .group   = Group::UserProfile,
     .kind    = RowKind::Number,
     .minimum = 0,
     .maximum = core::kMaxUserId},
    {.key     = "console_language",
     .label   = N_("Console language"),
     .tooltip = N_("The system language that games see"),
     .group   = Group::UserProfile,
     .kind    = RowKind::Choice,
     .choices = kConsoleLanguages},
    {.key     = "screen_resolution",
     .label   = N_("Screen resolution"),
     .tooltip = N_("Window resolution"),
     .group   = Group::Graphics,
     .kind    = RowKind::Choice,
     .choices = kResolutions},
    {.key     = "present_mode",
     .label   = N_("Present mode"),
     .tooltip = N_("Vulkan swapchain presentation mode"),
     .group   = Group::Graphics,
     .kind    = RowKind::Choice,
     .choices = kPresentModes},
    {.key     = "vblank_frequency",
     .label   = N_("Vblank frequency"),
     .tooltip = N_("Virtual display refresh rate used for frame pacing"),
     .group   = Group::Graphics,
     .kind    = RowKind::Number,
     .minimum = 30,
     .maximum = 360},
    {.key     = "shader_optimization_type",
     .label   = N_("Shader optimization"),
     .tooltip = N_("Optimize shaders for code size or performance"),
     .group   = Group::Graphics,
     .kind    = RowKind::Choice,
     .choices = kShaderOptimizations},
    {.key     = "gpu_index",
     .label   = N_("GPU"),
     .tooltip = N_("Graphics device used by the emulator"),
     .group   = Group::Graphics,
     .kind    = RowKind::Gpu},
    {.key     = "readback_linear_images",
     .label   = N_("Enable readback"),
     .tooltip = N_("Read back writable linear images"),
     .group   = Group::Graphics},
    {.key     = "readback_tiled_images",
     .label   = N_("Enable tiled readback"),
     .tooltip = N_("Read back small writable tiled images"),
     .group   = Group::Graphics},
    {.key     = "tessellation_enabled",
     .label   = N_("Enable tessellation support (experimental)"),
     .tooltip = N_("Draw tessellated geometry. Disabled by default; unsupported tessellation may "
                   "cause crashes."),
     .group   = Group::Graphics},
    {.key     = "hdr_enabled",
     .label   = N_("Enable HDR display support"),
     .tooltip = N_("Enable HDR10 output for supported games. Requires HDR enabled on the host "
                   "display."),
     .group   = Group::Graphics},
    {.key     = "audio_input_device",
     .label   = N_("Microphone"),
     .tooltip = N_("Microphone used by games. None supplies silence."),
     .group   = Group::Audio,
     .kind    = RowKind::Microphone},
    {.key     = "trophy_enabled",
     .label   = N_("Enable trophy notifications"),
     .tooltip = N_("Show trophy unlock notifications and play their sound."),
     .group   = Group::Notifications},
    {.key     = "trophy_duration_seconds",
     .label   = N_("Trophy notification duration (seconds)"),
     .tooltip = N_("How long each trophy unlock notification stays on screen."),
     .group   = Group::Notifications,
     .kind    = RowKind::Number,
     .minimum = 5,
     .maximum = 10},
    {.key     = "fullscreen_enabled",
     .label   = N_("Run in fullscreen"),
     .tooltip = N_("Run in borderless desktop fullscreen"),
     .group   = Group::General},
    {.key     = "hide_cursor_enabled",
     .label   = N_("Auto-hide cursor"),
     .tooltip = N_("Hide the host cursor after 2 seconds without mouse activity"),
     .group   = Group::General},
    {.key     = "perf_overlay_enabled",
     .label   = N_("Show performance panel"),
     .tooltip = N_("Show FPS, frame times, CPU/GPU usage, RAM and memory pressure in the game "
                   "window. Press F2 to toggle it, and add Shift for the detailed view."),
     .group   = Group::General},
    {.key     = "skip_notice_screen",
     .label   = N_("Skip boot logos"),
     .tooltip = N_("Skip startup logos and notices in supported games."),
     .group   = Group::General},
    {.key     = "amd_cpu_enabled",
     .label   = N_("AMD CPU patch (experimental)"),
     .tooltip = N_("Patch CPU instructions that differ between AMD and the host CPU."),
     .group   = Group::Compatibility},
    {.key     = "network_connected",
     .label   = N_("Enable network connection"),
     .tooltip = N_("Let games use your computer's local network connection."),
     .group   = Group::Network},
    {.key     = "vulkan_validation_enabled",
     .label   = N_("Vulkan validation"),
     .tooltip = N_("Enable Vulkan validation layers"),
     .group   = Group::Debugging},
    {.key     = "shader_validation_enabled",
     .label   = N_("Shader validation"),
     .tooltip = N_("Validate SPIR-V binary"),
     .group   = Group::Debugging},
    {.key     = "renderdoc_enabled",
     .label   = N_("RenderDoc capture"),
     .tooltip = N_("Enable RenderDoc capture"),
     .group   = Group::Debugging},
    {.key     = "command_buffer_dump_enabled",
     .label   = N_("Command buffer dump"),
     .tooltip = N_("Dump command buffers"),
     .group   = Group::Debugging},
    {.key          = "command_buffer_dump_folder",
     .label        = N_("Command buffer folder"),
     .tooltip      = N_("Specify directory to dump command buffers"),
     .group        = Group::Debugging,
     .kind         = RowKind::Text,
     .enabled_by   = "command_buffer_dump_enabled",
     .enabled_when = 1},
    {.key     = "shader_log_direction",
     .label   = N_("Shader logging"),
     .tooltip = N_("Dump shaders to file or console window. If enabled may decrease emulator "
                   "performance"),
     .group   = Group::Debugging,
     .kind    = RowKind::Choice,
     .choices = kLogDirections},
    {.key          = "shader_log_folder",
     .label        = N_("Shader log folder"),
     .tooltip      = N_("Specify directory to dump shaders"),
     .group        = Group::Debugging,
     .kind         = RowKind::Text,
     .enabled_by   = "shader_log_direction",
     .enabled_when = kFileDirection},
    {.key     = "printf_direction",
     .label   = N_("Printf output"),
     .tooltip = N_("Print logs to file or console window. If enabled may decrease emulator "
                   "performance"),
     .group   = Group::Debugging,
     .kind    = RowKind::Choice,
     .choices = kLogDirections},
    {.key          = "printf_output_file",
     .label        = N_("Printf output file"),
     .tooltip      = N_("Specify file to dump logs"),
     .group        = Group::Debugging,
     .kind         = RowKind::Text,
     .enabled_by   = "printf_direction",
     .enabled_when = kFileDirection},
    {.key     = "profiler_enabled",
     .label   = N_("Tracy profiler"),
     .tooltip = N_("Enable the Tracy profiler. Profiling may decrease emulator performance"),
     .group   = Group::Debugging},
    {.key     = "sync_raw_image_buffers",
     .label   = N_("Accurate texture synchronization"),
     .tooltip = N_("Can fix missing textures and incorrect transparency (Resident Evil 2)."),
     .group   = Group::Rendering},
};

const FieldSpec* FindSpec(std::string_view key) {
	for (const auto& spec: kFields) {
		if (spec.key == key) {
			return &spec;
		}
	}
	return nullptr;
}

const char* PageOf(Group group) {
	switch (group) {
		case Group::Network: return "network";
		case Group::Debugging:
		case Group::Rendering: return "debug";
		default: return "general";
	}
}

// One shown field.
struct FieldRow {
	const FieldSpec*             spec  = nullptr;
	const core::FieldDescriptor* field = nullptr;
	GtkWidget*                   row   = nullptr;
	// Gpu: the gpu_index of each item; Microphone: the device name of each item ("" = none).
	std::vector<int64_t>     gpu_values;
	std::vector<std::string> device_values;
};

} // namespace

namespace kyty::gtk {

struct SettingsWindowState {
	LauncherContext*               context = nullptr;
	std::optional<core::GameEntry> entry; // the per-game editor's game
	// The configuration the editor opened with; the fields it does not show keep these values.
	core::GameConfiguration base;
	std::vector<FieldRow>   rows;
	DeviceList              gpus;
	DeviceList              microphones;

	// Global mode: the Folders, Controller, and Updates pages.
	std::vector<std::string> folders;
	GtkListBox*              folder_list           = nullptr;
	GtkWidget*               remove_folders_button = nullptr;
	GtkWidget*               addons_row            = nullptr;
	GtkWidget*               addons_warning        = nullptr;
	std::string              color; // "" (the game controls the lightbar) or "#rrggbb"
	GtkWidget*               color_row            = nullptr;
	GtkWidget*               color_button         = nullptr;
	GtkWidget*               color_default_button = nullptr;
	GtkWidget*               vibration_scale      = nullptr;
	GtkWidget*               volume_scale         = nullptr;
	GtkWidget*               keyboard_player_row  = nullptr;
	GtkWidget*               check_updates_row    = nullptr;

	bool previewed = false; // the lightbar shows a color chosen here
	bool loading   = false; // widgets are being set from values
	bool disposed  = false;
	// Cancelled when the window goes away: no chooser, alert, or launcher callback runs after it.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	GObjectPtr<GSimpleActionGroup> actions;
	std::vector<SignalScope>       signals;
};

} // namespace kyty::gtk

struct _KytySettingsWindow {
	AdwWindow            parent_instance;
	AdwViewStack*        stack;
	GtkWidget*           game_info;
	GtkLabel*            game_title_label;
	GtkLabel*            game_note_label;
	AdwViewStackPage*    folders_page;
	AdwViewStackPage*    controller_page;
	AdwViewStackPage*    updates_page;
	AdwViewStackPage*    about_page;
	AdwPreferencesGroup* user_profile_group;
	AdwPreferencesGroup* graphics_group;
	AdwPreferencesGroup* audio_group;
	AdwPreferencesGroup* notifications_group;
	AdwPreferencesGroup* general_group;
	AdwPreferencesGroup* compatibility_group;
	AdwPreferencesGroup* network_group;
	AdwPreferencesGroup* debugging_group;
	AdwPreferencesGroup* rendering_group;
	AdwPreferencesGroup* game_folders_group;
	AdwPreferencesGroup* addons_group;
	AdwPreferencesGroup* controller_group;
	AdwPreferencesGroup* updates_group;
	AdwPreferencesGroup* about_group;
	GtkButton*           reset_button;
	GtkMenuButton*       game_configs_button;
	GtkButton*           save_button;

	kyty::gtk::SettingsWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytySettingsWindow, kyty_settings_window, ADW_TYPE_WINDOW)

namespace {

using kyty::gtk::SettingsWindowState;

guint g_saved_signal = 0;

// ---- Texts

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

std::string Join(const std::vector<std::string>& items, std::string_view separator) {
	std::string text;
	for (const auto& item: items) {
		if (!text.empty()) {
			text += separator;
		}
		text += item;
	}
	return text;
}

bool HasBackslash(std::string_view path) {
	return path.find('\\') != std::string_view::npos;
}

// The label of a field the editor does not show.
const char* HiddenFieldLabel(std::string_view key) {
	if (key == "elf") {
		return _("Executable");
	}
	if (key == "input_mapping_custom") {
		return _("Own input mapping");
	}
	if (key == "host_input_mapping") {
		return _("Input mapping");
	}
	return nullptr;
}

// "<label> (<key>)" for a Game_Configuration field, or "<key>" for an unknown one.
std::string FieldName(std::string_view key) {
	const std::string key_text(key);
	if (const auto* spec = FindSpec(key); spec != nullptr) {
		// TRANSLATORS: a name and, in parentheses, its key or title ID.
		return Format(_("%s (%s)"), _(spec->label), key_text.c_str());
	}
	if (const char* label = HiddenFieldLabel(key); label != nullptr) {
		return Format(_("%s (%s)"), label, key_text.c_str());
	}
	// TRANSLATORS: a quoted setting key.
	return Format(_("“%s”"), key_text.c_str());
}

// The rule of a field, as the launcher's own text (plan D46); the core's message (English) only for
// a field the launcher does not know.
std::string FieldRule(std::string_view key, const std::string& core_message) {
	if (key == "user_name") {
		return _("The user name must contain 1 to 16 UTF-8 bytes after leading and trailing spaces "
		         "are removed.");
	}
	if (key == "user_id") {
		return _("The user ID must be between 0 and 2147482879 and must not be 254 (everyone) or "
		         "255 (system).");
	}
	if (key == "gpu_index") {
		return _("The GPU must be Automatic or a device index from 0.");
	}
	if (key == "trophy_duration_seconds") {
		return _("The trophy notification duration must be between 5 and 10 seconds.");
	}
	if (key == "vblank_frequency") {
		return _("The vblank frequency must be between 30 and 360.");
	}
	if (key == "console_language") {
		return _("The console language must be one of the listed languages.");
	}
	if (key == "elf") {
		return _("The executable must be a non-empty relative path without a “..” component.");
	}
	if (key == "shader_log_folder") {
		return _("The shader log folder must not be empty while shader logs go to a file.");
	}
	if (key == "command_buffer_dump_folder") {
		return _("The command buffer folder must not be empty while command buffer dumps are "
		         "enabled.");
	}
	if (key == "printf_output_file") {
		return _("The printf output file must not be empty while printf output goes to a file.");
	}
	if (key == "host_input_mapping") {
		return _("Each input mapping entry must have the form Name=Value with a non-empty name "
		         "and value.");
	}
	if (const auto* field = core::FindGameConfigurationField(key);
	    field != nullptr && field->kind == core::FieldKind::Enum) {
		return _("The value must be one of the listed choices.");
	}
	if (const auto* field = core::FindGameConfigurationField(key);
	    field != nullptr && field->kind == core::FieldKind::Bool) {
		return _("The value must be true or false.");
	}
	return core_message;
}

std::string FieldErrorText(const core::ValidationError& error) {
	// TRANSLATORS: an invalid setting: "<label> (<key>): <rule>".
	return Format(_("%s: %s"), FieldName(error.field).c_str(),
	              FieldRule(error.field, error.message).c_str());
}

// The text of a Game_Settings_File import error (HC-19): the field and the title the error is
// about, in the launcher's words; the core's message only where it holds a path and a reason.
std::string ImportErrorText(const core::ImportError& error) {
	const char* title = error.title.c_str();
	const auto  field = FieldName(error.field);
	switch (error.kind) {
		case core::ImportErrorKind::Unreadable:
			return Format(_("The file could not be read: %s"), error.message.c_str());
		case core::ImportErrorKind::InvalidJson: return _("The file is not valid JSON.");
		case core::ImportErrorKind::NotAnObject:
			return _("The file must contain a JSON object with game configs keyed by title ID.");
		case core::ImportErrorKind::NoSettings: return _("The file contains no game configs.");
		case core::ImportErrorKind::InvalidTitle:
			return _("The file contains a game config without a title ID.");
		case core::ImportErrorKind::DuplicateTitle:
			return Format(_("The file contains the game config of %s more than once."), title);
		case core::ImportErrorKind::MemberNotAnObject:
			return Format(_("The game config of %s is not a JSON object."), title);
		case core::ImportErrorKind::EmptyMember:
			return Format(_("The game config of %s contains no setting."), title);
		case core::ImportErrorKind::DuplicateField:
			return Format(_("The game config of %s contains the setting %s more than once."), title,
			              field.c_str());
		case core::ImportErrorKind::UnknownField:
			return Format(_("The game config of %s contains the unknown setting %s."), title,
			              field.c_str());
		case core::ImportErrorKind::WrongType:
			return Format(_("The setting %s in the game config of %s has a value of the wrong "
			                "type."),
			              field.c_str(), title);
		case core::ImportErrorKind::InvalidValue:
			return Format(_("The setting %s in the game config of %s has an invalid value. %s"),
			              field.c_str(), title, FieldRule(error.field, error.message).c_str());
		case core::ImportErrorKind::NoMatchingMember:
			// Only the per-game import (task 21.7) selects a member for one game.
			if (error.title.empty()) {
				return _("The file has no game config for this game, which has no title ID.");
			}
			return Format(_("The file contains no game config for %s."), title);
	}
	return error.message;
}

// ---- Values

std::string ColorText(const GdkRGBA& rgba) {
	const auto channel = [](float value) {
		return static_cast<unsigned>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
	};
	return Format("#%02x%02x%02x", channel(rgba.red), channel(rgba.green), channel(rgba.blue));
}

std::optional<kyty::gtk::LightbarColor> LightbarOf(const std::string& color) {
	GdkRGBA rgba {};
	if (color.empty() || gdk_rgba_parse(&rgba, color.c_str()) == FALSE) {
		return std::nullopt;
	}
	const auto channel = [](float value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
	};
	return kyty::gtk::LightbarColor {channel(rgba.red), channel(rgba.green), channel(rgba.blue)};
}

std::vector<std::string> NormalizedFolders(std::span<const std::string> folders) {
	auto                     file_system = core::MakeHostScanFileSystem();
	std::vector<std::string> result;
	for (const auto& folder: core::NormalizeGameFolders(folders, *file_system)) {
		result.push_back(folder.string());
	}
	return result;
}

std::vector<core::GameEntry> LibraryEntries(LauncherContext& context) {
	std::vector<core::GameEntry> entries;
	GListModel*                  games = context.library.Games();
	const guint                  count = g_list_model_get_n_items(games);
	for (guint i = 0; i < count; i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		entries.push_back(kyty_game_get_entry(game.get()));
	}
	return entries;
}

GtkStringList* StringList(const std::vector<std::string>& items) {
	GtkStringList* list = gtk_string_list_new(nullptr);
	for (const auto& item: items) {
		gtk_string_list_append(list, item.c_str());
	}
	return list;
}

void SetComboItems(GtkWidget* row, const std::vector<std::string>& items) {
	auto list = GObjectPtr<GtkStringList>::Adopt(StringList(items));
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list.get()));
}

guint Selected(GtkWidget* row) {
	return adw_combo_row_get_selected(ADW_COMBO_ROW(row));
}

void SetPlainTitle(GtkWidget* row, const char* title) {
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
}

GtkWidget* WarningIcon(const char* text) {
	GtkWidget* icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
	gtk_widget_add_css_class(icon, "warning");
	gtk_widget_set_tooltip_text(icon, text);
	gtk_accessible_update_property(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
	return icon;
}

AdwPreferencesGroup* GroupWidget(KytySettingsWindow* self, Group group) {
	switch (group) {
		case Group::UserProfile: return self->user_profile_group;
		case Group::Graphics: return self->graphics_group;
		case Group::Audio: return self->audio_group;
		case Group::Notifications: return self->notifications_group;
		case Group::General: return self->general_group;
		case Group::Compatibility: return self->compatibility_group;
		case Group::Network: return self->network_group;
		case Group::Debugging: return self->debugging_group;
		case Group::Rendering: return self->rendering_group;
	}
	return self->general_group;
}

FieldRow* FindRow(SettingsWindowState& state, std::string_view key) {
	for (auto& row: state.rows) {
		if (row.spec->key == key) {
			return &row;
		}
	}
	return nullptr;
}

// ---- Field rows

GtkWidget* MakeFieldWidget(const FieldSpec& spec) {
	GtkWidget* row = nullptr;
	switch (spec.kind) {
		case RowKind::Switch: row = adw_switch_row_new(); break;
		case RowKind::Choice:
		case RowKind::Gpu:
		case RowKind::Microphone: row = adw_combo_row_new(); break;
		case RowKind::Number:
			row = adw_spin_row_new_with_range(static_cast<double>(spec.minimum),
			                                  static_cast<double>(spec.maximum), 1);
			adw_spin_row_set_numeric(ADW_SPIN_ROW(row), TRUE);
			adw_spin_row_set_digits(ADW_SPIN_ROW(row), 0);
			break;
		case RowKind::Text: row = adw_entry_row_new(); break;
	}
	SetPlainTitle(row, _(spec.label));
	if (spec.tooltip != nullptr) {
		gtk_widget_set_tooltip_text(row, _(spec.tooltip));
	}
	if (spec.kind == RowKind::Choice) {
		std::vector<std::string> items;
		for (const char* choice: spec.choices) {
			items.emplace_back(_(choice));
		}
		SetComboItems(row, items);
	}
	return row;
}

void LoadGpuRow(FieldRow& row, int64_t gpu_index, const kyty::gtk::DeviceList& gpus) {
	std::vector<std::string> items = {_("Automatic")};
	row.gpu_values                 = {-1};
	for (std::size_t i = 0; i < gpus.names.size(); i++) {
		items.push_back(gpus.names[i]);
		row.gpu_values.push_back(static_cast<int64_t>(i));
	}
	guint selected = 0;
	if (gpu_index >= 0) {
		if (static_cast<uint64_t>(gpu_index) < gpus.names.size()) {
			selected = static_cast<guint>(gpu_index + 1);
		} else {
			// A stored device that this host does not list stays selected (the Qt launcher fell
			// back to Automatic).
			items.push_back(Format(_("GPU %d (not found)"), static_cast<int>(gpu_index)));
			row.gpu_values.push_back(gpu_index);
			selected = static_cast<guint>(items.size() - 1);
		}
	}
	SetComboItems(row.row, items);
	adw_combo_row_set_selected(ADW_COMBO_ROW(row.row), selected);
	if (!gpus.error.empty()) {
		adw_action_row_set_subtitle(
		    ADW_ACTION_ROW(row.row),
		    Format(_("The GPUs could not be listed: %s"), gpus.error.c_str()).c_str());
	}
}

void LoadMicrophoneRow(FieldRow& row, const std::string& device,
                       const kyty::gtk::DeviceList& microphones) {
	// TRANSLATORS: no microphone; games get silence.
	std::vector<std::string> items = {C_("microphone", "None")};
	row.device_values              = {""};
	for (const auto& name: microphones.names) {
		if (std::find(row.device_values.begin(), row.device_values.end(), name) ==
		    row.device_values.end()) {
			items.push_back(name);
			row.device_values.push_back(name);
		}
	}
	auto found = std::find(row.device_values.begin(), row.device_values.end(), device);
	if (found == row.device_values.end()) {
		items.push_back(Format(_("%s (unavailable)"), device.c_str()));
		row.device_values.push_back(device);
		found = row.device_values.end() - 1;
	}
	SetComboItems(row.row, items);
	adw_combo_row_set_selected(ADW_COMBO_ROW(row.row),
	                           static_cast<guint>(found - row.device_values.begin()));
	if (!microphones.error.empty()) {
		adw_action_row_set_subtitle(
		    ADW_ACTION_ROW(row.row),
		    Format(_("The microphones could not be listed: %s"), microphones.error.c_str())
		        .c_str());
	}
}

void LoadRow(FieldRow& row, const core::GameConfiguration& configuration,
             const SettingsWindowState& state) {
	const core::FieldValue value = row.field->get(configuration);
	const auto*            flag  = std::get_if<bool>(&value);
	const auto*            text  = std::get_if<std::string>(&value);
	const auto*            index = std::get_if<int64_t>(&value);
	switch (row.spec->kind) {
		case RowKind::Switch:
			adw_switch_row_set_active(ADW_SWITCH_ROW(row.row),
			                          flag != nullptr && *flag ? TRUE : FALSE);
			break;
		case RowKind::Choice:
			adw_combo_row_set_selected(ADW_COMBO_ROW(row.row), index != nullptr && *index >= 0
			                                                       ? static_cast<guint>(*index)
			                                                       : 0);
			break;
		case RowKind::Number:
			adw_spin_row_set_value(ADW_SPIN_ROW(row.row),
			                       index != nullptr ? static_cast<double>(*index) : 0.0);
			break;
		case RowKind::Text:
			gtk_editable_set_text(GTK_EDITABLE(row.row), text != nullptr ? text->c_str() : "");
			break;
		case RowKind::Gpu: LoadGpuRow(row, index != nullptr ? *index : -1, state.gpus); break;
		case RowKind::Microphone:
			LoadMicrophoneRow(row, text != nullptr ? *text : std::string(), state.microphones);
			break;
	}
}

core::FieldValue ReadRow(const FieldRow& row) {
	switch (row.spec->kind) {
		case RowKind::Switch: return adw_switch_row_get_active(ADW_SWITCH_ROW(row.row)) != FALSE;
		case RowKind::Choice: return static_cast<int64_t>(Selected(row.row));
		case RowKind::Number:
			adw_spin_row_update(ADW_SPIN_ROW(row.row));
			return static_cast<int64_t>(
			    std::llround(adw_spin_row_get_value(ADW_SPIN_ROW(row.row))));
		case RowKind::Text: {
			std::string text = gtk_editable_get_text(GTK_EDITABLE(row.row));
			// As the Qt launcher, the user name is stored without surrounding whitespace.
			if (row.spec->key == "user_name") {
				text = std::string(core::TrimWhitespace(text));
			}
			return text;
		}
		case RowKind::Gpu: {
			const guint selected = Selected(row.row);
			return selected < row.gpu_values.size() ? row.gpu_values[selected] : int64_t {-1};
		}
		case RowKind::Microphone: {
			const guint selected = Selected(row.row);
			return selected < row.device_values.size() ? row.device_values[selected]
			                                           : std::string();
		}
	}
	return false;
}

// The value a dependent row compares with: 1 or 0 for a switch, the index of a choice.
int64_t ReadIndex(const FieldRow& row) {
	if (row.spec->kind == RowKind::Switch) {
		return adw_switch_row_get_active(ADW_SWITCH_ROW(row.row)) != FALSE ? 1 : 0;
	}
	return static_cast<int64_t>(Selected(row.row));
}

void UpdateDependents(SettingsWindowState& state) {
	for (auto& row: state.rows) {
		if (row.spec->enabled_by.empty()) {
			continue;
		}
		const FieldRow* controller = FindRow(state, row.spec->enabled_by);
		const bool      enabled =
		    controller != nullptr && ReadIndex(*controller) == row.spec->enabled_when;
		gtk_widget_set_sensitive(row.row, enabled ? TRUE : FALSE);
	}
}

void LoadRows(SettingsWindowState& state, const core::GameConfiguration& configuration) {
	state.loading = true;
	for (auto& row: state.rows) {
		LoadRow(row, configuration, state);
	}
	state.loading = false;
	UpdateDependents(state);
}

// The configuration the editor shows: the opened one with every shown field from its row.
core::GameConfiguration EditedConfiguration(const SettingsWindowState& state) {
	core::GameConfiguration configuration = state.base;
	for (const auto& row: state.rows) {
		if (!row.field->set(configuration, ReadRow(row))) {
			g_critical("settings field %s: the row's value does not fit the field",
			           std::string(row.spec->key).c_str());
		}
	}
	return configuration;
}

void OnDependencyChanged(GObject* /*row*/, GParamSpec* /*pspec*/, gpointer data) {
	auto* self = KYTY_SETTINGS_WINDOW(data);
	if (self->state != nullptr && !self->state->disposed) {
		UpdateDependents(*self->state);
	}
}

void BuildFieldRows(KytySettingsWindow* self) {
	auto& state = *self->state;
	for (const auto& spec: kFields) {
		const auto* field = core::FindGameConfigurationField(spec.key);
		if (field == nullptr) {
			g_critical("settings field %s is not a Game_Configuration field",
			           std::string(spec.key).c_str());
			continue;
		}
		FieldRow row;
		row.spec  = &spec;
		row.field = field;
		row.row   = MakeFieldWidget(spec);
		adw_preferences_group_add(GroupWidget(self, spec.group), row.row);
		state.rows.push_back(std::move(row));
	}
	// The rows that other rows depend on (LFI-19).
	for (const auto& row: state.rows) {
		const bool controls =
		    std::any_of(std::begin(kFields), std::end(kFields),
		                [&row](const FieldSpec& spec) { return spec.enabled_by == row.spec->key; });
		if (!controls) {
			continue;
		}
		const char* property =
		    row.spec->kind == RowKind::Switch ? "notify::active" : "notify::selected";
		state.signals.push_back(
		    SignalScope::Connect(row.row, property, G_CALLBACK(OnDependencyChanged), self));
	}
}

void ClearRowErrors(SettingsWindowState& state) {
	for (auto& row: state.rows) {
		gtk_widget_remove_css_class(row.row, "error");
	}
}

// ---- Controller page (global mode; LFI-22, Requirement 11.15)

void ShowColor(SettingsWindowState& state) {
	const bool custom = !state.color.empty();
	if (custom) {
		std::string upper = state.color;
		std::transform(upper.begin(), upper.end(), upper.begin(),
		               [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 32) : c; });
		adw_action_row_set_subtitle(ADW_ACTION_ROW(state.color_row), upper.c_str());
	} else {
		adw_action_row_set_subtitle(ADW_ACTION_ROW(state.color_row),
		                            _("Default: the game controls the lightbar"));
	}
	gtk_widget_set_sensitive(state.color_default_button, custom ? TRUE : FALSE);
	GdkRGBA rgba {};
	gdk_rgba_parse(&rgba, custom ? state.color.c_str() : kDefaultPreviewColor);
	if (ColorText(rgba) == kDefaultPreviewColor) {
		rgba.alpha = kStartColorAlpha;
	}
	const bool loading = std::exchange(state.loading, true);
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(state.color_button), &rgba);
	state.loading = loading;
}

// The live preview on the connected controllers (11.15): at once, from the main thread.
void PreviewColor(SettingsWindowState& state) {
	state.context->services.controller.SetLightbarColor(LightbarOf(state.color));
	state.previewed = true;
}

void OnColorChosen(GObject* button, GParamSpec* /*pspec*/, gpointer data) {
	auto* self  = KYTY_SETTINGS_WINDOW(data);
	auto& state = *self->state;
	if (state.loading || state.disposed) {
		return;
	}
	const GdkRGBA*    rgba   = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
	const std::string chosen = ColorText(*rgba);
	// The same color again changes nothing. This also ends the emission that GObject restarts
	// after ShowColor() below (notify is not emitted recursively, so its notify comes afterwards).
	if (chosen == state.color) {
		return;
	}
	state.color = chosen;
	ShowColor(state);
	PreviewColor(state);
}

void OnColorDefault(GtkButton* /*button*/, gpointer data) {
	auto& state = *KYTY_SETTINGS_WINDOW(data)->state;
	state.color.clear();
	ShowColor(state);
	PreviewColor(state);
}

gchar* FormatPercent(GtkScale* /*scale*/, double value, gpointer /*data*/) {
	// TRANSLATORS: a percentage, for example "50%".
	return g_strdup_printf(_("%d%%"), static_cast<int>(std::lround(value)));
}

GtkWidget* PercentRow(AdwPreferencesGroup* group, const char* title, const char* tooltip,
                      GtkWidget** scale_out) {
	GtkWidget* row = adw_action_row_new();
	SetPlainTitle(row, title);
	gtk_widget_set_tooltip_text(row, tooltip);
	GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_digits(GTK_SCALE(scale), 0);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
	gtk_scale_set_format_value_func(GTK_SCALE(scale), FormatPercent, nullptr, nullptr);
	// style.css keeps the value clear of the knob at the end of the trough.
	gtk_widget_add_css_class(scale, "percent-scale");
	gtk_widget_set_size_request(scale, 260, -1);
	gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(scale), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
	adw_preferences_group_add(group, row);
	*scale_out = scale;
	return row;
}

void LoadController(SettingsWindowState& state, const core::ControllerSettings& controller) {
	state.color = controller.color;
	ShowColor(state);
	gtk_range_set_value(GTK_RANGE(state.vibration_scale), controller.vibration_intensity);
	gtk_range_set_value(GTK_RANGE(state.volume_scale), controller.speaker_volume);
	adw_combo_row_set_selected(
	    ADW_COMBO_ROW(state.keyboard_player_row),
	    static_cast<guint>(std::clamp(controller.keyboard_player, 0, core::kMaxKeyboardPlayer)));
}

core::ControllerSettings ReadController(const SettingsWindowState& state) {
	core::ControllerSettings controller;
	controller.color = state.color;
	controller.vibration_intensity =
	    static_cast<int32_t>(std::lround(gtk_range_get_value(GTK_RANGE(state.vibration_scale))));
	controller.speaker_volume =
	    static_cast<int32_t>(std::lround(gtk_range_get_value(GTK_RANGE(state.volume_scale))));
	controller.keyboard_player = static_cast<int32_t>(Selected(state.keyboard_player_row));
	return controller;
}

void BuildControllerPage(KytySettingsWindow* self) {
	auto& state     = *self->state;
	auto* group     = self->controller_group;
	state.color_row = adw_action_row_new();
	SetPlainTitle(state.color_row, _("DualSense lightbar"));
	GtkColorDialog* dialog = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(dialog, FALSE);
	gtk_color_dialog_set_title(dialog, _("DualSense lightbar color"));
	gtk_color_dialog_set_modal(dialog, TRUE);
	state.color_button = gtk_color_dialog_button_new(dialog);
	gtk_widget_set_valign(state.color_button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(state.color_button,
	                            _("Choose and preview a fixed LED color. Bluetooth DualSense may "
	                              "take several seconds to update."));
	gtk_accessible_update_property(GTK_ACCESSIBLE(state.color_button),
	                               GTK_ACCESSIBLE_PROPERTY_LABEL, _("DualSense lightbar color"),
	                               -1);
	// TRANSLATORS: the button that lets the game control the lightbar color again.
	state.color_default_button = gtk_button_new_with_label(C_("lightbar", "Default"));
	gtk_widget_set_valign(state.color_default_button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(state.color_default_button, _("Let the game control the lightbar"));
	adw_action_row_add_suffix(ADW_ACTION_ROW(state.color_row), state.color_button);
	adw_action_row_add_suffix(ADW_ACTION_ROW(state.color_row), state.color_default_button);
	adw_preferences_group_add(group, state.color_row);
	state.signals.push_back(
	    SignalScope::Connect(state.color_button, "notify::rgba", G_CALLBACK(OnColorChosen), self));
	state.signals.push_back(SignalScope::Connect(state.color_default_button, "clicked",
	                                             G_CALLBACK(OnColorDefault), self));

	PercentRow(group, _("Vibration intensity"),
	           _("Scale DualSense vibration over USB and Bluetooth"), &state.vibration_scale);
	PercentRow(group, _("Speaker volume"),
	           _("Scale the DualSense speaker over USB and Bluetooth. 50% is the previous default; "
	             "100% adds 6 dB of gain."),
	           &state.volume_scale);

	state.keyboard_player_row = adw_combo_row_new();
	SetPlainTitle(state.keyboard_player_row, _("Keyboard player"));
	gtk_widget_set_tooltip_text(state.keyboard_player_row,
	                            _("Gamepads fill the remaining players in connection order. Every "
	                              "player uses the same input mapping."));
	// TRANSLATORS: the default keyboard player: the keyboard and the first gamepad are player 1.
	std::vector<std::string> players = {_("Shared with player 1")};
	for (int player = 1; player <= core::kMaxKeyboardPlayer; player++) {
		players.push_back(Format(_("Player %d"), player));
	}
	SetComboItems(state.keyboard_player_row, players);
	adw_preferences_group_add(group, state.keyboard_player_row);
}

// ---- Folders page (global mode; LFI-20, Requirements 7.1, 7.2, HC-20)

void UpdateRemoveButton(SettingsWindowState& state) {
	GList*     selected = gtk_list_box_get_selected_rows(state.folder_list);
	const bool any      = selected != nullptr;
	g_list_free(selected);
	gtk_widget_set_sensitive(state.remove_folders_button, any ? TRUE : FALSE);
}

void ShowFolders(SettingsWindowState& state) {
	// Only the rows: gtk_list_box_remove_all() also removes the placeholder ("No game folders"),
	// which then never showed on a first run.
	while (GtkListBoxRow* row = gtk_list_box_get_row_at_index(state.folder_list, 0)) {
		gtk_list_box_remove(state.folder_list, GTK_WIDGET(row));
	}
	for (const auto& folder: state.folders) {
		GtkWidget* row = adw_action_row_new();
		SetPlainTitle(row, folder.c_str());
		if (HasBackslash(folder)) {
			const char* warning = _("This path contains a backslash (\\). The emulator turns it "
			                        "into a slash (/), so the games in this folder may not start.");
			adw_action_row_set_subtitle(ADW_ACTION_ROW(row), warning);
			adw_action_row_add_prefix(ADW_ACTION_ROW(row), WarningIcon(warning));
		}
		gtk_list_box_append(state.folder_list, row);
	}
	UpdateRemoveButton(state);
}

void UpdateAddonsWarning(SettingsWindowState& state) {
	const char* text = gtk_editable_get_text(GTK_EDITABLE(state.addons_row));
	gtk_widget_set_visible(state.addons_warning, HasBackslash(text) ? TRUE : FALSE);
}

void OnFolderSelectionChanged(GtkListBox* /*list*/, gpointer data) {
	auto* self = KYTY_SETTINGS_WINDOW(data);
	if (self->state != nullptr && !self->state->disposed) {
		UpdateRemoveButton(*self->state);
	}
}

void OnAddonsChanged(GtkEditable* /*editable*/, gpointer data) {
	auto* self = KYTY_SETTINGS_WINDOW(data);
	if (self->state != nullptr && !self->state->disposed) {
		UpdateAddonsWarning(*self->state);
	}
}

void OnAddFolders(GtkButton* /*button*/, gpointer data) {
	auto* self  = KYTY_SETTINGS_WINDOW(data);
	auto& state = *self->state;
	state.context->services.file_chooser.SelectFolders(
	    GTK_WINDOW(self), _("Add game folders"),
	    [self](std::vector<fs::path> folders, const std::string& error) {
		    auto& chosen_state = *self->state;
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The folders could not be chosen"), error,
			                         chosen_state.cancellable.get());
			    return;
		    }
		    if (folders.empty()) {
			    return;
		    }
		    std::vector<std::string> all = chosen_state.folders;
		    for (const auto& folder: folders) {
			    all.push_back(folder.string());
		    }
		    // One entry per resolved directory (Requirement 7.1), as the Qt launcher kept it.
		    chosen_state.folders = NormalizedFolders(all);
		    ShowFolders(chosen_state);
	    },
	    state.cancellable.get());
}

void OnRemoveFolders(GtkButton* /*button*/, gpointer data) {
	auto&              state    = *KYTY_SETTINGS_WINDOW(data)->state;
	GList*             selected = gtk_list_box_get_selected_rows(state.folder_list);
	std::vector<guint> indexes;
	for (GList* item = selected; item != nullptr; item = item->next) {
		indexes.push_back(
		    static_cast<guint>(gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(item->data))));
	}
	g_list_free(selected);
	std::sort(indexes.rbegin(), indexes.rend());
	for (const guint index: indexes) {
		if (index < state.folders.size()) {
			state.folders.erase(state.folders.begin() + static_cast<std::ptrdiff_t>(index));
		}
	}
	ShowFolders(state);
}

void OnChooseAddons(GtkButton* /*button*/, gpointer data) {
	auto* self  = KYTY_SETTINGS_WINDOW(data);
	auto& state = *self->state;
	state.context->services.file_chooser.SelectFolders(
	    GTK_WINDOW(self), _("Select add-ons folder"),
	    [self](std::vector<fs::path> folders, const std::string& error) {
		    auto& chosen_state = *self->state;
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The folder could not be chosen"), error,
			                         chosen_state.cancellable.get());
			    return;
		    }
		    if (!folders.empty()) {
			    gtk_editable_set_text(GTK_EDITABLE(chosen_state.addons_row),
			                          folders.front().c_str());
		    }
	    },
	    state.cancellable.get());
}

// The add-ons directory to store: the trimmed text made absolute, as the Qt launcher stored it.
std::string ReadAddonsDirectory(const SettingsWindowState& state) {
	const std::string text(
	    core::TrimWhitespace(gtk_editable_get_text(GTK_EDITABLE(state.addons_row))));
	if (text.empty()) {
		return {};
	}
	const std::string folders[]  = {text};
	const auto        normalized = NormalizedFolders(folders);
	return normalized.empty() ? text : normalized.front();
}

void BuildFoldersPage(KytySettingsWindow* self) {
	auto& state   = *self->state;
	auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
	GtkWidget* add = gtk_button_new_with_label(_("Add folders…"));
	gtk_widget_set_tooltip_text(add, _("Add game folders"));
	state.remove_folders_button = gtk_button_new_with_label(_("Remove"));
	gtk_widget_set_tooltip_text(state.remove_folders_button, _("Remove the selected game folders"));
	gtk_box_append(GTK_BOX(buttons), add);
	gtk_box_append(GTK_BOX(buttons), state.remove_folders_button);
	adw_preferences_group_set_header_suffix(self->game_folders_group, buttons);

	state.folder_list = GTK_LIST_BOX(gtk_list_box_new());
	gtk_list_box_set_selection_mode(state.folder_list, GTK_SELECTION_MULTIPLE);
	gtk_widget_add_css_class(GTK_WIDGET(state.folder_list), "boxed-list");
	GtkWidget* placeholder =
	    gtk_label_new(_("No game folders. Add the folders that hold your games."));
	gtk_label_set_wrap(GTK_LABEL(placeholder), TRUE);
	gtk_widget_add_css_class(placeholder, "dim-label");
	gtk_widget_set_margin_top(placeholder, 18);
	gtk_widget_set_margin_bottom(placeholder, 18);
	gtk_widget_set_margin_start(placeholder, 12);
	gtk_widget_set_margin_end(placeholder, 12);
	gtk_list_box_set_placeholder(state.folder_list, placeholder);
	adw_preferences_group_add(self->game_folders_group, GTK_WIDGET(state.folder_list));

	state.addons_row = adw_entry_row_new();
	SetPlainTitle(state.addons_row, _("Add-ons folder"));
	GtkWidget* choose = gtk_button_new_from_icon_name("folder-open-symbolic");
	gtk_widget_add_css_class(choose, "flat");
	gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(choose, _("Select add-ons folder"));
	gtk_accessible_update_property(GTK_ACCESSIBLE(choose), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               _("Select add-ons folder"), -1);
	adw_entry_row_add_suffix(ADW_ENTRY_ROW(state.addons_row), choose);
	adw_preferences_group_add(self->addons_group, state.addons_row);
	const char* warning  = _("The add-ons folder contains a backslash (\\). The emulator turns it "
	                         "into a slash (/), so it may not find the add-ons.");
	state.addons_warning = adw_action_row_new();
	SetPlainTitle(state.addons_warning, warning);
	adw_action_row_add_prefix(ADW_ACTION_ROW(state.addons_warning), WarningIcon(warning));
	gtk_widget_set_visible(state.addons_warning, FALSE);
	adw_preferences_group_add(self->addons_group, state.addons_warning);

	state.signals.push_back(SignalScope::Connect(add, "clicked", G_CALLBACK(OnAddFolders), self));
	state.signals.push_back(SignalScope::Connect(state.remove_folders_button, "clicked",
	                                             G_CALLBACK(OnRemoveFolders), self));
	state.signals.push_back(SignalScope::Connect(state.folder_list, "selected-rows-changed",
	                                             G_CALLBACK(OnFolderSelectionChanged), self));
	state.signals.push_back(
	    SignalScope::Connect(choose, "clicked", G_CALLBACK(OnChooseAddons), self));
	state.signals.push_back(
	    SignalScope::Connect(state.addons_row, "changed", G_CALLBACK(OnAddonsChanged), self));
}

void LoadFolders(SettingsWindowState& state, const core::LauncherSettings& settings) {
	state.folders = NormalizedFolders(settings.game_folders);
	ShowFolders(state);
	gtk_editable_set_text(GTK_EDITABLE(state.addons_row), settings.addons_dir.c_str());
	UpdateAddonsWarning(state);
}

// ---- Updates page (global mode; LFI-34, Requirement 13.3)

void OnReleasesPage(AdwActionRow* /*row*/, gpointer data) {
	auto* self  = KYTY_SETTINGS_WINDOW(data);
	auto& state = *self->state;
	state.context->services.desktop_launcher.OpenUri(
	    GTK_WINDOW(self), std::string(kyty::updater::kReleasesPage),
	    [self](const std::string& error) {
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The releases page could not be opened"),
			                         error, self->state->cancellable.get());
		    }
	    },
	    state.cancellable.get());
}

void BuildUpdatesPage(KytySettingsWindow* self, const core::LauncherSettings& settings) {
	auto& state = *self->state;
	switch (state.context->update_mode) {
		case kyty::updater::UpdateUiMode::Official:
			state.check_updates_row = adw_switch_row_new();
			SetPlainTitle(state.check_updates_row, _("Check for updates on startup"));
			adw_action_row_set_subtitle(
			    ADW_ACTION_ROW(state.check_updates_row),
			    _("Look for a newer release each time the launcher starts."));
			adw_switch_row_set_active(ADW_SWITCH_ROW(state.check_updates_row),
			                          settings.check_updates_on_startup ? TRUE : FALSE);
			adw_preferences_group_add(self->updates_group, state.check_updates_row);
			break;
		case kyty::updater::UpdateUiMode::Other: {
			adw_preferences_group_set_description(
			    self->updates_group, _("This build does not install updates. New releases are "
			                           "published on the project's releases page."));
			GtkWidget* row = adw_action_row_new();
			SetPlainTitle(row, _("Releases page"));
			adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
			                            std::string(kyty::updater::kReleasesPage).c_str());
			gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
			GtkWidget* icon = gtk_image_new_from_icon_name("web-browser-symbolic");
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon);
			gtk_widget_set_tooltip_text(row, _("Open the releases page in the web browser"));
			adw_preferences_group_add(self->updates_group, row);
			state.signals.push_back(
			    SignalScope::Connect(row, "activated", G_CALLBACK(OnReleasesPage), self));
			break;
		}
		case kyty::updater::UpdateUiMode::Flatpak:
			// Flatpak: no update control and no update setting (13.3).
			adw_view_stack_page_set_visible(self->updates_page, FALSE);
			break;
	}
}

// ---- About this installation (global mode; LFI-32, Requirement 12.5)

void AboutRow(AdwPreferencesGroup* group, const char* title, const std::string& value) {
	GtkWidget* row = adw_action_row_new();
	SetPlainTitle(row, title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value.c_str());
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
	gtk_widget_add_css_class(row, "property");
	adw_preferences_group_add(group, row);
}

void BuildAboutPage(KytySettingsWindow* self) {
	const LauncherContext& context = *self->state->context;
	AboutRow(self->about_group, _("Settings file"), context.paths.settings_file.string());
	AboutRow(self->about_group, _("Data directory"), context.paths.data_dir.string());
	AboutRow(self->about_group, _("Emulator"),
	         context.emulator.found
	             ? context.emulator.executable.string()
	             : Format(_("Not found at %s"), context.emulator.executable.c_str()));
	AboutRow(self->about_group, _("Emulator version"),
	         context.emulator_version.empty() ? std::string(_("Unknown"))
	                                          : context.emulator_version);
}

// ---- Writing the Settings_Store (Y2, HC-16, Requirement 12.8)

// Writes next as the Settings_Store (settingsWrite.h). On success the context's settings become
// next, "saved" is emitted, and done runs. A store that could not be loaded is replaced only after
// the user chose Overwrite; any other failure is shown with the path and the reason, and nothing
// changes.
void WriteStore(KytySettingsWindow* self, core::LauncherSettings next, std::function<void()> done) {
	auto& state = *self->state;
	kyty::gtk::SaveSettingsInteractively(GTK_WINDOW(self), *state.context, std::move(next),
	                                     state.cancellable.get(), [self, done]() {
		                                     g_signal_emit(self, g_saved_signal, 0);
		                                     if (done) {
			                                     done();
		                                     }
	                                     });
}

// ---- Geometry (LFI-47)

std::optional<core::LauncherWindowState> CurrentGeometry(KytySettingsWindow* self) {
	int width  = 0;
	int height = 0;
	gtk_window_get_default_size(GTK_WINDOW(self), &width, &height);
	if (width < 1 || height < 1) {
		return std::nullopt;
	}
	return core::LauncherWindowState {width, height,
	                                  gtk_window_is_maximized(GTK_WINDOW(self)) != FALSE};
}

// ---- Save, Reset

void SaveEdits(KytySettingsWindow* self);

void ShowFieldError(KytySettingsWindow* self, const core::ValidationError& error) {
	auto& state = *self->state;
	if (FieldRow* row = FindRow(state, error.field); row != nullptr) {
		adw_view_stack_set_visible_child_name(self->stack, PageOf(row->spec->group));
		gtk_widget_add_css_class(row->row, "error");
		gtk_widget_grab_focus(row->row);
	}
	kyty::gtk::ShowError(GTK_WINDOW(self), _("The settings could not be saved"),
	                     FieldErrorText(error), state.cancellable.get());
}

// Y1 (HC-18): the input mapping in use has entries that are not Name=Value; they are listed,
// numbered from 1, and removed only when the user agrees.
void OfferDroppingInvalidEntries(KytySettingsWindow*            self,
                                 const core::GameConfiguration& configuration) {
	auto&       state   = *self->state;
	const auto  invalid = core::FindInvalidMappingEntries(configuration.host_input_mapping);
	std::string body =
	    state.entry ? _("The input mapping of this game has entries that are not of the form "
	                    "Name=Value:")
	                : _("The global input mapping has entries that are not of the form "
	                    "Name=Value:");
	for (const auto& entry: invalid) {
		body += "\n";
		// TRANSLATORS: an invalid input mapping entry: its number (from 1) and its text.
		body += Format(_("Entry %u: “%s”"), static_cast<unsigned>(entry.index + 1),
		               entry.entry.c_str());
	}
	body += "\n\n";
	body += _("The emulator ignores these entries. Remove them and save?");
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Invalid input mapping entries"), body, _("Remove and save"),
	    _("Cancel"), true,
	    [self](bool accepted) {
		    if (!accepted) {
			    return;
		    }
		    auto& accepted_state = *self->state;
		    accepted_state.base.host_input_mapping =
		        core::DropInvalidMappingEntries(accepted_state.base.host_input_mapping);
		    SaveEdits(self);
	    },
	    state.cancellable.get());
}

void SaveEdits(KytySettingsWindow* self) {
	auto& state = *self->state;
	ClearRowErrors(state);
	const core::GameConfiguration configuration = EditedConfiguration(state);
	const auto scope = state.entry ? core::ValidationScope::Game : core::ValidationScope::Global;
	if (auto error = core::Validate(configuration, scope)) {
		if (error->field == "host_input_mapping") {
			OfferDroppingInvalidEntries(self, configuration);
		} else {
			ShowFieldError(self, *error);
		}
		return;
	}
	// Merge only this editor's part into the current settings (plan D37).
	core::LauncherSettings next = state.context->settings.loaded.settings;
	if (state.entry) {
		if (auto saved = core::SaveCustomSettings(next, *state.entry, configuration); !saved) {
			ShowFieldError(self, saved.error());
			return;
		}
	} else {
		next.global                      = configuration;
		next.controller                  = ReadController(state);
		std::vector<std::string> folders = state.folders;
		next.game_folders                = NormalizedFolders(folders);
		next.addons_dir                  = ReadAddonsDirectory(state);
		if (state.check_updates_row != nullptr) {
			next.check_updates_on_startup =
			    adw_switch_row_get_active(ADW_SWITCH_ROW(state.check_updates_row)) != FALSE;
		}
	}
	if (const auto geometry = CurrentGeometry(self)) {
		next.settings_window = *geometry;
	}
	WriteStore(self, std::move(next), [self]() { gtk_window_close(GTK_WINDOW(self)); });
}

void OnSave(GtkButton* /*button*/, gpointer data) {
	SaveEdits(KYTY_SETTINGS_WINDOW(data));
}

// Reset (plan D38): the defaults of the shown fields; the folders and the add-ons directory stay,
// and nothing is written until Save.
void OnReset(GtkButton* /*button*/, gpointer data) {
	auto& state = *KYTY_SETTINGS_WINDOW(data)->state;
	ClearRowErrors(state);
	if (state.entry) {
		LoadRows(state, state.context->settings.loaded.settings.global);
		return;
	}
	LoadRows(state, core::GameConfiguration {});
	const bool had_color = !state.color.empty();
	LoadController(state, core::ControllerSettings {});
	if (had_color) {
		PreviewColor(state);
	}
	if (state.check_updates_row != nullptr) {
		adw_switch_row_set_active(ADW_SWITCH_ROW(state.check_updates_row),
		                          core::LauncherSettings {}.check_updates_on_startup ? TRUE
		                                                                             : FALSE);
	}
}

// ---- Game configs (global mode; LFI-17, plan D39, HC-19, Requirements 9.8-9.10, 9.15)

void ExportGame(KytySettingsWindow* self, const core::GameEntry& entry) {
	auto&             state    = *self->state;
	const std::string title_id = core::NormalizeTitleId(entry.title_id);
	state.context->services.file_chooser.SaveFile(
	    GTK_WINDOW(self), _("Export game config"), title_id + ".json",
	    [self, entry](std::optional<fs::path> file, const std::string& error) {
		    auto& chosen_state = *self->state;
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The file could not be chosen"), error,
			                         chosen_state.cancellable.get());
			    return;
		    }
		    if (!file) {
			    return;
		    }
		    // The configuration the game runs with: its custom settings, or the global ones.
		    const auto configuration =
		        core::EnableCustomSettings(chosen_state.context->settings.loaded.settings, entry);
		    if (auto written = core::WriteGameSettingsFile(*file, entry.title_id, configuration);
		        !written) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The game config could not be exported"),
			                         Format(_("%s could not be written:\n%s"), file->c_str(),
			                                written.error().message.c_str()),
			                         chosen_state.cancellable.get());
		    }
	    },
	    state.cancellable.get());
}

void OnExportGameConfig(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto*      self    = KYTY_SETTINGS_WINDOW(data);
	const auto entries = LibraryEntries(*self->state->context);
	auto*      picker  = kyty_game_picker_dialog_new(
	    GTK_WINDOW(self), entries, _("Export game config"),
	    _("Choose the game whose config to write to a file."), _("Export"),
	    [window = GObjectPtr<KytySettingsWindow>::Ref(self)](const core::GameEntry& entry) {
		    if (window->state != nullptr && !window->state->disposed) {
			    ExportGame(window.get(), entry);
		    }
	    });
	gtk_window_present(GTK_WINDOW(picker));
}

struct ImportedGame {
	core::GameEntry         entry;
	core::GameConfiguration configuration;
};

void ApplyImport(KytySettingsWindow* self, const std::vector<ImportedGame>& games) {
	auto&                  state = *self->state;
	core::LauncherSettings next  = state.context->settings.loaded.settings;
	for (const auto& game: games) {
		if (auto saved = core::SaveCustomSettings(next, game.entry, game.configuration); !saved) {
			kyty::gtk::ShowError(GTK_WINDOW(self), _("The game configs could not be imported"),
			                     FieldErrorText(saved.error()), state.cancellable.get());
			return;
		}
	}
	WriteStore(self, std::move(next), nullptr);
}

void ImportFile(KytySettingsWindow* self, const fs::path& path) {
	auto&       state   = *self->state;
	const char* heading = _("The game configs could not be imported");
	auto        result  = core::ReadGameSettingsFile(path);
	if (!result) {
		kyty::gtk::ShowError(GTK_WINDOW(self), heading, ImportErrorText(result.error()),
		                     state.cancellable.get());
		return;
	}
	const auto&               settings = state.context->settings.loaded.settings;
	std::vector<ImportedGame> games;
	std::set<std::string>     matched;
	for (const auto& entry: LibraryEntries(*state.context)) {
		const auto title = core::NormalizeTitleId(entry.title_id);
		const auto found = result->by_title.find(title);
		if (title.empty() || found == result->by_title.end()) {
			continue;
		}
		// Fields the file does not have keep the game's current values (Requirement 9.9).
		auto configuration =
		    core::ApplyPatch(core::EnableCustomSettings(settings, entry), found->second);
		if (auto error = core::Validate(configuration, core::ValidationScope::Game)) {
			kyty::gtk::ShowError(GTK_WINDOW(self), heading,
			                     Format(_("The game config of %s does not fit %s. %s"),
			                            title.c_str(), entry.title_name.c_str(),
			                            FieldErrorText(*error).c_str()),
			                     state.cancellable.get());
			return;
		}
		games.push_back({entry, std::move(configuration)});
		matched.insert(title);
	}
	std::vector<std::string> unmatched;
	for (const auto& [title, patch]: result->by_title) {
		if (!matched.contains(title)) {
			unmatched.push_back(title);
		}
	}
	if (games.empty()) {
		kyty::gtk::ShowError(GTK_WINDOW(self), _("No matching games"),
		                     Format(_("No game in the library has a title ID of the file: %s"),
		                            Join(unmatched, ", ").c_str()),
		                     state.cancellable.get());
		return;
	}
	std::string body = _("Configs will be updated for the following games:");
	body += "\n";
	for (const auto& game: games) {
		// TRANSLATORS: one game of the import confirmation: "<title ID> — <title>".
		body += "\n" + Format(_("%s — %s"), core::NormalizeTitleId(game.entry.title_id).c_str(),
		                      game.entry.title_name.c_str());
	}
	if (!unmatched.empty()) {
		body += "\n\n" + Format(_("Games not found: %s"), Join(unmatched, ", ").c_str());
	}
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Import game configs?"), body, _("Import"), _("Cancel"), false,
	    [self, games](bool accepted) {
		    if (accepted) {
			    ApplyImport(self, games);
		    }
	    },
	    state.cancellable.get());
}

void OnImportGameConfigs(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	auto* self  = KYTY_SETTINGS_WINDOW(data);
	auto& state = *self->state;
	state.context->services.file_chooser.OpenFile(
	    GTK_WINDOW(self), _("Import game configs"), {{_("JSON files"), {"*.json"}}},
	    [self](std::optional<fs::path> file, const std::string& error) {
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The file could not be chosen"), error,
			                         self->state->cancellable.get());
			    return;
		    }
		    if (file) {
			    ImportFile(self, *file);
		    }
	    },
	    state.cancellable.get());
}

void AddGameConfigActions(KytySettingsWindow* self) {
	auto& state   = *self->state;
	state.actions = GObjectPtr<GSimpleActionGroup>::Adopt(g_simple_action_group_new());
	static const GActionEntry kActions[] = {
	    {"import-game-configs", OnImportGameConfigs, nullptr, nullptr, nullptr, {0, 0, 0}},
	    {"export-game-config", OnExportGameConfig, nullptr, nullptr, nullptr, {0, 0, 0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(state.actions.get()), kActions,
	                                G_N_ELEMENTS(kActions), self);
	gtk_widget_insert_action_group(GTK_WIDGET(self), "settings",
	                               G_ACTION_GROUP(state.actions.get()));
}

// ---- Construction

KytySettingsWindow* NewWindow(LauncherContext& context, GtkWindow* parent,
                              const core::GameEntry* entry) {
	auto* self    = KYTY_SETTINGS_WINDOW(g_object_new(KYTY_TYPE_SETTINGS_WINDOW, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	if (entry != nullptr) {
		state.entry = *entry;
	}
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	const core::LauncherSettings& settings = context.settings.loaded.settings;
	const auto&                   geometry = settings.settings_window;
	gtk_window_set_default_size(GTK_WINDOW(self), geometry.width, geometry.height);
	if (geometry.maximized) {
		gtk_window_maximize(GTK_WINDOW(self));
	}

	// The device lists, read when the editor opens (Requirement 9.5).
	state.gpus        = context.services.device_lists.VulkanDevices();
	state.microphones = context.services.device_lists.RecordingDevices();
	if (!state.gpus.error.empty()) {
		g_message("cannot list the Vulkan devices: %s", state.gpus.error.c_str());
	}
	if (!state.microphones.error.empty()) {
		g_message("cannot list the recording devices: %s", state.microphones.error.c_str());
	}

	state.base = entry != nullptr ? core::EnableCustomSettings(settings, *entry) : settings.global;
	BuildFieldRows(self);
	LoadRows(state, state.base);

	if (entry != nullptr) {
		gtk_window_set_title(GTK_WINDOW(self), _("Edit game config"));
		for (AdwViewStackPage* page:
		     {self->folders_page, self->controller_page, self->updates_page, self->about_page}) {
			adw_view_stack_page_set_visible(page, FALSE);
		}
		gtk_widget_set_visible(GTK_WIDGET(self->game_configs_button), FALSE);
		const std::string title_id = core::NormalizeTitleId(entry->title_id);
		const std::string title    = entry->title_name.empty() ? title_id : entry->title_name;
		gtk_label_set_text(self->game_title_label,
		                   title_id.empty() || title == title_id
		                       ? title.c_str()
		                       : Format(_("%s (%s)"), title.c_str(), title_id.c_str()).c_str());
		gtk_label_set_text(self->game_note_label,
		                   core::HasCustomSettings(settings, *entry)
		                       ? _("This game has its own settings.")
		                       : _("This game uses the global settings. Saving gives it its own "
		                           "settings."));
		gtk_widget_set_visible(self->game_info, TRUE);
		gtk_widget_set_tooltip_text(GTK_WIDGET(self->reset_button),
		                            _("Set every field to the global settings"));
	} else {
		gtk_window_set_title(GTK_WINDOW(self), _("Global settings"));
		gtk_widget_set_tooltip_text(GTK_WIDGET(self->reset_button), _("Restore default settings"));
		BuildFoldersPage(self);
		LoadFolders(state, settings);
		BuildControllerPage(self);
		LoadController(state, settings.controller);
		BuildUpdatesPage(self, settings);
		BuildAboutPage(self);
		AddGameConfigActions(self);
	}
	state.signals.push_back(
	    SignalScope::Connect(self->save_button, "clicked", G_CALLBACK(OnSave), self));
	state.signals.push_back(
	    SignalScope::Connect(self->reset_button, "clicked", G_CALLBACK(OnReset), self));
	return self;
}

} // namespace

static void kyty_settings_window_init(KytySettingsWindow* self) {
	self->state = new kyty::gtk::SettingsWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_settings_window_dispose(GObject* object) {
	auto* self = KYTY_SETTINGS_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		g_cancellable_cancel(state->cancellable.get());
		state->signals.clear();
		if (state->context != nullptr) {
			// The size stays in the context's settings; the next write stores it (9.14: closing
			// writes nothing).
			if (const auto geometry = CurrentGeometry(self)) {
				state->context->settings.loaded.settings.settings_window = *geometry;
			}
			// A previewed color gives way to the saved one.
			if (state->previewed) {
				state->context->services.controller.SetLightbarColor(
				    LightbarOf(state->context->settings.loaded.settings.controller.color));
			}
		}
		gtk_widget_insert_action_group(GTK_WIDGET(self), "settings", nullptr);
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_SETTINGS_WINDOW);
	G_OBJECT_CLASS(kyty_settings_window_parent_class)->dispose(object);
}

static void kyty_settings_window_finalize(GObject* object) {
	auto* self = KYTY_SETTINGS_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_settings_window_parent_class)->finalize(object);
}

static void kyty_settings_window_class_init(KytySettingsWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_settings_window_dispose;
	object_class->finalize = kyty_settings_window_finalize;
	g_saved_signal = g_signal_new("saved", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
	                              nullptr, nullptr, G_TYPE_NONE, 0);
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, stack);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, game_info);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, game_title_label);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, game_note_label);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, folders_page);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, controller_page);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, updates_page);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, about_page);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, user_profile_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, graphics_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, audio_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, notifications_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, general_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, compatibility_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, network_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, debugging_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, rendering_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, game_folders_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, addons_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, controller_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, updates_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, about_group);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, reset_button);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, game_configs_button);
	gtk_widget_class_bind_template_child(widget_class, KytySettingsWindow, save_button);
	// Escape is Cancel (closing writes nothing).
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytySettingsWindow* kyty_settings_window_new_global(kyty::gtk::LauncherContext& context,
                                                    GtkWindow*                  parent) {
	return NewWindow(context, parent, nullptr);
}

KytySettingsWindow* kyty_settings_window_new_for_game(kyty::gtk::LauncherContext&  context,
                                                      GtkWindow*                   parent,
                                                      const kyty::core::GameEntry& entry) {
	return NewWindow(context, parent, &entry);
}

void kyty_settings_window_show_page(KytySettingsWindow* window, const char* name) {
	g_return_if_fail(KYTY_IS_SETTINGS_WINDOW(window));
	adw_view_stack_set_visible_child_name(window->stack, name);
}

namespace kyty::gtk {

std::string SettingsFieldErrorText(const core::ValidationError& error) {
	return FieldErrorText(error);
}

std::string GameSettingsImportErrorText(const core::ImportError& error) {
	return ImportErrorText(error);
}

} // namespace kyty::gtk
