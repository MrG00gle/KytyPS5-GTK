#pragma once

// The emulator settings of a game and of the Global_Configuration (design.md, "Game_Configuration
// fields"; Requirements 3.4, 9.1, 9.4, 9.12). Ported from the Qt launcher's Configuration and
// ControllerSettings (src/launcher/include/configuration.h, src/launcher/src/configuration.cpp).
//
// The Qt field red_zone_protection_enabled is not modeled: the --redzone option is gone
// (Requirement 3.4). In Kyty.ini a key of that name is an unknown key, kept on rewrite; in a
// Game_Settings_File it is an unknown field, rejected on import. The field
// shader_optimization_type is not modeled either: the emulator drops its option. In Kyty.ini it is
// an unknown key, kept on rewrite; a Game_Settings_File import ignores it.
//
// The fields from stretch_enabled to guest_tsc_enabled came with the upstream launcher's later
// settings, under its key names. dlss_preset is this launcher's own (the upstream launcher has no
// such setting). fsr_folder and the FSR upscaler are kept so that upstream's files load, but the
// settings editor does not offer them: FSR has no effect on Linux.
//
// configurationFields.h has the field table: the Kyty.ini and JSON key of each field, its text
// form, and its rules.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// The screen size the emulator's window gets, in pixels (the upstream launcher's free-form
// resolution). Kyty.ini and Game_Settings_File store it as "<width>x<height>".
struct ScreenResolution {
	int32_t width  = 1280;
	int32_t height = 720;

	friend bool operator==(const ScreenResolution&, const ScreenResolution&) = default;
};

// The upstream launcher's limits for each side.
inline constexpr int32_t kMinScreenSize = 320;
inline constexpr int32_t kMaxScreenSize = 16384;

// The upstream launcher's presets, in its order: 1280x720, 1920x1080, 2560x1440, 3840x2160, then
// the ultrawide 2560x1080, 3440x1440, 3840x1600, and 5120x2160.
std::span<const ScreenResolution> ScreenResolutionPresets();

// Each side kMinScreenSize-kMaxScreenSize.
bool IsValidScreenResolution(const ScreenResolution& resolution);

// "<width>x<height>" in decimal, as the upstream launcher writes it ("1280x720").
std::string FormatScreenResolution(const ScreenResolution& resolution);

// The sizes the text forms give, without the range check: the upstream launcher's form (optional
// ASCII spaces around, an optional "R", the width, "x" or "X", the height: "1920x1080",
// " 3440 X 1440 "), which also reads the old keys "R1280X720", "R1920X1080", "R2560X1440", and
// "R3840X2160"; and the integer index 0-3 of those four (the old enum's legacy form). Nothing for
// other text or a side that does not fit int32_t.
std::optional<ScreenResolution> ParseScreenResolution(std::string_view text);

// The Qt launcher's Q_ENUM types. Kyty.ini and Game_Settings_File store their key names.
enum class PresentMode { Fifo, Mailbox, Immediate };
enum class LogDirection { Silent, Console, File };
enum class PssrUpscaler { Linear, DLSS, FSR };
// Default is the DLSS model's own preset; A-O are passed as --dlss-preset <letter>.
enum class DlssPreset { Default, A, B, C, D, E, F, G, H, I, J, K, L, M, N, O };

// The emulator's limits (src/common/emulatorConfig.h).
inline constexpr int32_t kDefaultUserId = 1000;
// INT32_MAX minus 3 player steps of 0x100: the IDs of players 2-4 derive from the user ID.
inline constexpr int32_t kMaxUserId              = 2147482879;
inline constexpr size_t  kMaxUserNameBytes       = 16;
inline constexpr int32_t kDefaultConsoleLanguage = 1;
inline constexpr int32_t kMaxConsoleLanguage     = 29;
inline constexpr int32_t kMaxControllerPercent   = 100;
inline constexpr int32_t kMaxKeyboardPlayer      = 4; // the emulator's player count

// Every Game_Configuration field with the design's default. The Global_Configuration has the same
// fields; input_mapping_custom is meaningful only per game. An empty audio_input_device means no
// audio input, and gpu_index -1 means automatic.
struct GameConfiguration {
	ScreenResolution screen_resolution           = {};
	std::string      user_name                   = "Kyty";
	int32_t          user_id                     = kDefaultUserId;
	std::string      audio_input_device          = {};
	PresentMode      present_mode                = PresentMode::Mailbox;
	int32_t          gpu_index                   = -1;
	bool             fullscreen_enabled          = false;
	bool             hdr_enabled                 = false;
	bool             hide_cursor_enabled         = false;
	bool             stretch_enabled             = false;
	bool             readback_linear_images      = false;
	bool             readback_tiled_images       = false;
	bool             readback_storage_images     = false;
	bool             sync_raw_image_buffers      = false;
	int32_t          flush_draws                 = 0; // 0 is off
	bool             tessellation_enabled        = false;
	bool             occlusion_queries_enabled   = false;
	bool             relaxed_shader_float        = false;
	bool             native_rt                   = false;
	bool             shader_cache_enabled        = true;
	bool             trophy_enabled              = true;
	int32_t          trophy_duration_seconds     = 6;
	bool             perf_overlay_enabled        = false;
	bool             skip_notice_screen          = false;
	bool             network_connected           = false;
	bool             psn_emulation_enabled       = false;
	int32_t          vblank_frequency            = 60;
	int32_t          console_language            = kDefaultConsoleLanguage;
	bool             vulkan_validation_enabled   = false;
	bool             shader_validation_enabled   = false;
	LogDirection     shader_log_direction        = LogDirection::Silent;
	std::string      shader_log_folder           = "_Shaders";
	bool             command_buffer_dump_enabled = false;
	std::string      command_buffer_dump_folder  = "_Buffers";
	LogDirection     printf_direction            = LogDirection::Silent;
	std::string      printf_output_file          = "_kyty.txt";
	bool             profiler_enabled            = false;
	bool             renderdoc_enabled           = false;
	bool             pro_enabled                 = false;
	PssrUpscaler     pssr_upscaler               = PssrUpscaler::Linear;
	std::string      dlss_folder                 = {};
	DlssPreset       dlss_preset                 = DlssPreset::Default;
	std::string      fsr_folder                  = {};
	bool             amd_cpu_enabled             = false;
	bool             guest_tsc_enabled           = false;
	std::string      elf                         = "eboot.bin";
	// Per game: true when host_input_mapping replaces the global mapping (Requirement 11.14).
	bool input_mapping_custom = false;
	// Entries "Control=Input", one --keymap option each.
	std::vector<std::string> host_input_mapping;

	friend bool operator==(const GameConfiguration&, const GameConfiguration&) = default;
};

// The global-only controller fields ([GlobalConfiguration] in Kyty.ini), always taken from the
// Global_Configuration.
struct ControllerSettings {
	std::string color;                     // "" (no override) or "#RRGGBB"
	int32_t     speaker_volume      = 50;  // 0-100
	int32_t     vibration_intensity = 100; // 0-100
	// 0 shares player 1 with the first gamepad; 1-4 dedicates that player to the keyboard.
	int32_t keyboard_player = 0;

	friend bool operator==(const ControllerSettings&, const ControllerSettings&) = default;
};

struct InvalidMappingEntry {
	std::size_t index = 0; // the 0-based position in the mapping
	std::string entry;     // the entry as stored

	friend bool operator==(const InvalidMappingEntry&, const InvalidMappingEntry&) = default;
};

// Which rule a value breaks (plan V1: the GTK layer has its own text per rule and field).
enum class ValidationRule {
	Field,                 // the field's own rule (FieldDescriptor::check): its range or form
	MissingOutputPath,     // an output path is empty while its output is on (IsMissingOutputPath)
	InvalidMappingEntries, // host_input_mapping entries that are not IsValidMappingEntry
};

struct ValidationError {
	std::string field;   // the Kyty.ini and JSON key of the invalid field
	std::string message; // what is wrong, in English, without the key
	ValidationRule                   rule = ValidationRule::Field;
	std::vector<InvalidMappingEntry> entries; // InvalidMappingEntries: every invalid entry

	friend bool operator==(const ValidationError&, const ValidationError&) = default;
};

// Which configuration Validate() checks (plan D26). Global: the Global_Configuration, whose
// host_input_mapping is always used. Game: a game's custom configuration (SaveCustomSettings and
// the Game_Settings_File import), whose own host_input_mapping is used only while
// input_mapping_custom is true, so its entries are checked only then. Stores written by the Qt
// launcher hold per-game entries without '=' (tests/fixtures/settings/qt6-special-values.ini);
// they stay as they are while the game uses the global mapping.
enum class ValidationScope { Global, Game };

// The first field, in GameConfigurationFields() order, whose value the settings editor and an
// import must reject, or nothing when every value is valid. The rules: the Requirement 9.4 rules
// (user name 1-16 UTF-8 bytes after TrimWhitespace; user ID 0-2,147,482,879 except 254 and 255;
// vblank frequency 30-360; trophy duration 5-10; shader_log_folder, printf_output_file, and
// command_buffer_dump_folder not empty while shader logs or printf output go to a File or command
// buffer dumps are enabled); IsValidScreenResolution; gpu_index >= -1; flush_draws >= 0;
// console_language 0-29; enum values that name a key;
// elf a non-empty relative path without a ".." component; and each host_input_mapping entry
// IsValidMappingEntry (in Game scope only while input_mapping_custom is true). The
// host_input_mapping message lists every invalid entry, numbered from 1, with its quoted text.
std::optional<ValidationError> Validate(const GameConfiguration& configuration,
                                        ValidationScope          scope = ValidationScope::Global);

// True for an input mapping entry "Name=Value" with both sides non-empty after TrimWhitespace
// (split at the first '='). Only valid entries become --keymap options.
bool IsValidMappingEntry(std::string_view entry);

// Every entry of mapping that is not IsValidMappingEntry, in order: what the settings editor lists
// so that the user can remove it.
std::vector<InvalidMappingEntry> FindInvalidMappingEntries(std::span<const std::string> mapping);

// The valid entries of mapping, in order: the mapping with every invalid entry removed.
std::vector<std::string> DropInvalidMappingEntries(std::span<const std::string> mapping);

// True when field is shader_log_folder, command_buffer_dump_folder, or printf_output_file, its
// value in configuration is empty, and its output is on (shader logs or printf output go to a File,
// command buffer dumps are enabled): the Requirement 9.4 rule that needs two fields.
bool IsMissingOutputPath(std::string_view field, const GameConfiguration& configuration);

// The controller values clamped as Requirement 9.12 says: speaker volume and vibration intensity
// to 0-100, the keyboard player to 0-4; in-range values stay. The color is "".
ControllerSettings ClampControllerSettings(int64_t volume, int64_t vibration,
                                           int64_t keyboard_player);

// True for "" and "#" followed by 6 hexadecimal digits of either letter case.
bool IsValidControllerColor(std::string_view color);

} // namespace kyty::core
