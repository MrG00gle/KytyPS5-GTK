#include "launcher/core/inputMapping.h"

#include "launcher/core/keyNames.h"
#include "launcher/core/text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

constexpr std::string_view kKeyboardDisabled  = "Keyboard=Disabled";
constexpr std::string_view kPadDisabled       = "Pad=Disabled";
constexpr std::string_view kPadPrefix         = "Pad:";
constexpr std::string_view kMouseSensitivity  = "MouseSensitivity=";
constexpr std::string_view kTouchParticleSkin = "TouchParticleSkin=";
constexpr std::string_view kTouchCursorImage  = "TouchCursorImage=";
constexpr std::string_view kTouchCursorOff    = "TouchCursorImage=Off";

constexpr double kMinMouseSensitivity = 0.1;
constexpr double kMaxMouseSensitivity = 5.0;

using S = InputSection;

// The Qt editor's PAD_CONTROLS: id, label, keyboard label, keyboard default, pad default, on the
// Gamepad tab, pad-mappable in the Emulator (every control but the stick directions), section.
constexpr std::array kControls = {
    InputControl {"Cross", "Cross", "Cross", "J", "South", true, true, S::Face},
    InputControl {"Circle", "Circle", "Circle", "L", "East", true, true, S::Face},
    InputControl {"Square", "Square", "Square", "K", "West", true, true, S::Face},
    InputControl {"Triangle", "Triangle", "Triangle", "I", "North", true, true, S::Face},
    InputControl {"Up", "Up", "Up", "Up", "DpadUp", true, true, S::Dpad},
    InputControl {"Down", "Down", "Down", "Down", "DpadDown", true, true, S::Dpad},
    InputControl {"Left", "Left", "Left", "Left", "DpadLeft", true, true, S::Dpad},
    InputControl {"Right", "Right", "Right", "Right", "DpadRight", true, true, S::Dpad},
    InputControl {"L1", "L1", "L1", "Q", "LeftShoulder", true, true, S::Shoulders},
    InputControl {"R1", "R1", "R1", "E", "RightShoulder", true, true, S::Shoulders},
    InputControl {"L2", "L2", "L2", "Z", "LeftTrigger", true, true, S::Shoulders},
    InputControl {"R2", "R2", "R2", "C", "RightTrigger", true, true, S::Shoulders},
    InputControl {"L3", "L3 (left stick click)", "L3 (left stick click)", "Left Shift", "LeftStick",
                  true, true, S::Sticks},
    InputControl {"R3", "R3 (right stick click)", "R3 (right stick click)", "Left Ctrl",
                  "RightStick", true, true, S::Sticks},
    InputControl {"Options", "Options", "Options", "Return", "Start", true, true, S::System},
    InputControl {"TouchPad", "Touch pad click", "Touch pad left", "Backspace", "Touchpad", true,
                  true, S::System},
    InputControl {"TouchPadRight", "Touch pad right", "Touch pad right", "Tab", "", false, true,
                  S::System},
    InputControl {"GyroToggle", "Gyro emulation toggle", "Gyro emulation toggle", "", "Back", true,
                  true, S::System},
    InputControl {"TouchPadMode", "Emulated touch pad mode", "Emulated touch pad mode", "F6", "",
                  true, true, S::System},
    InputControl {"LeftStickUp", "Up", "Up", "W", "", false, false, S::LeftStick},
    InputControl {"LeftStickDown", "Down", "Down", "S", "", false, false, S::LeftStick},
    InputControl {"LeftStickLeft", "Left", "Left", "A", "", false, false, S::LeftStick},
    InputControl {"LeftStickRight", "Right", "Right", "D", "", false, false, S::LeftStick},
    InputControl {"RightStickUp", "Up", "Up", "T", "", false, false, S::RightStick},
    InputControl {"RightStickDown", "Down", "Down", "G", "", false, false, S::RightStick},
    InputControl {"RightStickLeft", "Left", "Left", "F", "", false, false, S::RightStick},
    InputControl {"RightStickRight", "Right", "Right", "H", "", false, false, S::RightStick},
    InputControl {"SpeakerVolume", "Speaker volume", "Speaker volume", "1", "", false, true,
                  S::Shortcuts},
    InputControl {"VibrationIntensity", "Vibration intensity", "Vibration intensity", "2", "",
                  false, true, S::Shortcuts},
    InputControl {"TriggerEffectIntensity", "Trigger intensity", "Trigger intensity", "3", "",
                  false, true, S::Shortcuts},
};

constexpr std::array kPadSources = {
    PadSource {"South", "A / Cross"},
    PadSource {"East", "B / Circle"},
    PadSource {"West", "X / Square"},
    PadSource {"North", "Y / Triangle"},
    PadSource {"Back", "View / Back / Share"},
    PadSource {"Guide", "Guide / Home"},
    PadSource {"Start", "Menu / Start"},
    PadSource {"LeftStick", "Left stick click"},
    PadSource {"RightStick", "Right stick click"},
    PadSource {"LeftShoulder", "LB / L1"},
    PadSource {"RightShoulder", "RB / R1"},
    PadSource {"DpadUp", "D-pad Up"},
    PadSource {"DpadDown", "D-pad Down"},
    PadSource {"DpadLeft", "D-pad Left"},
    PadSource {"DpadRight", "D-pad Right"},
    PadSource {"Misc1", "Share / Misc"},
    PadSource {"RightPaddle1", "Right paddle 1"},
    PadSource {"LeftPaddle1", "Left paddle 1"},
    PadSource {"RightPaddle2", "Right paddle 2"},
    PadSource {"LeftPaddle2", "Left paddle 2"},
    PadSource {"Touchpad", "Touch pad click"},
    PadSource {"LeftTrigger", "LT / L2"},
    PadSource {"RightTrigger", "RT / R2"},
};

constexpr std::array kSkins = {
    TouchParticleSkinInfo {TouchParticleSkin::Classic, "Classic", "Classic"},
    TouchParticleSkinInfo {TouchParticleSkin::BlueNeon, "BlueNeon", "Blue neon"},
    TouchParticleSkinInfo {TouchParticleSkin::Monochrome, "Monochrome", "Monochrome"},
    TouchParticleSkinInfo {TouchParticleSkin::Crimson, "Crimson", "Crimson"},
    TouchParticleSkinInfo {TouchParticleSkin::RedBlue, "RedBlue", "Red/Blue"},
};

using Bindings = std::map<std::string, std::string>;

// Qt::CaseInsensitive comparison (SDL_strcasecmp); text with a NUL byte must match byte for byte.
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
	if (a.find('\0') != std::string_view::npos || b.find('\0') != std::string_view::npos) {
		return a == b;
	}
	return CompareCaseInsensitive(a, b) == 0;
}

bool IsPadSource(std::string_view id) {
	return std::any_of(kPadSources.begin(), kPadSources.end(),
	                   [id](const auto& source) { return source.id == id; });
}

// Qt's AssignBinding: control gets value, and no other control keeps the same value.
void Bind(Bindings& bindings, std::string_view control, std::string_view value) {
	for (auto it = bindings.begin(); it != bindings.end();) {
		if (it->first != control && EqualsIgnoreCase(it->second, value)) {
			it = bindings.erase(it);
		} else {
			++it;
		}
	}
	bindings[std::string(control)] = std::string(value);
}

Bindings KeyboardDefaults() {
	Bindings bindings;
	for (const auto& control: kControls) {
		if (!control.keyboard_default.empty()) {
			bindings.emplace(control.id, control.keyboard_default);
		}
	}
	return bindings;
}

Bindings PadDefaults() {
	Bindings bindings;
	for (const auto& control: kControls) {
		if (!control.pad_default.empty()) {
			bindings.emplace(control.id, control.pad_default);
		}
	}
	return bindings;
}

// The Emulator's std::strtof(...) of a decimal number (leading whitespace and a sign allowed, the
// longest number prefix used, none giving 0), then clamp(isfinite ? value : 1.0, 0.1, 5.0); in
// tenths, rounded.
int ParseMouseSensitivity(std::string_view text) {
	while (!text.empty() &&
	       (text.front() == ' ' || (text.front() >= '\t' && text.front() <= '\r'))) {
		text.remove_prefix(1);
	}
	if (!text.empty() && text.front() == '+') {
		text.remove_prefix(1);
	}
	double     value  = 0.0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	if (parsed.ec == std::errc::result_out_of_range) {
		value = 1.0; // the strtof result is infinite (or a denormal, rare enough to ignore)
	} else if (parsed.ec != std::errc()) {
		value = 0.0;
	}
	if (!std::isfinite(value)) {
		value = 1.0;
	}
	value = std::clamp(value, kMinMouseSensitivity, kMaxMouseSensitivity);
	return std::clamp(static_cast<int>(std::lround(value * 10.0)), kMinMouseSensitivityTenths,
	                  kMaxMouseSensitivityTenths);
}

std::optional<TouchParticleSkin> FindSkin(std::string_view key) {
	for (const auto& skin: kSkins) {
		if (skin.key == key) {
			return skin.skin;
		}
	}
	return std::nullopt;
}

bool IsValidKeyboardInput(std::string_view input) {
	return IsMouseButtonName(input) || (IsEmulatorKeyName(input) && !IsReservedKeyName(input));
}

} // namespace

std::string_view InputSectionTitle(InputSection section) {
	switch (section) {
		case InputSection::Face: return "Face buttons";
		case InputSection::Dpad: return "D-pad";
		case InputSection::Shoulders: return "Shoulders & triggers";
		case InputSection::Sticks: return "Stick clicks";
		case InputSection::System: return "System";
		case InputSection::LeftStick: return "Left stick";
		case InputSection::RightStick: return "Right stick";
		case InputSection::Shortcuts: break;
	}
	return "In-game shortcuts (cycle)";
}

std::span<const InputControl> InputControls() {
	return kControls;
}

const InputControl* FindInputControl(std::string_view id) {
	const auto found = std::find_if(kControls.begin(), kControls.end(),
	                                [id](const auto& control) { return control.id == id; });
	return found != kControls.end() ? &*found : nullptr;
}

std::span<const PadSource> PadSources() {
	return kPadSources;
}

std::string_view PadSourceLabel(std::string_view id) {
	for (const auto& source: kPadSources) {
		if (source.id == id) {
			return source.label;
		}
	}
	return id;
}

std::span<const TouchParticleSkinInfo> TouchParticleSkins() {
	return kSkins;
}

bool IsMouseButtonName(std::string_view name) {
	return std::find(kMouseButtonNames.begin(), kMouseButtonNames.end(), name) !=
	       kMouseButtonNames.end();
}

InputMappingModel ParseMapping(std::span<const std::string> entries) {
	InputMappingModel model;
	bool              skin_found = false;
	for (const auto& text: entries) {
		const std::string_view entry = text;
		if (entry == kKeyboardDisabled) {
			model.keyboard_custom = true;
		} else if (entry == kPadDisabled) {
			model.pad_custom = true;
		} else if (entry.starts_with(kMouseSensitivity)) {
			model.mouse_sensitivity_tenths =
			    ParseMouseSensitivity(entry.substr(kMouseSensitivity.size()));
		} else if (entry.starts_with(kTouchParticleSkin)) {
			const auto skin = FindSkin(entry.substr(kTouchParticleSkin.size()));
			if (skin && !skin_found) {
				model.particle_skin = *skin;
				skin_found          = true;
			}
		} else if (entry.starts_with(kTouchCursorImage)) {
			model.touch_cursor = model.touch_cursor && entry != kTouchCursorOff;
		} else if (entry.starts_with(kPadPrefix)) {
			const auto  body      = entry.substr(kPadPrefix.size());
			const auto  separator = body.find('=');
			const auto* control   = separator != std::string_view::npos
			                            ? FindInputControl(body.substr(0, separator))
			                            : nullptr;
			if (control != nullptr && control->pad_mappable &&
			    IsPadSource(body.substr(separator + 1))) {
				Bind(model.pad, control->id, body.substr(separator + 1));
				model.pad_custom = true;
			} else {
				model.unknown.push_back(text);
			}
		} else {
			const auto  separator = entry.find('=');
			const auto* control   = separator != std::string_view::npos
			                            ? FindInputControl(entry.substr(0, separator))
			                            : nullptr;
			if (control != nullptr && separator + 1 < entry.size()) {
				Bind(model.keyboard, control->id, entry.substr(separator + 1));
				model.keyboard_custom = true;
			} else {
				model.unknown.push_back(text);
			}
		}
	}
	if (!model.keyboard_custom) {
		model.keyboard = KeyboardDefaults();
	}
	if (!model.pad_custom) {
		model.pad = PadDefaults();
	}
	return model;
}

std::vector<std::string> SerializeMapping(const InputMappingModel& model) {
	std::vector<std::string> entries;
	const auto               write = [&entries](const Bindings& bindings, std::string_view prefix,
	                                            std::string_view disabled) {
		const auto start = entries.size();
		for (const auto& control: kControls) {
			const auto found = bindings.find(std::string(control.id));
			if (found != bindings.end() && !found->second.empty()) {
				entries.push_back(std::string(prefix) + std::string(control.id) + "=" +
				                  found->second);
			}
		}
		if (entries.size() == start) {
			entries.emplace_back(disabled);
		}
	};
	if (model.keyboard_custom) {
		write(model.keyboard, "", kKeyboardDisabled);
	}
	if (model.pad_custom) {
		write(model.pad, kPadPrefix, kPadDisabled);
	}
	const int tenths = std::clamp(model.mouse_sensitivity_tenths, kMinMouseSensitivityTenths,
	                              kMaxMouseSensitivityTenths);
	if (tenths != kDefaultMouseSensitivityTenths) {
		entries.push_back(std::string(kMouseSensitivity) + std::to_string(tenths / 10) + "." +
		                  std::to_string(tenths % 10));
	}
	if (model.particle_skin != TouchParticleSkin::Classic) {
		for (const auto& skin: kSkins) {
			if (skin.skin == model.particle_skin) {
				entries.push_back(std::string(kTouchParticleSkin) + std::string(skin.key));
			}
		}
	}
	if (!model.touch_cursor) {
		entries.emplace_back(kTouchCursorOff);
	}
	entries.insert(entries.end(), model.unknown.begin(), model.unknown.end());
	return entries;
}

bool Assign(InputMappingModel& model, InputDevice device, std::string_view control,
            std::string_view input) {
	const auto* found = FindInputControl(control);
	if (found == nullptr) {
		return false;
	}
	if (device == InputDevice::Pad) {
		if (!found->pad_mappable || !IsPadSource(input)) {
			return false;
		}
		Bind(model.pad, found->id, input);
		model.pad_custom = true;
		return true;
	}
	if (!IsValidKeyboardInput(input) ||
	    !IsValidMappingEntry(std::string(found->id) + "=" + std::string(input))) {
		return false;
	}
	Bind(model.keyboard, found->id, input);
	model.keyboard_custom = true;
	return true;
}

bool Unmap(InputMappingModel& model, InputDevice device, std::string_view control) {
	const auto* found = FindInputControl(control);
	if (found == nullptr || (device == InputDevice::Pad && !found->pad_mappable)) {
		return false;
	}
	if (device == InputDevice::Pad) {
		model.pad.erase(std::string(found->id));
		model.pad_custom = true;
	} else {
		model.keyboard.erase(std::string(found->id));
		model.keyboard_custom = true;
	}
	return true;
}

void RestoreDefaults(InputMappingModel& model, InputTab tab) {
	if (tab == InputTab::Gamepad) {
		model.pad        = PadDefaults();
		model.pad_custom = false;
		return;
	}
	model.keyboard                 = KeyboardDefaults();
	model.keyboard_custom          = false;
	model.mouse_sensitivity_tenths = kDefaultMouseSensitivityTenths;
}

void SetMouseSensitivity(InputMappingModel& model, int tenths) {
	model.mouse_sensitivity_tenths =
	    std::clamp(tenths, kMinMouseSensitivityTenths, kMaxMouseSensitivityTenths);
}

std::vector<std::string> MappingForTarget(const LauncherSettings& settings, MappingTarget target,
                                          const GameEntry& entry) {
	if (target == MappingTarget::Game) {
		const auto* game = FindGameSettings(settings, entry);
		if (game != nullptr && game->configuration.input_mapping_custom) {
			return game->configuration.host_input_mapping;
		}
	}
	return settings.global.host_input_mapping;
}

Expected<void, ValidationError> SaveMapping(LauncherSettings& settings, MappingTarget target,
                                            const GameEntry&         entry,
                                            const InputMappingModel& model) {
	if (target == MappingTarget::Global) {
		auto global               = settings.global;
		global.host_input_mapping = SerializeMapping(model);
		if (auto error = Validate(global)) {
			return MakeUnexpected(std::move(*error));
		}
		settings.global = std::move(global);
		return {};
	}
	auto configuration                 = EnableCustomSettings(settings, entry);
	configuration.input_mapping_custom = true;
	configuration.host_input_mapping   = SerializeMapping(model);
	return SaveCustomSettings(settings, entry, configuration);
}

bool UseGlobalMapping(LauncherSettings& settings, const GameEntry& entry) {
	const auto* game = FindGameSettings(settings, entry);
	if (game == nullptr || !game->configuration.input_mapping_custom) {
		return false;
	}
	const auto index = static_cast<std::size_t>(game - settings.games.data());
	settings.games[index].configuration.input_mapping_custom = false;
	return true;
}

} // namespace kyty::core
