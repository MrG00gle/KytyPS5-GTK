#include "launcher/core/gameConfiguration.h"

#include "launcher/core/configurationFields.h"
#include "launcher/core/text.h"

#include <algorithm>

namespace kyty::core {

namespace {

// "Name=Value" with both sides non-empty after TrimWhitespace (split at the first '=').
bool IsMappingEntry(std::string_view entry) {
	const auto split = entry.find('=');
	return split != std::string_view::npos && !TrimWhitespace(entry.substr(0, split)).empty() &&
	       !TrimWhitespace(entry.substr(split + 1)).empty();
}

// The Validate() rules beyond FieldDescriptor::check for the field called name.
std::optional<std::string> SaveRule(std::string_view name, const GameConfiguration& configuration) {
	if (IsMissingOutputPath(name, configuration)) {
		if (name == "shader_log_folder") {
			return "The shader log folder must not be empty while shader logs go to a file.";
		}
		if (name == "command_buffer_dump_folder") {
			return "The command buffer dump folder must not be empty while command buffer dumps "
			       "are enabled.";
		}
		return "The printf output file must not be empty while printf output goes to a file.";
	}
	if (name == "host_input_mapping") {
		for (const auto& entry: configuration.host_input_mapping) {
			if (!IsMappingEntry(entry)) {
				return "Each input mapping entry must have the form Name=Value with a non-empty "
				       "name and value; \"" +
				       entry + "\" does not.";
			}
		}
	}
	return std::nullopt;
}

bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

std::optional<ValidationError> Validate(const GameConfiguration& configuration) {
	for (const auto& field: GameConfigurationFields()) {
		auto message = field.check(field.get(configuration));
		if (!message) {
			message = SaveRule(field.name, configuration);
		}
		if (message) {
			return ValidationError {std::string(field.name), std::move(*message)};
		}
	}
	return std::nullopt;
}

bool IsMissingOutputPath(std::string_view field, const GameConfiguration& configuration) {
	if (field == "shader_log_folder") {
		return configuration.shader_log_folder.empty() &&
		       configuration.shader_log_direction == LogDirection::File;
	}
	if (field == "command_buffer_dump_folder") {
		return configuration.command_buffer_dump_folder.empty() &&
		       configuration.command_buffer_dump_enabled;
	}
	if (field == "printf_output_file") {
		return configuration.printf_output_file.empty() &&
		       configuration.printf_direction == LogDirection::File;
	}
	return false;
}

ControllerSettings ClampControllerSettings(int64_t volume, int64_t vibration,
                                           int64_t keyboard_player) {
	ControllerSettings settings;
	settings.speaker_volume =
	    static_cast<int32_t>(std::clamp<int64_t>(volume, 0, kMaxControllerPercent));
	settings.vibration_intensity =
	    static_cast<int32_t>(std::clamp<int64_t>(vibration, 0, kMaxControllerPercent));
	settings.keyboard_player =
	    static_cast<int32_t>(std::clamp<int64_t>(keyboard_player, 0, kMaxKeyboardPlayer));
	return settings;
}

bool IsValidControllerColor(std::string_view color) {
	return color.empty() || (color.size() == 7 && color.front() == '#' &&
	                         std::all_of(color.begin() + 1, color.end(), IsHexDigit));
}

} // namespace kyty::core
