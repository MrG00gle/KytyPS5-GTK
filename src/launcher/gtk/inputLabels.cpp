#include "launcher/gtk/inputLabels.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <iterator>

namespace kyty::gtk {

namespace {

using core::InputSection;

struct SectionText {
	InputSection section;
	const char*  text;
};

constexpr SectionText kSectionTexts[] = {
    {InputSection::Face, N_("Face buttons")},
    {InputSection::Dpad, N_("D-pad")},
    {InputSection::Shoulders, N_("Shoulders and triggers")},
    {InputSection::Sticks, N_("Stick clicks")},
    {InputSection::System, N_("System")},
    {InputSection::LeftStick, N_("Left stick")},
    {InputSection::RightStick, N_("Right stick")},
    {InputSection::Shortcuts, N_("In-game shortcuts (cycle)")},
};

// Every control of core::InputControls(): its Gamepad and its Keyboard and mouse label.
struct ControlText {
	std::string_view id;
	const char*      pad;
	const char*      keyboard;
};

constexpr ControlText kControlTexts[] = {
    // TRANSLATORS: the PS5 controls the input mapping editor binds. "Up", "Down", "Left", and
    // "Right" are D-pad buttons and stick directions.
    {"Cross", N_("Cross"), N_("Cross")},
    {"Circle", N_("Circle"), N_("Circle")},
    {"Square", N_("Square"), N_("Square")},
    {"Triangle", N_("Triangle"), N_("Triangle")},
    {"Up", N_("Up"), N_("Up")},
    {"Down", N_("Down"), N_("Down")},
    {"Left", N_("Left"), N_("Left")},
    {"Right", N_("Right"), N_("Right")},
    {"L1", N_("L1"), N_("L1")},
    {"R1", N_("R1"), N_("R1")},
    {"L2", N_("L2"), N_("L2")},
    {"R2", N_("R2"), N_("R2")},
    {"L3", N_("L3 (left stick click)"), N_("L3 (left stick click)")},
    {"R3", N_("R3 (right stick click)"), N_("R3 (right stick click)")},
    {"Options", N_("Options"), N_("Options")},
    {"TouchPad", N_("Touch pad click"), N_("Touch pad left")},
    {"TouchPadRight", N_("Touch pad right"), N_("Touch pad right")},
    {"GyroToggle", N_("Gyro emulation toggle"), N_("Gyro emulation toggle")},
    {"TouchPadMode", N_("Emulated touch pad mode"), N_("Emulated touch pad mode")},
    {"LeftStickUp", N_("Up"), N_("Up")},
    {"LeftStickDown", N_("Down"), N_("Down")},
    {"LeftStickLeft", N_("Left"), N_("Left")},
    {"LeftStickRight", N_("Right"), N_("Right")},
    {"RightStickUp", N_("Up"), N_("Up")},
    {"RightStickDown", N_("Down"), N_("Down")},
    {"RightStickLeft", N_("Left"), N_("Left")},
    {"RightStickRight", N_("Right"), N_("Right")},
    {"SpeakerVolume", N_("Speaker volume"), N_("Speaker volume")},
    {"VibrationIntensity", N_("Vibration intensity"), N_("Vibration intensity")},
    {"TriggerEffectIntensity", N_("Trigger intensity"), N_("Trigger intensity")},
};

struct SourceText {
	std::string_view id;
	const char*      text;
};

constexpr SourceText kSourceTexts[] = {
    // TRANSLATORS: gamepad inputs, named as on Xbox and PlayStation controllers.
    {"South", N_("A / Cross")},
    {"East", N_("B / Circle")},
    {"West", N_("X / Square")},
    {"North", N_("Y / Triangle")},
    {"Back", N_("View / Back / Share")},
    {"Guide", N_("Guide / Home")},
    {"Start", N_("Menu / Start")},
    {"LeftStick", N_("Left stick click")},
    {"RightStick", N_("Right stick click")},
    {"LeftShoulder", N_("LB / L1")},
    {"RightShoulder", N_("RB / R1")},
    {"DpadUp", N_("D-pad Up")},
    {"DpadDown", N_("D-pad Down")},
    {"DpadLeft", N_("D-pad Left")},
    {"DpadRight", N_("D-pad Right")},
    {"Misc1", N_("Share / Misc")},
    {"RightPaddle1", N_("Right paddle 1")},
    {"LeftPaddle1", N_("Left paddle 1")},
    {"RightPaddle2", N_("Right paddle 2")},
    {"LeftPaddle2", N_("Left paddle 2")},
    {"Touchpad", N_("Touch pad click")},
    {"LeftTrigger", N_("LT / L2")},
    {"RightTrigger", N_("RT / R2")},
};

struct SkinText {
	core::TouchParticleSkin skin;
	const char*             text;
};

constexpr SkinText kSkinTexts[] = {
    // TRANSLATORS: color schemes of the emulated touch pad's particles.
    {core::TouchParticleSkin::Classic, N_("Classic")},
    {core::TouchParticleSkin::BlueNeon, N_("Blue neon")},
    {core::TouchParticleSkin::Monochrome, N_("Monochrome")},
    {core::TouchParticleSkin::Crimson, N_("Crimson")},
    {core::TouchParticleSkin::RedBlue, N_("Red/Blue")},
};

} // namespace

std::string InputSectionText(InputSection section) {
	const auto* found =
	    std::find_if(std::begin(kSectionTexts), std::end(kSectionTexts),
	                 [section](const SectionText& text) { return text.section == section; });
	return found != std::end(kSectionTexts) ? _(found->text)
	                                        : std::string(core::InputSectionTitle(section));
}

std::string InputControlText(const core::InputControl& control, core::InputDevice device) {
	const bool  pad = device == core::InputDevice::Pad;
	const auto* found =
	    std::find_if(std::begin(kControlTexts), std::end(kControlTexts),
	                 [&control](const ControlText& text) { return text.id == control.id; });
	if (found == std::end(kControlTexts)) {
		return std::string(pad ? control.label : control.keyboard_label);
	}
	return _(pad ? found->pad : found->keyboard);
}

std::string InputControlTitle(const core::InputControl& control, core::InputDevice device) {
	const std::string section = InputSectionText(control.section);
	const std::string label   = InputControlText(control, device);
	// TRANSLATORS: a binding of the input mapping editor: "<section> / <control>", for example
	// "Face buttons / Cross".
	GCharPtr text(g_strdup_printf(_("%s / %s"), section.c_str(), label.c_str()));
	return std::string(text.View());
}

std::string PadSourceText(std::string_view id) {
	const auto* found = std::find_if(std::begin(kSourceTexts), std::end(kSourceTexts),
	                                 [id](const SourceText& text) { return text.id == id; });
	return found != std::end(kSourceTexts) ? _(found->text) : std::string(id);
}

std::string TouchParticleSkinText(core::TouchParticleSkin skin) {
	const auto* found = std::find_if(std::begin(kSkinTexts), std::end(kSkinTexts),
	                                 [skin](const SkinText& text) { return text.skin == skin; });
	if (found != std::end(kSkinTexts)) {
		return _(found->text);
	}
	for (const auto& info: core::TouchParticleSkins()) {
		if (info.skin == skin) {
			return std::string(info.label);
		}
	}
	return {};
}

} // namespace kyty::gtk
