// Example tests of the Game_Configuration fields, their table, and their rules
// (src/launcher/core/gameConfiguration.h, configurationFields.h; task 13.2, Requirements 3.4, 9.1,
// 9.4, 9.12): the design defaults, every enum spelling including the legacy forms, the text forms
// of booleans and integers, every Requirement 9.4 boundary, the gpu_index, console_language, elf,
// and input mapping rules, the order Validate() reports in, and ClampControllerSettings at and
// beyond its limits.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "support/noDisplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using kyty::core::ClampControllerSettings;
using kyty::core::ControllerSettings;
using kyty::core::FieldDescriptor;
using kyty::core::FieldKind;
using kyty::core::FieldValue;
using kyty::core::FindGameConfigurationField;
using kyty::core::GameConfiguration;
using kyty::core::GameConfigurationFields;
using kyty::core::GlobalOnlyFields;
using kyty::core::LogDirection;
using kyty::core::PresentMode;
using kyty::core::ScreenResolution;
using kyty::core::ShaderOptimizationType;
using kyty::core::Validate;

constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();
constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int32_t kInt32Min = std::numeric_limits<int32_t>::min();
constexpr int32_t kInt32Max = std::numeric_limits<int32_t>::max();

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameConfigurationTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const FieldDescriptor& Field(std::string_view name) {
	const auto* field = FindGameConfigurationField(name);
	Check(field != nullptr, "no field " + std::string(name));
	return *field;
}

// The field that Validate() names, or "" when the configuration is valid.
std::string Invalid(const GameConfiguration& configuration) {
	const auto error = Validate(configuration);
	if (!error) {
		return {};
	}
	Check(!error->field.empty() && !error->message.empty(), "an error has a field and a message");
	return error->field;
}

void CheckValid(const GameConfiguration& configuration, const std::string& what) {
	const auto error = Validate(configuration);
	Check(!error, what + " is valid, got " + (error ? error->field + ": " + error->message : ""));
}

void CheckInvalid(const GameConfiguration& configuration, const std::string& field,
                  const std::string& what) {
	Check(Invalid(configuration) == field,
	      what + " is invalid in " + field + ", got '" + Invalid(configuration) + "'");
}

// ---- Defaults and the table

void TestDefaults() {
	const GameConfiguration c;
	Check(c.screen_resolution == ScreenResolution::R1280X720, "screen_resolution default");
	Check(c.user_name == "Kyty", "user_name default");
	Check(c.user_id == 1000, "user_id default");
	Check(c.audio_input_device.empty(), "audio_input_device default");
	Check(c.present_mode == PresentMode::Mailbox, "present_mode default");
	Check(c.gpu_index == -1, "gpu_index default");
	Check(!c.fullscreen_enabled && !c.hdr_enabled && !c.hide_cursor_enabled &&
	          !c.readback_linear_images && !c.readback_tiled_images && !c.sync_raw_image_buffers &&
	          !c.tessellation_enabled && !c.perf_overlay_enabled && !c.skip_notice_screen &&
	          !c.network_connected && !c.vulkan_validation_enabled &&
	          !c.shader_validation_enabled && !c.command_buffer_dump_enabled &&
	          !c.profiler_enabled && !c.renderdoc_enabled && !c.amd_cpu_enabled &&
	          !c.input_mapping_custom,
	      "boolean defaults are false");
	Check(c.trophy_enabled, "trophy_enabled default");
	Check(c.trophy_duration_seconds == 6, "trophy_duration_seconds default");
	Check(c.vblank_frequency == 60, "vblank_frequency default");
	Check(c.console_language == 1, "console_language default");
	Check(c.shader_optimization_type == ShaderOptimizationType::Performance,
	      "shader_optimization_type default");
	Check(c.shader_log_direction == LogDirection::Silent, "shader_log_direction default");
	Check(c.shader_log_folder == "_Shaders", "shader_log_folder default");
	Check(c.command_buffer_dump_folder == "_Buffers", "command_buffer_dump_folder default");
	Check(c.printf_direction == LogDirection::Silent, "printf_direction default");
	Check(c.printf_output_file == "_kyty.txt", "printf_output_file default");
	Check(c.elf == "eboot.bin", "elf default");
	Check(c.host_input_mapping.empty(), "host_input_mapping default");
	CheckValid(c, "the default configuration");

	const ControllerSettings controller;
	Check(controller.color.empty() && controller.speaker_volume == 50 &&
	          controller.vibration_intensity == 100 && controller.keyboard_player == 0,
	      "controller defaults");
}

void TestTable() {
	const std::vector<std::string_view> names  = {"screen_resolution",
	                                              "user_name",
	                                              "user_id",
	                                              "audio_input_device",
	                                              "present_mode",
	                                              "gpu_index",
	                                              "fullscreen_enabled",
	                                              "hdr_enabled",
	                                              "hide_cursor_enabled",
	                                              "readback_linear_images",
	                                              "readback_tiled_images",
	                                              "sync_raw_image_buffers",
	                                              "tessellation_enabled",
	                                              "trophy_enabled",
	                                              "trophy_duration_seconds",
	                                              "perf_overlay_enabled",
	                                              "skip_notice_screen",
	                                              "network_connected",
	                                              "vblank_frequency",
	                                              "console_language",
	                                              "vulkan_validation_enabled",
	                                              "shader_validation_enabled",
	                                              "shader_optimization_type",
	                                              "shader_log_direction",
	                                              "shader_log_folder",
	                                              "command_buffer_dump_enabled",
	                                              "command_buffer_dump_folder",
	                                              "printf_direction",
	                                              "printf_output_file",
	                                              "profiler_enabled",
	                                              "renderdoc_enabled",
	                                              "amd_cpu_enabled",
	                                              "elf",
	                                              "input_mapping_custom",
	                                              "host_input_mapping"};
	const auto                          fields = GameConfigurationFields();
	Check(fields.size() == names.size(), "35 Game_Configuration fields");
	for (size_t i = 0; i < names.size(); i++) {
		Check(fields[i].name == names[i], "field " + std::to_string(i) + " is " +
		                                      std::string(names[i]) + ", got " +
		                                      std::string(fields[i].name));
		Check(FindGameConfigurationField(names[i]) == &fields[i], "find " + std::string(names[i]));
		Check(fields[i].parse != nullptr && fields[i].format != nullptr &&
		          fields[i].get != nullptr && fields[i].set != nullptr &&
		          fields[i].check != nullptr,
		      "every function of " + std::string(names[i]));
	}
	Check(FindGameConfigurationField("red_zone_protection_enabled") == nullptr,
	      "red_zone_protection_enabled is not a field (Requirement 3.4)");
	Check(FindGameConfigurationField("name") == nullptr &&
	          FindGameConfigurationField("game_path") == nullptr &&
	          FindGameConfigurationField("controller_color") == nullptr &&
	          FindGameConfigurationField("") == nullptr,
	      "entry keys and global-only fields are not Game_Configuration fields");

	Check(Field("screen_resolution").kind == FieldKind::Enum, "enum kind");
	Check(Field("user_name").kind == FieldKind::String, "string kind");
	Check(Field("user_id").kind == FieldKind::Int, "int kind");
	Check(Field("hdr_enabled").kind == FieldKind::Bool, "bool kind");
	Check(Field("host_input_mapping").kind == FieldKind::StringList, "list kind");
	Check(Field("screen_resolution").enum_keys.size() == 4 &&
	          Field("present_mode").enum_keys.size() == 3 && Field("user_id").enum_keys.empty(),
	      "enum keys");

	const auto global = GlobalOnlyFields();
	Check(global.size() == 4 && global[0].name == "controller_color" &&
	          global[1].name == "controller_speaker_volume" &&
	          global[2].name == "controller_vibration_intensity" &&
	          global[3].name == "controller_keyboard_player",
	      "global-only fields");
	Check(global[0].kind == FieldKind::String && global[1].kind == FieldKind::Int,
	      "global-only kinds");
}

// Every field: format(get) parses back to the same value and set() stores it.
void TestFieldRoundTrip() {
	GameConfiguration c;
	c.screen_resolution           = ScreenResolution::R3840X2160;
	c.user_name                   = "Player, \"two\"";
	c.user_id                     = 0;
	c.audio_input_device          = "USB Audio";
	c.present_mode                = PresentMode::Immediate;
	c.gpu_index                   = 3;
	c.fullscreen_enabled          = true;
	c.trophy_enabled              = false;
	c.trophy_duration_seconds     = 10;
	c.vblank_frequency            = 360;
	c.console_language            = 29;
	c.shader_optimization_type    = ShaderOptimizationType::None;
	c.shader_log_direction        = LogDirection::File;
	c.shader_log_folder           = "logs/shaders";
	c.command_buffer_dump_enabled = true;
	c.printf_direction            = LogDirection::Console;
	c.elf                         = "bin/eboot_alt.bin";
	c.input_mapping_custom        = true;
	c.host_input_mapping          = {"Cross=J", "L3=Left Shift"};
	CheckValid(c, "the round-trip configuration");

	GameConfiguration copy;
	for (const auto& field: GameConfigurationFields()) {
		const auto value = field.get(c);
		Check(!field.check(value), std::string(field.name) + " passes its check");
		if (field.kind == FieldKind::StringList) {
			Check(!field.parse("Cross=J").has_value() && field.format(value).empty(),
			      "a string list has no text form");
			Check(field.set(copy, value), "set " + std::string(field.name));
			continue;
		}
		const auto text   = field.format(value);
		const auto parsed = field.parse(text);
		Check(parsed.has_value() && *parsed == value,
		      std::string(field.name) + " parses its text form '" + text + "'");
		Check(field.set(copy, *parsed), "set " + std::string(field.name));
	}
	Check(copy == c, "every field copied through its text form");

	// set() refuses another alternative and values that do not fit, leaving the target unchanged.
	GameConfiguration target = c;
	Check(!Field("hdr_enabled").set(target, FieldValue(int64_t {1})), "bool from an int");
	Check(!Field("user_id").set(target, FieldValue(std::string("1"))), "int from a string");
	Check(!Field("user_id").set(target, FieldValue(int64_t {kInt32Max} + 1)), "int above int32");
	Check(!Field("gpu_index").set(target, FieldValue(int64_t {kInt32Min} - 1)), "int below int32");
	Check(!Field("present_mode").set(target, FieldValue(int64_t {3})), "enum index 3");
	Check(!Field("present_mode").set(target, FieldValue(int64_t {-1})), "enum index -1");
	Check(!Field("user_name").set(target, FieldValue(true)), "string from a bool");
	Check(!Field("host_input_mapping").set(target, FieldValue(std::string("x=y"))),
	      "list from a string");
	Check(target == c, "refused set() calls change nothing");
	Check(Field("hdr_enabled").check(FieldValue(std::string("true"))).has_value() &&
	          Field("user_name").check(FieldValue(int64_t {1})).has_value() &&
	          Field("host_input_mapping").check(FieldValue(false)).has_value(),
	      "check() rejects another alternative");
	Check(Field("hdr_enabled").format(FieldValue(int64_t {1})).empty() &&
	          Field("user_id").format(FieldValue(true)).empty() &&
	          Field("present_mode").format(FieldValue(int64_t {7})).empty() &&
	          Field("user_name").format(FieldValue(false)).empty(),
	      "format() of another alternative is empty");
}

// ---- Text forms

void TestEnumSpellings() {
	struct Case {
		std::string_view       field;
		std::string_view       text;
		std::optional<int64_t> index;
	};
	const std::vector<Case> cases = {
	    {"screen_resolution", "R1280X720", 0},
	    {"screen_resolution", "R1920X1080", 1},
	    {"screen_resolution", "R2560X1440", 2},
	    {"screen_resolution", "R3840X2160", 3},
	    // Legacy text (Qt's TextToEnum) and integer indexes.
	    {"screen_resolution", "1280x720", 0},
	    {"screen_resolution", "1920x1080", 1},
	    {"screen_resolution", "2560x1440", 2},
	    {"screen_resolution", "3840x2160", 3},
	    {"screen_resolution", "1920X1080", 1},
	    {"screen_resolution", "0", 0},
	    {"screen_resolution", "3", 3},
	    {"screen_resolution", "003", 3},
	    {"screen_resolution", "4", std::nullopt},
	    {"screen_resolution", "-1", std::nullopt},
	    {"screen_resolution", "R1280x720", std::nullopt},
	    {"screen_resolution", "r1280x720", std::nullopt},
	    {"screen_resolution", "1280x721", std::nullopt},
	    {"screen_resolution", "1280", std::nullopt},
	    {"screen_resolution", "R1280X720 ", std::nullopt},
	    {"screen_resolution", "", std::nullopt},
	    {"present_mode", "Fifo", 0},
	    {"present_mode", "Mailbox", 1},
	    {"present_mode", "Immediate", 2},
	    {"present_mode", "2", 2},
	    {"present_mode", "3", std::nullopt},
	    {"present_mode", "mailbox", std::nullopt},
	    {"present_mode", "MAILBOX", std::nullopt},
	    {"shader_optimization_type", "None", 0},
	    {"shader_optimization_type", "Size", 1},
	    {"shader_optimization_type", "Performance", 2},
	    {"shader_optimization_type", "1", 1},
	    {"shader_optimization_type", "Speed", std::nullopt},
	    {"shader_log_direction", "Silent", 0},
	    {"shader_log_direction", "Console", 1},
	    {"shader_log_direction", "File", 2},
	    {"shader_log_direction", "0", 0},
	    {"shader_log_direction", "file", std::nullopt},
	    {"printf_direction", "File", 2},
	    {"printf_direction", "Off", std::nullopt},
	};
	for (const auto& c: cases) {
		const auto parsed = Field(c.field).parse(c.text);
		const auto what   = std::string(c.field) + " '" + std::string(c.text) + "'";
		if (c.index) {
			Check(parsed.has_value() && *parsed == FieldValue(*c.index), what + " parses");
		} else {
			Check(!parsed.has_value(), what + " is rejected");
		}
	}
	Check(Field("screen_resolution").format(FieldValue(int64_t {1})) == "R1920X1080" &&
	          Field("present_mode").format(FieldValue(int64_t {0})) == "Fifo" &&
	          Field("shader_optimization_type").format(FieldValue(int64_t {1})) == "Size" &&
	          Field("printf_direction").format(FieldValue(int64_t {2})) == "File",
	      "enums are written as their key names");
}

void TestBoolAndIntText() {
	for (const auto& [text, value]:
	     std::vector<std::pair<std::string_view, std::optional<bool>>> {{"true", true},
	                                                                    {"false", false},
	                                                                    {"TRUE", std::nullopt},
	                                                                    {"True", std::nullopt},
	                                                                    {"1", std::nullopt},
	                                                                    {"0", std::nullopt},
	                                                                    {"yes", std::nullopt},
	                                                                    {"", std::nullopt},
	                                                                    {" true", std::nullopt}}) {
		Check(kyty::core::ParseBoolText(text) == value, "bool text '" + std::string(text) + "'");
		Check(Field("hdr_enabled").parse(text).has_value() == value.has_value(),
		      "bool field text '" + std::string(text) + "'");
	}
	Check(kyty::core::FormatBoolText(true) == "true" &&
	          kyty::core::FormatBoolText(false) == "false",
	      "bool format");
	for (const auto& [text, value]:
	     std::vector<std::pair<std::string_view, std::optional<int64_t>>> {
	         {"0", 0},
	         {"-1", -1},
	         {"1000", 1000},
	         {"007", 7},
	         {"2147483648", 2147483648},
	         {"9223372036854775807", kInt64Max},
	         {"-9223372036854775808", kInt64Min},
	         {"9223372036854775808", std::nullopt},
	         {"+1", std::nullopt},
	         {" 1", std::nullopt},
	         {"1 ", std::nullopt},
	         {"1.0", std::nullopt},
	         {"0x10", std::nullopt},
	         {"1e3", std::nullopt},
	         {"abc", std::nullopt},
	         {"-", std::nullopt},
	         {"", std::nullopt}}) {
		Check(kyty::core::ParseIntText(text) == value, "int text '" + std::string(text) + "'");
	}
	Check(Field("user_id").format(FieldValue(int64_t {-5})) == "-5", "int format");
	Check(Field("user_name").parse("@Kyty, \"=;\\") == FieldValue(std::string("@Kyty, \"=;\\")),
	      "a string is its own text form");
}

// ---- Requirement 9.4 and the other rules

void TestUserName() {
	GameConfiguration c;
	const std::string e_acute = "\xC3\xA9"; // 2 bytes
	std::string       sixteen;
	for (int i = 0; i < 8; i++) {
		sixteen += e_acute;
	}
	const std::vector<std::pair<std::string, bool>> cases = {
	    {"", false},
	    {" ", false},
	    {"\t\n ", false},
	    {"\xE3\x80\x80", false},                       // U+3000 only: empty after trimming
	    {"a", true},                                   // 1 byte
	    {"\xC3\xA9", true},                            // 2 bytes
	    {"\xF0\x9F\x8E\xAE", true},                    // 4 bytes, one character
	    {sixteen, true},                               // 16 bytes
	    {sixteen + "a", false},                        // 17 bytes
	    {"a" + sixteen, false},                        // 17 bytes
	    {"0123456789abcdef", true},                    // 16 ASCII bytes
	    {"0123456789abcdefg", false},                  // 17 ASCII bytes
	    {"  " + sixteen + " \t", true},                // 16 bytes after trimming, 19 before
	    {"\xE3\x80\x80" + sixteen + "\xC2\xA0", true}, // Unicode spaces trimmed
	    {" Kyty ", true},
	    {"a b", true},
	};
	for (const auto& [name, valid]: cases) {
		c.user_name     = name;
		const auto what = "user name of " + std::to_string(name.size()) + " bytes '" + name + "'";
		if (valid) {
			CheckValid(c, what);
		} else {
			CheckInvalid(c, "user_name", what);
		}
	}
}

void TestIntegerBoundaries() {
	struct Case {
		std::string_view field;
		int64_t          value;
		bool             valid;
	};
	const std::vector<Case> cases = {
	    {"user_id", -1, false},
	    {"user_id", 0, true},
	    {"user_id", 1, true},
	    {"user_id", 253, true},
	    {"user_id", 254, false},
	    {"user_id", 255, false},
	    {"user_id", 256, true},
	    {"user_id", 2147482879, true},
	    {"user_id", 2147482880, false},
	    {"user_id", kInt32Max, false},
	    {"user_id", kInt32Min, false},
	    {"vblank_frequency", 29, false},
	    {"vblank_frequency", 30, true},
	    {"vblank_frequency", 360, true},
	    {"vblank_frequency", 361, false},
	    {"vblank_frequency", 0, false},
	    {"trophy_duration_seconds", 4, false},
	    {"trophy_duration_seconds", 5, true},
	    {"trophy_duration_seconds", 10, true},
	    {"trophy_duration_seconds", 11, false},
	    {"gpu_index", -2, false},
	    {"gpu_index", -1, true},
	    {"gpu_index", 0, true},
	    {"gpu_index", kInt32Max, true},
	    {"gpu_index", kInt32Min, false},
	    {"console_language", -1, false},
	    {"console_language", 0, true},
	    {"console_language", 29, true},
	    {"console_language", 30, false},
	};
	for (const auto& c: cases) {
		GameConfiguration configuration;
		Check(Field(c.field).set(configuration, FieldValue(c.value)),
		      "set " + std::string(c.field));
		Check(Field(c.field).check(FieldValue(c.value)).has_value() != c.valid,
		      "check of " + std::string(c.field) + " " + std::to_string(c.value));
		const auto what = std::string(c.field) + " " + std::to_string(c.value);
		if (c.valid) {
			CheckValid(configuration, what);
		} else {
			CheckInvalid(configuration, std::string(c.field), what);
		}
	}
	// Beyond int32: the field cannot hold it and check() rejects it.
	Check(Field("gpu_index").check(FieldValue(int64_t {kInt32Max} + 1)).has_value(),
	      "gpu_index above int32");
	Check(Field("user_id").check(FieldValue(kInt64Max)).has_value(), "user_id int64 max");
}

void TestOutputPaths() {
	struct Path {
		std::string_view field;
		void (*turn_on)(GameConfiguration&, bool on);
		void (*set_path)(GameConfiguration&, std::string value);
	};
	const std::vector<Path> paths = {
	    {"shader_log_folder",
	     [](GameConfiguration& c, bool on) {
		     c.shader_log_direction = on ? LogDirection::File : LogDirection::Console;
	     },
	     [](GameConfiguration& c, std::string value) { c.shader_log_folder = std::move(value); }},
	    {"command_buffer_dump_folder",
	     [](GameConfiguration& c, bool on) { c.command_buffer_dump_enabled = on; },
	     [](GameConfiguration& c, std::string value) {
		     c.command_buffer_dump_folder = std::move(value);
	     }},
	    {"printf_output_file",
	     [](GameConfiguration& c, bool on) {
		     c.printf_direction = on ? LogDirection::File : LogDirection::Silent;
	     },
	     [](GameConfiguration& c, std::string value) { c.printf_output_file = std::move(value); }},
	};
	for (const auto& path: paths) {
		const auto name = std::string(path.field);
		for (const bool on: {false, true}) {
			for (const auto* value: {"", " ", "x", "logs/out.txt"}) {
				GameConfiguration c;
				path.turn_on(c, on);
				path.set_path(c, value);
				const bool expect_valid = !on || std::string_view(value) != "";
				Check(kyty::core::IsMissingOutputPath(name, c) == !expect_valid,
				      "IsMissingOutputPath " + name);
				const auto what = name + " '" + value + "' with its output " + (on ? "on" : "off");
				if (expect_valid) {
					CheckValid(c, what);
				} else {
					CheckInvalid(c, name, what);
				}
			}
		}
	}
	Check(!kyty::core::IsMissingOutputPath("elf", GameConfiguration {}), "other fields");
}

void TestElf() {
	const std::vector<std::pair<std::string, bool>> cases = {
	    {"eboot.bin", true},
	    {"bin/eboot_alt.bin", true},
	    {"./eboot.bin", true},
	    {"a..b", true},
	    {"..a/b", true},
	    {"dir/.../x", true},
	    {"", false},
	    {"/eboot.bin", false},
	    {"/", false},
	    {"..", false},
	    {"../eboot.bin", false},
	    {"bin/../eboot.bin", false},
	    {"bin/..", false},
	    {"a//../b", false},
	};
	for (const auto& [elf, valid]: cases) {
		GameConfiguration c;
		c.elf = elf;
		if (valid) {
			CheckValid(c, "elf '" + elf + "'");
		} else {
			CheckInvalid(c, "elf", "elf '" + elf + "'");
		}
	}
}

void TestInputMapping() {
	const std::vector<std::pair<std::vector<std::string>, bool>> cases = {
	    {{}, true},
	    {{"Cross=J"}, true},
	    {{"Cross=J", "L3=Left Shift", "Pad:Cross=South"}, true},
	    {{"Triangle=="}, true},
	    {{"Square=,"}, true},
	    {{" Cross = J "}, true},
	    {{"Cross"}, false},
	    {{"=J"}, false},
	    {{"Cross="}, false},
	    {{" =J"}, false},
	    {{"Cross= \t"}, false},
	    {{""}, false},
	    {{"Cross=J", "bad"}, false},
	};
	for (const auto& [mapping, valid]: cases) {
		GameConfiguration c;
		c.host_input_mapping = mapping;
		std::string shown;
		for (const auto& entry: mapping) {
			shown += "[" + entry + "]";
		}
		if (valid) {
			CheckValid(c, "mapping " + shown);
		} else {
			CheckInvalid(c, "host_input_mapping", "mapping " + shown);
		}
	}
}

void TestEnumsOutOfRange() {
	GameConfiguration c;
	c.present_mode = static_cast<PresentMode>(3);
	CheckInvalid(c, "present_mode", "a present mode without a key");
	Check(Validate(c)->message.find("Fifo, Mailbox, or Immediate") != std::string::npos,
	      "the message lists the keys");
	c                   = {};
	c.screen_resolution = static_cast<ScreenResolution>(-1);
	CheckInvalid(c, "screen_resolution", "a resolution without a key");
}

// Validate() names the first invalid field in table order.
void TestValidateOrder() {
	GameConfiguration c;
	c.user_name        = "";
	c.vblank_frequency = 10;
	c.elf              = "/x";
	CheckInvalid(c, "user_name", "user_name before vblank_frequency and elf");
	c.user_name = "Kyty";
	CheckInvalid(c, "vblank_frequency", "vblank_frequency before elf");
	c.vblank_frequency = 60;
	CheckInvalid(c, "elf", "elf");
	c.elf                  = "eboot.bin";
	c.printf_direction     = LogDirection::File;
	c.printf_output_file   = "";
	c.host_input_mapping   = {"bad"};
	c.shader_log_direction = LogDirection::File;
	c.shader_log_folder    = "";
	CheckInvalid(c, "shader_log_folder", "shader_log_folder before printf_output_file");
	c.shader_log_folder = "s";
	CheckInvalid(c, "printf_output_file", "printf_output_file before host_input_mapping");
	c.printf_output_file = "p";
	CheckInvalid(c, "host_input_mapping", "host_input_mapping");
	c.host_input_mapping = {"a=b"};
	CheckValid(c, "all fixed");
}

// ---- Controller settings (Requirement 9.12)

void TestClampControllerSettings() {
	struct Case {
		int64_t in;
		int32_t percent;
		int32_t player;
	};
	const std::vector<Case> cases = {
	    {kInt64Min, 0, 0}, {-1, 0, 0},     {0, 0, 0},           {1, 1, 1},   {3, 3, 3},
	    {4, 4, 4},         {5, 5, 4},      {50, 50, 4},         {99, 99, 4}, {100, 100, 4},
	    {101, 100, 4},     {1000, 100, 4}, {kInt64Max, 100, 4},
	};
	for (const auto& c: cases) {
		const auto clamped = ClampControllerSettings(c.in, c.in, c.in);
		Check(clamped.speaker_volume == c.percent && clamped.vibration_intensity == c.percent &&
		          clamped.keyboard_player == c.player && clamped.color.empty(),
		      "clamp " + std::to_string(c.in));
	}
	const auto mixed = ClampControllerSettings(75, -3, 2);
	Check(mixed.speaker_volume == 75 && mixed.vibration_intensity == 0 &&
	          mixed.keyboard_player == 2,
	      "each value is clamped on its own");

	// The global-only descriptors clamp as well.
	ControllerSettings settings;
	const auto         global = GlobalOnlyFields();
	Check(global[1].set(settings, FieldValue(int64_t {150})) && settings.speaker_volume == 100,
	      "volume field clamps");
	Check(global[2].set(settings, FieldValue(kInt64Min)) && settings.vibration_intensity == 0,
	      "vibration field clamps");
	Check(global[3].set(settings, FieldValue(int64_t {9})) && settings.keyboard_player == 4,
	      "keyboard player field clamps");
	Check(!global[3].set(settings, FieldValue(std::string("1"))), "keyboard player from a string");
	Check(global[1].parse("75") == FieldValue(int64_t {75}) && !global[1].parse("x").has_value(),
	      "controller integers parse");
	Check(!global[1].check(FieldValue(kInt64Max)), "any integer passes, set() clamps it");
}

void TestControllerColor() {
	for (const auto* color: {"", "#0070d1", "#0070D1", "#FFFFFF", "#000000", "#abcdef"}) {
		Check(kyty::core::IsValidControllerColor(color), std::string("valid color ") + color);
		Check(!GlobalOnlyFields()[0].check(FieldValue(std::string(color))),
		      std::string("color field accepts ") + color);
	}
	for (const auto* color:
	     {"red", "#12345", "#1234567", "#12345g", "0070d1", "#0070d1 ", " ", "##0070d", "#fff"}) {
		Check(!kyty::core::IsValidControllerColor(color), std::string("invalid color ") + color);
		Check(GlobalOnlyFields()[0].check(FieldValue(std::string(color))).has_value(),
		      std::string("color field rejects ") + color);
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameConfigurationTests");
	TestDefaults();
	TestTable();
	TestFieldRoundTrip();
	TestEnumSpellings();
	TestBoolAndIntText();
	TestUserName();
	TestIntegerBoundaries();
	TestOutputPaths();
	TestElf();
	TestInputMapping();
	TestEnumsOutOfRange();
	TestValidateOrder();
	TestClampControllerSettings();
	TestControllerColor();
	std::puts("GameConfigurationTests: all tests passed");
	return 0;
}
