#pragma once

// The emulator settings of a game and of the Global_Configuration (design.md, "Game_Configuration
// fields"; Requirements 3.4, 9.1, 9.4, 9.12). Ported from the Qt launcher's Configuration and
// ControllerSettings (src/launcher/include/configuration.h, src/launcher/src/configuration.cpp).
//
// The Qt field red_zone_protection_enabled is not modeled: the --redzone option is gone
// (Requirement 3.4). In Kyty.ini a key of that name is an unknown key, kept on rewrite; in a
// Game_Settings_File it is an unknown field, rejected on import.
//
// configurationFields.h has the field table: the Kyty.ini and JSON key of each field, its text
// form, and its rules.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// The Qt launcher's Q_ENUM types. Kyty.ini and Game_Settings_File store their key names.
enum class ScreenResolution { R1280X720, R1920X1080, R2560X1440, R3840X2160 };
enum class PresentMode { Fifo, Mailbox, Immediate };
enum class ShaderOptimizationType { None, Size, Performance };
enum class LogDirection { Silent, Console, File };

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
	ScreenResolution       screen_resolution           = ScreenResolution::R1280X720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = kDefaultUserId;
	std::string            audio_input_device          = {};
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   readback_linear_images      = false;
	bool                   readback_tiled_images       = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   trophy_enabled              = true;
	int32_t                trophy_duration_seconds     = 6;
	bool                   perf_overlay_enabled        = false;
	bool                   skip_notice_screen          = false;
	bool                   network_connected           = false;
	int32_t                vblank_frequency            = 60;
	int32_t                console_language            = kDefaultConsoleLanguage;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::Performance;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::string            shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::string            command_buffer_dump_folder  = "_Buffers";
	LogDirection           printf_direction            = LogDirection::Silent;
	std::string            printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	bool                   renderdoc_enabled           = false;
	bool                   amd_cpu_enabled             = false;
	std::string            elf                         = "eboot.bin";
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

struct ValidationError {
	std::string field;   // the Kyty.ini and JSON key of the invalid field
	std::string message; // what is wrong, in English, without the key

	friend bool operator==(const ValidationError&, const ValidationError&) = default;
};

// The first field, in GameConfigurationFields() order, whose value the settings editor and an
// import must reject, or nothing when every value is valid. The rules: the Requirement 9.4 rules
// (user name 1-16 UTF-8 bytes after TrimWhitespace; user ID 0-2,147,482,879 except 254 and 255;
// vblank frequency 30-360; trophy duration 5-10; shader_log_folder, printf_output_file, and
// command_buffer_dump_folder not empty while shader logs or printf output go to a File or command
// buffer dumps are enabled); gpu_index >= -1; console_language 0-29; enum values that name a key;
// elf a non-empty relative path without a ".." component; and each host_input_mapping entry
// "Name=Value" with both sides non-empty after TrimWhitespace.
std::optional<ValidationError> Validate(const GameConfiguration& configuration);

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
