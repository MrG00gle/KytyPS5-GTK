// Example tests of the input mapping editor's capture rules (src/launcher/gtk/bindingCapture.h,
// task 21.2, HC-28): Escape cancels; keys without an Emulator name are unsupported; F1, F2, F7,
// F8, and F11 are reserved; character keys with Ctrl, Alt, or Super are refused while Shift and
// Caps Lock change nothing; named keys and the modifier keys themselves are bound whatever is
// held; mouse buttons 1, 2, 3, 8, and 9 are Mouse:Left, Middle, Right, X1, and X2 (the Qt
// launcher's LeftButton, MiddleButton, RightButton, BackButton, ForwardButton); every bound input
// is one that core::Assign accepts. Pad capture ignores inputs held when it starts until they are
// released. The editor's texts (inputLabels.h) cover every control, pad source, and skin of
// core/inputMapping.h. No display and no gtk_init.

#include "launcher/core/inputMapping.h"
#include "launcher/gtk/bindingCapture.h"
#include "launcher/gtk/inputLabels.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <gdk/gdk.h>
#include <optional>
#include <set>
#include <string>

namespace {

namespace core = kyty::core;
using kyty::gtk::ClassifyCapturedKey;
using kyty::gtk::KeyCapture;
using kyty::gtk::MouseButtonBinding;
using kyty::gtk::PadCapture;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "BindingCaptureTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

const char* OutcomeName(KeyCapture outcome) {
	switch (outcome) {
		case KeyCapture::Bound: return "Bound";
		case KeyCapture::Cancelled: return "Cancelled";
		case KeyCapture::Unsupported: return "Unsupported";
		case KeyCapture::Reserved: return "Reserved";
		case KeyCapture::Modified: return "Modified";
	}
	return "?";
}

GdkModifierType Mods(unsigned mask) {
	return static_cast<GdkModifierType>(mask);
}

// The model's keyboard binding of control, or "(none)".
std::string Binding(const core::InputMappingModel& model, const std::string& control) {
	const auto found = model.keyboard.find(control);
	return found != model.keyboard.end() ? found->second : "(none)";
}

void ExpectOutcome(uint32_t keyval, unsigned state, KeyCapture expected, const std::string& what) {
	const auto result = ClassifyCapturedKey(keyval, Mods(state));
	Check(result.outcome == expected, what + ": expected " + OutcomeName(expected) + ", got " +
	                                      OutcomeName(result.outcome) + " (\"" + result.name +
	                                      "\")");
	if (expected != KeyCapture::Bound) {
		Check(result.name.empty(), what + ": a refusal carries no name");
	}
}

// Bound to name, and core::Assign takes it for a keyboard binding.
void ExpectBound(uint32_t keyval, unsigned state, const std::string& name,
                 const std::string& what) {
	const auto result = ClassifyCapturedKey(keyval, Mods(state));
	Check(result.outcome == KeyCapture::Bound && result.name == name,
	      what + ": expected Bound \"" + name + "\", got " + OutcomeName(result.outcome) + " \"" +
	          result.name + "\"");
	core::InputMappingModel model;
	Check(core::Assign(model, core::InputDevice::KeyboardMouse, "Cross", result.name) &&
	          model.keyboard_custom && Binding(model, "Cross") == name,
	      what + ": core::Assign binds \"" + name + "\"");
}

void TestEscapeCancels() {
	ExpectOutcome(GDK_KEY_Escape, 0, KeyCapture::Cancelled, "Escape");
	ExpectOutcome(GDK_KEY_Escape, GDK_CONTROL_MASK | GDK_SHIFT_MASK, KeyCapture::Cancelled,
	              "Ctrl+Shift+Escape");
}

void TestUnsupportedKeys() {
	ExpectOutcome(GDK_KEY_dead_acute, 0, KeyCapture::Unsupported, "a dead key");
	ExpectOutcome(GDK_KEY_Menu, 0, KeyCapture::Unsupported, "the Menu key");
	ExpectOutcome(GDK_KEY_VoidSymbol, 0, KeyCapture::Unsupported, "VoidSymbol");
	ExpectOutcome(GDK_KEY_Super_L, 0, KeyCapture::Unsupported, "Super_L (no SDL name in the core)");
	ExpectOutcome(GDK_KEY_ISO_Level3_Shift, 0, KeyCapture::Unsupported, "AltGr (ISO_Level3_Shift)");
	ExpectOutcome(0, 0, KeyCapture::Unsupported, "keyval 0");
}

void TestReservedKeys() {
	for (const uint32_t key: {GDK_KEY_F1, GDK_KEY_F2, GDK_KEY_F7, GDK_KEY_F8, GDK_KEY_F11}) {
		ExpectOutcome(key, 0, KeyCapture::Reserved,
		              "F-key " + std::to_string(key - GDK_KEY_F1 + 1));
		ExpectOutcome(key, GDK_CONTROL_MASK, KeyCapture::Reserved,
		              "Ctrl+F-key " + std::to_string(key - GDK_KEY_F1 + 1));
	}
	ExpectBound(GDK_KEY_F3, 0, "F3", "F3 is not reserved");
	ExpectBound(GDK_KEY_F6, 0, "F6", "F6 (the touch pad mode key's default) is not reserved");
	ExpectBound(GDK_KEY_F12, GDK_ALT_MASK, "F12", "Alt+F12: named keys ignore the modifiers");
}

void TestModifiedCharacterKeys() {
	struct Case {
		uint32_t    keyval;
		unsigned    state;
		const char* what;
	};
	const Case refused[] = {
	    {GDK_KEY_a, GDK_CONTROL_MASK, "Ctrl+a"},
	    {GDK_KEY_A, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Ctrl+Shift+A"},
	    {GDK_KEY_1, GDK_ALT_MASK, "Alt+1"},
	    {GDK_KEY_slash, GDK_SUPER_MASK, "Super+/"},
	    {GDK_KEY_bracketleft, GDK_META_MASK, "Meta+["},
	    {GDK_KEY_semicolon, GDK_HYPER_MASK, "Hyper+;"},
	    {GDK_KEY_eacute, GDK_CONTROL_MASK, "Ctrl+é (Latin-1)"},
	};
	for (const auto& c: refused) {
		ExpectOutcome(c.keyval, c.state, KeyCapture::Modified, c.what);
	}
	ExpectBound(GDK_KEY_a, 0, "A", "a");
	ExpectBound(GDK_KEY_a, GDK_SHIFT_MASK, "A", "Shift+a");
	ExpectBound(GDK_KEY_A, GDK_SHIFT_MASK | GDK_LOCK_MASK, "A", "Shift+A with Caps Lock");
	ExpectBound(GDK_KEY_1, 0, "1", "1");
	ExpectBound(GDK_KEY_slash, 0, "/", "/");
	ExpectBound(GDK_KEY_j, GDK_BUTTON1_MASK, "J", "j with a mouse button held");
}

void TestNamedKeysWithModifiers() {
	ExpectBound(GDK_KEY_Return, GDK_CONTROL_MASK, "Return", "Ctrl+Return");
	ExpectBound(GDK_KEY_space, GDK_CONTROL_MASK, "Space", "Ctrl+Space");
	ExpectBound(GDK_KEY_Up, GDK_ALT_MASK, "Up", "Alt+Up");
	ExpectBound(GDK_KEY_BackSpace, GDK_SUPER_MASK, "Backspace", "Super+BackSpace");
	ExpectBound(GDK_KEY_Tab, 0, "Tab", "Tab");
	ExpectBound(GDK_KEY_KP_1, GDK_CONTROL_MASK, "Keypad 1", "Ctrl+KP_1");
	ExpectBound(GDK_KEY_KP_Enter, GDK_ALT_MASK, "Keypad Enter", "Alt+KP_Enter");
}

void TestModifierKeysAlone() {
	ExpectBound(GDK_KEY_Control_L, GDK_CONTROL_MASK, "Left Ctrl", "Control_L (state with Ctrl)");
	ExpectBound(GDK_KEY_Control_R, 0, "Left Ctrl", "Control_R");
	ExpectBound(GDK_KEY_Shift_L, GDK_SHIFT_MASK, "Left Shift", "Shift_L");
	ExpectBound(GDK_KEY_Alt_L, GDK_ALT_MASK, "Left Alt", "Alt_L (state with Alt)");
	ExpectBound(GDK_KEY_Meta_L, GDK_META_MASK, "Left GUI", "Meta_L");
}

void TestMouseButtons() {
	const struct {
		unsigned    button;
		const char* binding;
	} kButtons[] = {
	    {1, "Mouse:Left"}, {2, "Mouse:Middle"}, {3, "Mouse:Right"},
	    {8, "Mouse:X1"},   {9, "Mouse:X2"},
	};
	for (const auto& [button, binding]: kButtons) {
		const auto name = MouseButtonBinding(button);
		Check(name && *name == binding,
		      "mouse button " + std::to_string(button) + " is " + binding);
		Check(core::IsMouseButtonName(*name), std::string(binding) + " is a mouse button name");
		core::InputMappingModel model;
		Check(core::Assign(model, core::InputDevice::KeyboardMouse, "R2", *name) &&
		          Binding(model, "R2") == binding,
		      std::string("core::Assign binds R2=") + binding);
	}
	for (const unsigned button: {0U, 4U, 5U, 6U, 7U, 10U, 99U}) {
		Check(!MouseButtonBinding(button),
		      "mouse button " + std::to_string(button) + " is ignored");
	}
	Check(GDK_BUTTON_PRIMARY == 1 && GDK_BUTTON_MIDDLE == 2 && GDK_BUTTON_SECONDARY == 3,
	      "GDK's button numbers");
}

void TestPadCapture() {
	PadCapture capture({"South"});
	Check(!capture.Update({"South"}), "an input held at the start does not count");
	Check(capture.Update({"South", "East"}) == std::optional<std::string>("East"),
	      "a new press while the held one stays down counts");
	Check(!capture.Update({}), "nothing pressed: nothing captured");
	Check(capture.Update({"South"}) == std::optional<std::string>("South"),
	      "the held input counts after it was released and pressed again");

	PadCapture fresh;
	Check(!fresh.Update({"Foo", "LeftStickUp"}), "ids that are not pad sources are ignored");
	Check(fresh.Update({"North", "East"}) == std::optional<std::string>("East"),
	      "two new presses: the first in core::PadSources() order");
	Check(fresh.Update({"RightTrigger"}) == std::optional<std::string>("RightTrigger"),
	      "triggers are pad sources");

	PadCapture both({"Back", "Guide"});
	Check(!both.Update({"Back", "Guide"}), "inputs held at the start never count while held");
	Check(!both.Update({"Back"}), "one of them released, the other still held: nothing new");
	Check(both.Update({"Back", "Guide"}) == std::optional<std::string>("Guide"),
	      "the released one counts when it is pressed again; the one still held does not");

	for (const auto& source: core::PadSources()) {
		PadCapture              one;
		const auto              id = one.Update({std::string(source.id)});
		core::InputMappingModel model;
		Check(id && core::Assign(model, core::InputDevice::Pad, "Cross", *id),
		      "every pad source can be captured and assigned: " + std::string(source.id));
	}
}

// Without translations (the C locale, no bound domain) every text is its msgid, which the tables
// keep equal to the core's English labels: so every control, pad source, and skin has an entry.
void TestEditorTexts() {
	for (const auto& control: core::InputControls()) {
		Check(kyty::gtk::InputControlText(control, core::InputDevice::Pad) == control.label,
		      "Gamepad label of " + std::string(control.id));
		Check(kyty::gtk::InputControlText(control, core::InputDevice::KeyboardMouse) ==
		          control.keyboard_label,
		      "Keyboard and mouse label of " + std::string(control.id));
		const std::string title =
		    kyty::gtk::InputControlTitle(control, core::InputDevice::KeyboardMouse);
		Check(title == kyty::gtk::InputSectionText(control.section) + " / " +
		                   std::string(control.keyboard_label),
		      "binding title of " + std::string(control.id) + ": " + title);
	}
	for (const auto section:
	     {core::InputSection::Face, core::InputSection::Dpad, core::InputSection::Sticks,
	      core::InputSection::System, core::InputSection::LeftStick, core::InputSection::RightStick,
	      core::InputSection::Shortcuts}) {
		Check(kyty::gtk::InputSectionText(section) == core::InputSectionTitle(section),
		      "section title " + std::string(core::InputSectionTitle(section)));
	}
	Check(kyty::gtk::InputSectionText(core::InputSection::Shoulders) == "Shoulders and triggers",
	      "the shoulders section is worded without an ampersand");
	for (const auto& source: core::PadSources()) {
		Check(kyty::gtk::PadSourceText(source.id) == source.label,
		      "pad source label of " + std::string(source.id));
	}
	Check(kyty::gtk::PadSourceText("Unknown9") == "Unknown9", "an unknown pad source as it is");
	for (const auto& skin: core::TouchParticleSkins()) {
		Check(kyty::gtk::TouchParticleSkinText(skin.skin) == skin.label,
		      "skin label of " + std::string(skin.key));
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("BindingCaptureTests");
	// A GLib critical or warning fails the test.
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestEscapeCancels();
	TestUnsupportedKeys();
	TestReservedKeys();
	TestModifiedCharacterKeys();
	TestNamedKeysWithModifiers();
	TestModifierKeysAlone();
	TestMouseButtons();
	TestPadCapture();
	TestEditorTexts();
	std::printf("BindingCaptureTests: all checks passed\n");
	return 0;
}
