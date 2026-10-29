#include "launcher/core/configurationFields.h"

#include "launcher/core/text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <system_error>

namespace kyty::core {

namespace {

using GC = GameConfiguration;
using CS = ControllerSettings;

using CheckFn = std::optional<std::string> (*)(const FieldValue&);

constexpr std::array<std::string_view, 4> kScreenResolutionKeys = {"R1280X720", "R1920X1080",
                                                                   "R2560X1440", "R3840X2160"};
constexpr std::array<std::string_view, 3> kPresentModeKeys      = {"Fifo", "Mailbox", "Immediate"};
constexpr std::array<std::string_view, 3> kShaderOptimizationTypeKeys = {"None", "Size",
                                                                         "Performance"};
constexpr std::array<std::string_view, 3> kLogDirectionKeys = {"Silent", "Console", "File"};

template <typename Enum>
std::span<const std::string_view> KeysOf();

template <>
std::span<const std::string_view> KeysOf<ScreenResolution>() {
	return kScreenResolutionKeys;
}
template <>
std::span<const std::string_view> KeysOf<PresentMode>() {
	return kPresentModeKeys;
}
template <>
std::span<const std::string_view> KeysOf<ShaderOptimizationType>() {
	return kShaderOptimizationTypeKeys;
}
template <>
std::span<const std::string_view> KeysOf<LogDirection>() {
	return kLogDirectionKeys;
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

char AsciiUpper(char c) {
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// ---- Text forms

std::optional<FieldValue> ParseBool(std::string_view text) {
	if (const auto value = ParseBoolText(text)) {
		return FieldValue(*value);
	}
	return std::nullopt;
}

std::optional<FieldValue> ParseInt(std::string_view text) {
	if (const auto value = ParseIntText(text)) {
		return FieldValue(*value);
	}
	return std::nullopt;
}

std::optional<FieldValue> ParseString(std::string_view text) {
	return FieldValue(std::string(text));
}

std::optional<FieldValue> ParseNothing(std::string_view /*text*/) {
	return std::nullopt;
}

// The key name; else the legacy integer index; else, for a text of 2+ characters that starts with a
// digit, "R" + the upper-case text (Qt's TextToEnum, which reads "1280x720" as R1280X720).
template <typename Enum>
std::optional<FieldValue> ParseEnum(std::string_view text) {
	const auto keys = KeysOf<Enum>();
	const auto find = [&keys](std::string_view key) -> std::optional<FieldValue> {
		for (size_t i = 0; i < keys.size(); i++) {
			if (keys[i] == key) {
				return FieldValue(static_cast<int64_t>(i));
			}
		}
		return std::nullopt;
	};
	if (auto value = find(text)) {
		return value;
	}
	if (!text.empty() && std::all_of(text.begin(), text.end(), IsDigit)) {
		uint64_t index        = 0;
		const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), index);
		if (err == std::errc {} && end == text.data() + text.size() && index < keys.size()) {
			return FieldValue(static_cast<int64_t>(index));
		}
		return std::nullopt;
	}
	if (text.size() > 1 && IsDigit(text.front())) {
		std::string key = "R";
		for (const char c: text) {
			key += AsciiUpper(c);
		}
		return find(key);
	}
	return std::nullopt;
}

std::string FormatBool(const FieldValue& value) {
	const auto* flag = std::get_if<bool>(&value);
	return flag != nullptr ? FormatBoolText(*flag) : std::string();
}

std::string FormatInt(const FieldValue& value) {
	const auto* number = std::get_if<int64_t>(&value);
	return number != nullptr ? std::to_string(*number) : std::string();
}

template <typename Enum>
std::string FormatEnum(const FieldValue& value) {
	const auto  keys  = KeysOf<Enum>();
	const auto* index = std::get_if<int64_t>(&value);
	if (index == nullptr || *index < 0 || static_cast<uint64_t>(*index) >= keys.size()) {
		return {};
	}
	return std::string(keys[static_cast<size_t>(*index)]);
}

std::string FormatString(const FieldValue& value) {
	const auto* text = std::get_if<std::string>(&value);
	return text != nullptr ? *text : std::string();
}

std::string FormatNothing(const FieldValue& /*value*/) {
	return {};
}

// ---- Access

template <typename Target, bool Target::* Member>
FieldValue GetBool(const Target& target) {
	return target.*Member;
}

template <typename Target, bool Target::* Member>
bool SetBool(Target& target, const FieldValue& value) {
	const auto* flag = std::get_if<bool>(&value);
	if (flag == nullptr) {
		return false;
	}
	target.*Member = *flag;
	return true;
}

template <typename Target, int32_t Target::* Member>
FieldValue GetInt(const Target& target) {
	return static_cast<int64_t>(target.*Member);
}

template <typename Target, int32_t Target::* Member>
bool SetInt(Target& target, const FieldValue& value) {
	const auto* number = std::get_if<int64_t>(&value);
	if (number == nullptr || *number < std::numeric_limits<int32_t>::min() ||
	    *number > std::numeric_limits<int32_t>::max()) {
		return false;
	}
	target.*Member = static_cast<int32_t>(*number);
	return true;
}

// Requirement 9.12: any integer is accepted and clamped to 0-Max.
template <typename Target, int32_t Target::* Member, int32_t Max>
bool SetClamped(Target& target, const FieldValue& value) {
	const auto* number = std::get_if<int64_t>(&value);
	if (number == nullptr) {
		return false;
	}
	target.*Member = static_cast<int32_t>(std::clamp<int64_t>(*number, 0, Max));
	return true;
}

template <typename Target, typename Enum, Enum Target::* Member>
FieldValue GetEnum(const Target& target) {
	return static_cast<int64_t>(target.*Member);
}

template <typename Target, typename Enum, Enum Target::* Member>
bool SetEnum(Target& target, const FieldValue& value) {
	const auto* index = std::get_if<int64_t>(&value);
	if (index == nullptr || *index < 0 || static_cast<uint64_t>(*index) >= KeysOf<Enum>().size()) {
		return false;
	}
	target.*Member = static_cast<Enum>(*index);
	return true;
}

template <typename Target, std::string Target::* Member>
FieldValue GetString(const Target& target) {
	return target.*Member;
}

template <typename Target, std::string Target::* Member>
bool SetString(Target& target, const FieldValue& value) {
	const auto* text = std::get_if<std::string>(&value);
	if (text == nullptr) {
		return false;
	}
	target.*Member = *text;
	return true;
}

template <typename Target, std::vector<std::string> Target::* Member>
FieldValue GetList(const Target& target) {
	return target.*Member;
}

template <typename Target, std::vector<std::string> Target::* Member>
bool SetList(Target& target, const FieldValue& value) {
	const auto* list = std::get_if<std::vector<std::string>>(&value);
	if (list == nullptr) {
		return false;
	}
	target.*Member = *list;
	return true;
}

// ---- Rules

constexpr const char* kWrongType = "The value has the wrong type.";

std::optional<std::string> CheckBool(const FieldValue& value) {
	return std::holds_alternative<bool>(value) ? std::nullopt
	                                           : std::optional<std::string>(kWrongType);
}

std::optional<std::string> CheckString(const FieldValue& value) {
	return std::holds_alternative<std::string>(value) ? std::nullopt
	                                                  : std::optional<std::string>(kWrongType);
}

std::optional<std::string> CheckList(const FieldValue& value) {
	return std::holds_alternative<std::vector<std::string>>(value)
	           ? std::nullopt
	           : std::optional<std::string>(kWrongType);
}

std::optional<std::string> CheckAnyInt(const FieldValue& value) {
	return std::holds_alternative<int64_t>(value) ? std::nullopt
	                                              : std::optional<std::string>(kWrongType);
}

template <typename Enum>
std::optional<std::string> CheckEnum(const FieldValue& value) {
	const auto  keys  = KeysOf<Enum>();
	const auto* index = std::get_if<int64_t>(&value);
	if (index != nullptr && *index >= 0 && static_cast<uint64_t>(*index) < keys.size()) {
		return std::nullopt;
	}
	std::string message = "The value must be one of ";
	for (size_t i = 0; i < keys.size(); i++) {
		message += (i == 0 ? "" : (i + 1 == keys.size() ? ", or " : ", "));
		message += keys[i];
	}
	return message + ".";
}

// The value when it is an int64_t in [min, max], else nothing.
std::optional<int64_t> IntIn(const FieldValue& value, int64_t min, int64_t max) {
	const auto* number = std::get_if<int64_t>(&value);
	if (number == nullptr || *number < min || *number > max) {
		return std::nullopt;
	}
	return *number;
}

std::optional<std::string> CheckUserName(const FieldValue& value) {
	const auto* text = std::get_if<std::string>(&value);
	if (text != nullptr) {
		const auto trimmed = TrimWhitespace(*text);
		if (!trimmed.empty() && trimmed.size() <= kMaxUserNameBytes) {
			return std::nullopt;
		}
	}
	return "The user name must contain 1-16 UTF-8 bytes after leading and trailing whitespace is "
	       "removed.";
}

std::optional<std::string> CheckUserId(const FieldValue& value) {
	const auto number = IntIn(value, 0, kMaxUserId);
	if (number && *number != 254 && *number != 255) {
		return std::nullopt;
	}
	return "The user ID must be between 0 and 2147482879 and must not be 254 or 255.";
}

std::optional<std::string> CheckGpuIndex(const FieldValue& value) {
	if (IntIn(value, -1, std::numeric_limits<int32_t>::max())) {
		return std::nullopt;
	}
	return "The GPU index must be -1 (automatic) or a device index from 0 to 2147483647.";
}

std::optional<std::string> CheckTrophyDuration(const FieldValue& value) {
	if (IntIn(value, 5, 10)) {
		return std::nullopt;
	}
	return "The trophy notification duration must be between 5 and 10 seconds.";
}

std::optional<std::string> CheckVblankFrequency(const FieldValue& value) {
	if (IntIn(value, 30, 360)) {
		return std::nullopt;
	}
	return "The vblank frequency must be between 30 and 360.";
}

std::optional<std::string> CheckConsoleLanguage(const FieldValue& value) {
	if (IntIn(value, 0, kMaxConsoleLanguage)) {
		return std::nullopt;
	}
	return "The console language must be between 0 and 29.";
}

// A non-empty relative path without a ".." component ('/' separates components).
std::optional<std::string> CheckElf(const FieldValue& value) {
	const auto* text = std::get_if<std::string>(&value);
	if (text != nullptr && !text->empty() && text->front() != '/') {
		bool        parent = false;
		std::size_t start  = 0;
		while (start <= text->size()) {
			const auto end = std::min(text->find('/', start), text->size());
			if (std::string_view(*text).substr(start, end - start) == "..") {
				parent = true;
			}
			start = end + 1;
		}
		if (!parent) {
			return std::nullopt;
		}
	}
	return "The executable must be a non-empty relative path without a \"..\" component.";
}

std::optional<std::string> CheckControllerColor(const FieldValue& value) {
	const auto* text = std::get_if<std::string>(&value);
	if (text != nullptr && IsValidControllerColor(*text)) {
		return std::nullopt;
	}
	return "The controller color must be empty or #RRGGBB.";
}

// ---- The tables

template <bool GC::* Member>
FieldDescriptor BoolField(std::string_view name) {
	return {name,
	        FieldKind::Bool,
	        {},
	        ParseBool,
	        FormatBool,
	        GetBool<GC, Member>,
	        SetBool<GC, Member>,
	        CheckBool};
}

template <int32_t GC::* Member>
FieldDescriptor IntField(std::string_view name, CheckFn check) {
	return {name, FieldKind::Int, {}, ParseInt, FormatInt, GetInt<GC, Member>, SetInt<GC, Member>,
	        check};
}

template <typename Enum, Enum GC::* Member>
FieldDescriptor EnumField(std::string_view name) {
	return {name,
	        FieldKind::Enum,
	        KeysOf<Enum>(),
	        ParseEnum<Enum>,
	        FormatEnum<Enum>,
	        GetEnum<GC, Enum, Member>,
	        SetEnum<GC, Enum, Member>,
	        CheckEnum<Enum>};
}

template <std::string GC::* Member>
FieldDescriptor StringField(std::string_view name, CheckFn check = CheckString) {
	return {name,
	        FieldKind::String,
	        {},
	        ParseString,
	        FormatString,
	        GetString<GC, Member>,
	        SetString<GC, Member>,
	        check};
}

template <std::vector<std::string> GC::* Member>
FieldDescriptor ListField(std::string_view name) {
	return {name,
	        FieldKind::StringList,
	        {},
	        ParseNothing,
	        FormatNothing,
	        GetList<GC, Member>,
	        SetList<GC, Member>,
	        CheckList};
}

template <int32_t CS::* Member, int32_t Max>
ControllerFieldDescriptor ClampedField(std::string_view name) {
	return {name,
	        FieldKind::Int,
	        {},
	        ParseInt,
	        FormatInt,
	        GetInt<CS, Member>,
	        SetClamped<CS, Member, Max>,
	        CheckAnyInt};
}

} // namespace

std::span<const FieldDescriptor> GameConfigurationFields() {
	static const std::array<FieldDescriptor, 35> fields = {
	    EnumField<ScreenResolution, &GC::screen_resolution>("screen_resolution"),
	    StringField<&GC::user_name>("user_name", CheckUserName),
	    IntField<&GC::user_id>("user_id", CheckUserId),
	    StringField<&GC::audio_input_device>("audio_input_device"),
	    EnumField<PresentMode, &GC::present_mode>("present_mode"),
	    IntField<&GC::gpu_index>("gpu_index", CheckGpuIndex),
	    BoolField<&GC::fullscreen_enabled>("fullscreen_enabled"),
	    BoolField<&GC::hdr_enabled>("hdr_enabled"),
	    BoolField<&GC::hide_cursor_enabled>("hide_cursor_enabled"),
	    BoolField<&GC::readback_linear_images>("readback_linear_images"),
	    BoolField<&GC::readback_tiled_images>("readback_tiled_images"),
	    BoolField<&GC::sync_raw_image_buffers>("sync_raw_image_buffers"),
	    BoolField<&GC::tessellation_enabled>("tessellation_enabled"),
	    BoolField<&GC::trophy_enabled>("trophy_enabled"),
	    IntField<&GC::trophy_duration_seconds>("trophy_duration_seconds", CheckTrophyDuration),
	    BoolField<&GC::perf_overlay_enabled>("perf_overlay_enabled"),
	    BoolField<&GC::skip_notice_screen>("skip_notice_screen"),
	    BoolField<&GC::network_connected>("network_connected"),
	    IntField<&GC::vblank_frequency>("vblank_frequency", CheckVblankFrequency),
	    IntField<&GC::console_language>("console_language", CheckConsoleLanguage),
	    BoolField<&GC::vulkan_validation_enabled>("vulkan_validation_enabled"),
	    BoolField<&GC::shader_validation_enabled>("shader_validation_enabled"),
	    EnumField<ShaderOptimizationType, &GC::shader_optimization_type>(
	        "shader_optimization_type"),
	    EnumField<LogDirection, &GC::shader_log_direction>("shader_log_direction"),
	    StringField<&GC::shader_log_folder>("shader_log_folder"),
	    BoolField<&GC::command_buffer_dump_enabled>("command_buffer_dump_enabled"),
	    StringField<&GC::command_buffer_dump_folder>("command_buffer_dump_folder"),
	    EnumField<LogDirection, &GC::printf_direction>("printf_direction"),
	    StringField<&GC::printf_output_file>("printf_output_file"),
	    BoolField<&GC::profiler_enabled>("profiler_enabled"),
	    BoolField<&GC::renderdoc_enabled>("renderdoc_enabled"),
	    BoolField<&GC::amd_cpu_enabled>("amd_cpu_enabled"),
	    StringField<&GC::elf>("elf", CheckElf),
	    BoolField<&GC::input_mapping_custom>(kInputMappingCustomField),
	    ListField<&GC::host_input_mapping>("host_input_mapping"),
	};
	return fields;
}

std::span<const ControllerFieldDescriptor> GlobalOnlyFields() {
	static const std::array<ControllerFieldDescriptor, 4> fields = {
	    ControllerFieldDescriptor {"controller_color",
	                               FieldKind::String,
	                               {},
	                               ParseString,
	                               FormatString,
	                               GetString<CS, &CS::color>,
	                               SetString<CS, &CS::color>,
	                               CheckControllerColor},
	    ClampedField<&CS::speaker_volume, kMaxControllerPercent>("controller_speaker_volume"),
	    ClampedField<&CS::vibration_intensity, kMaxControllerPercent>(
	        "controller_vibration_intensity"),
	    ClampedField<&CS::keyboard_player, kMaxKeyboardPlayer>("controller_keyboard_player"),
	};
	return fields;
}

const FieldDescriptor* FindGameConfigurationField(std::string_view name) {
	for (const auto& field: GameConfigurationFields()) {
		if (field.name == name) {
			return &field;
		}
	}
	return nullptr;
}

std::optional<bool> ParseBoolText(std::string_view text) {
	if (text == "true") {
		return true;
	}
	if (text == "false") {
		return false;
	}
	return std::nullopt;
}

std::optional<int64_t> ParseIntText(std::string_view text) {
	if (text.empty()) {
		return std::nullopt;
	}
	int64_t value         = 0;
	const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (err != std::errc {} || end != text.data() + text.size()) {
		return std::nullopt;
	}
	return value;
}

std::string FormatBoolText(bool value) {
	return value ? "true" : "false";
}

} // namespace kyty::core
