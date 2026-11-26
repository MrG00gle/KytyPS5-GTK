#include "launcher/core/gameConfiguration.h"

#include "launcher/core/configurationFields.h"
#include "launcher/core/text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <string>
#include <system_error>

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

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

// QRegularExpression's \s without Unicode properties: ASCII whitespace.
bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string_view TrimSpaces(std::string_view text) {
	while (!text.empty() && IsSpace(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && IsSpace(text.back())) {
		text.remove_suffix(1);
	}
	return text;
}

// A run of decimal digits as an int32_t: nothing when it is empty or does not fit.
std::optional<int32_t> ParseSide(std::string_view digits) {
	if (digits.empty() || !std::all_of(digits.begin(), digits.end(), IsDigit)) {
		return std::nullopt;
	}
	int32_t value         = 0;
	const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
	if (err != std::errc {} || end != digits.data() + digits.size()) {
		return std::nullopt;
	}
	return value;
}

// The four values of the old resolution enum, by their integer index (the legacy form).
constexpr std::array<ScreenResolution, 4> kLegacyResolutions = {
    ScreenResolution {1280, 720}, ScreenResolution {1920, 1080}, ScreenResolution {2560, 1440},
    ScreenResolution {3840, 2160}};

// The upstream launcher's presets, in its order: 16:9, then ultrawide.
constexpr std::array<ScreenResolution, 8> kResolutionPresets = {
    ScreenResolution {1280, 720},  ScreenResolution {1920, 1080}, ScreenResolution {2560, 1440},
    ScreenResolution {3840, 2160}, ScreenResolution {2560, 1080}, ScreenResolution {3440, 1440},
    ScreenResolution {3840, 1600}, ScreenResolution {5120, 2160}};

} // namespace

std::span<const ScreenResolution> ScreenResolutionPresets() {
	return kResolutionPresets;
}

bool IsValidScreenResolution(const ScreenResolution& resolution) {
	return resolution.width >= kMinScreenSize && resolution.width <= kMaxScreenSize &&
	       resolution.height >= kMinScreenSize && resolution.height <= kMaxScreenSize;
}

std::string FormatScreenResolution(const ScreenResolution& resolution) {
	return std::to_string(resolution.width) + "x" + std::to_string(resolution.height);
}

std::optional<ScreenResolution> ParseScreenResolution(std::string_view text) {
	// The legacy integer index of the old enum ("0" ... "3", also "003").
	if (!text.empty() && std::all_of(text.begin(), text.end(), IsDigit)) {
		uint64_t index        = 0;
		const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), index);
		if (err == std::errc {} && end == text.data() + text.size() &&
		    index < kLegacyResolutions.size()) {
			return kLegacyResolutions[index];
		}
		return std::nullopt;
	}
	// The upstream launcher's form ^\s*R?(\d+)\s*[xX]\s*(\d+)\s*$, which also reads the old keys
	// such as "R1920X1080".
	auto rest = TrimSpaces(text);
	if (!rest.empty() && rest.front() == 'R') {
		rest.remove_prefix(1);
	}
	const auto separator = rest.find_first_of("xX");
	if (separator == std::string_view::npos) {
		return std::nullopt;
	}
	const auto width  = ParseSide(TrimSpaces(rest.substr(0, separator)));
	const auto height = ParseSide(TrimSpaces(rest.substr(separator + 1)));
	// The width is digits only (no space between "R" and the digits, as in the expression).
	if (!width || !height || rest.empty() || !IsDigit(rest.front())) {
		return std::nullopt;
	}
	return ScreenResolution {*width, *height};
}

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
