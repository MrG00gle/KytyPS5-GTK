#pragma once

// The field table behind Kyty.ini, Game_Settings_File, and validation (design.md,
// "Game_Configuration fields"; plan D21). Each field has its Kyty.ini and JSON key, its kind, its
// text form, typed access, and the rule a stored value must meet.
//
// Text forms are what the Qt launcher writes (QVariant::toString()): "true"/"false" for booleans,
// decimal integers, the Q_ENUM key name for enums, and the text itself for strings. Reading also
// accepts the legacy enum forms: the integer index ("1" is the second key) and, for screen
// resolutions, the "1280x720" spelling (Qt's TextToEnum: a text of 2+ characters that starts with
// a digit is read as "R" + its upper-case form). A string list (host_input_mapping) has no text
// form: it is a list in Kyty.ini and an array of strings in a Game_Settings_File.

#include "launcher/core/gameConfiguration.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kyty::core {

enum class FieldKind { Bool, Int, Enum, String, StringList };

// A typed field value: bool for Bool, int64_t for Int and for Enum (the index of the key in
// enum_keys), std::string for String, and std::vector<std::string> for StringList.
using FieldValue = std::variant<bool, int64_t, std::string, std::vector<std::string>>;

template <typename Target>
struct BasicFieldDescriptor {
	std::string_view                  name; // the Kyty.ini and JSON key
	FieldKind                         kind = FieldKind::String;
	std::span<const std::string_view> enum_keys; // Enum: the Q_ENUM key names in value order
	// The value of a text form, or nothing when the text is not one (always nothing for a
	// StringList). The value may still fail check().
	std::optional<FieldValue> (*parse)(std::string_view text) = nullptr;
	// The text form of a value of the field's kind ("" for a StringList or another alternative).
	std::string (*format)(const FieldValue& value) = nullptr;
	FieldValue (*get)(const Target& target)        = nullptr;
	// Stores the value; false, with target unchanged, when it is not of the field's kind or does
	// not fit (an enum index without a key, an integer outside int32_t). Controller integers are
	// clamped (Requirement 9.12) instead.
	bool (*set)(Target& target, const FieldValue& value) = nullptr;
	// The field's own rule, or nothing when value meets it: the field's range or form, as
	// Validate() checks it. Validate() adds the rules that need another field (an output path that
	// must not be empty while its log goes to a File) and the host_input_mapping entry form.
	std::optional<std::string> (*check)(const FieldValue& value) = nullptr;
};

using FieldDescriptor           = BasicFieldDescriptor<GameConfiguration>;
using ControllerFieldDescriptor = BasicFieldDescriptor<ControllerSettings>;

// The Game_Configuration fields in the order of the Qt launcher's Configuration::GameSettings()
// (screen_resolution ... amd_cpu_enabled, elf), then input_mapping_custom and host_input_mapping:
// 35 fields.
std::span<const FieldDescriptor> GameConfigurationFields();

// controller_color, controller_speaker_volume, controller_vibration_intensity,
// controller_keyboard_player.
std::span<const ControllerFieldDescriptor> GlobalOnlyFields();

// The Game_Configuration field with that key, or nullptr.
const FieldDescriptor* FindGameConfigurationField(std::string_view name);

// The key of the field that holds per game whether host_input_mapping is used
// (input_mapping_custom); the Global_Configuration does not store it.
inline constexpr std::string_view kInputMappingCustomField = "input_mapping_custom";

// The text forms of booleans and integers, also for the Kyty.ini keys outside the field table.
// ParseBoolText accepts exactly "true" and "false"; ParseIntText a decimal int64_t with an optional
// '-' and nothing else (no '+', no spaces).
std::optional<bool>    ParseBoolText(std::string_view text);
std::optional<int64_t> ParseIntText(std::string_view text);
std::string            FormatBoolText(bool value);

} // namespace kyty::core
