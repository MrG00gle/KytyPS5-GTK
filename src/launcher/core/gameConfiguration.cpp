#include "launcher/core/gameConfiguration.h"

#include "launcher/core/configurationFields.h"
#include "launcher/core/text.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace kyty::core {

namespace {

// The Validate() rules beyond FieldDescriptor::check for the field called name.
std::optional<ValidationError>
SaveRule(std::string_view name, const GameConfiguration& configuration, ValidationScope scope) {
	const auto error = [name](std::string message, ValidationRule rule) {
		return ValidationError {std::string(name), std::move(message), rule, {}};
	};
	if (IsMissingOutputPath(name, configuration)) {
		if (name == "shader_log_folder") {
			return error("The shader log folder must not be empty while shader logs go to a file.",
			             ValidationRule::MissingOutputPath);
		}
		if (name == "command_buffer_dump_folder") {
			return error("The command buffer dump folder must not be empty while command buffer "
			             "dumps are enabled.",
			             ValidationRule::MissingOutputPath);
		}
		return error("The printf output file must not be empty while printf output goes to a file.",
		             ValidationRule::MissingOutputPath);
	}
	if (name == "host_input_mapping" &&
	    (scope == ValidationScope::Global || configuration.input_mapping_custom)) {
		auto invalid = FindInvalidMappingEntries(configuration.host_input_mapping);
		if (!invalid.empty()) {
			std::string message = "Each input mapping entry must have the form Name=Value with a "
			                      "non-empty name and value. Invalid: ";
			for (size_t i = 0; i < invalid.size(); i++) {
				message += (i == 0 ? "entry " : ", entry ") + std::to_string(invalid[i].index + 1) +
				           " \"" + invalid[i].entry + "\"";
			}
			auto result    = error(message + ".", ValidationRule::InvalidMappingEntries);
			result.entries = std::move(invalid);
			return result;
		}
	}
	return std::nullopt;
}

bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

std::optional<ValidationError> Validate(const GameConfiguration& configuration,
                                        ValidationScope          scope) {
	for (const auto& field: GameConfigurationFields()) {
		if (auto message = field.check(field.get(configuration))) {
			return ValidationError {
			    std::string(field.name), std::move(*message), ValidationRule::Field, {}};
		}
		if (auto error = SaveRule(field.name, configuration, scope)) {
			return error;
		}
	}
	return std::nullopt;
}

bool IsValidMappingEntry(std::string_view entry) {
	const auto split = entry.find('=');
	return split != std::string_view::npos && !TrimWhitespace(entry.substr(0, split)).empty() &&
	       !TrimWhitespace(entry.substr(split + 1)).empty();
}

std::vector<InvalidMappingEntry> FindInvalidMappingEntries(std::span<const std::string> mapping) {
	std::vector<InvalidMappingEntry> invalid;
	for (size_t i = 0; i < mapping.size(); i++) {
		if (!IsValidMappingEntry(mapping[i])) {
			invalid.push_back({i, mapping[i]});
		}
	}
	return invalid;
}

std::vector<std::string> DropInvalidMappingEntries(std::span<const std::string> mapping) {
	std::vector<std::string> valid;
	std::copy_if(mapping.begin(), mapping.end(), std::back_inserter(valid),
	             [](const std::string& entry) { return IsValidMappingEntry(entry); });
	return valid;
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
