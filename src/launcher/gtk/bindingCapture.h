#pragma once

// The capture rules of the input mapping editor (task 21.2; HC-28; Requirement 11.14; plan D41),
// ported from the Qt launcher's InputCaptureDialog and PadCaptureDialog
// (src/launcher/src/inputMappingDialog.cpp). They take no widget and need no display, so that
// tests/launcher_gtk/BindingCaptureTests.cpp covers them; bindingCaptureDialog.cpp applies them to
// its key, mouse button, and controller events.

#include <cstdint>
#include <gdk/gdk.h>
#include <optional>
#include <set>
#include <string>

namespace kyty::gtk {

enum class KeyCapture {
	Bound,       // name is the key name to bind
	Cancelled,   // Escape: the capture ends without a binding
	Unsupported, // the Emulator has no name for the key ("That key is not supported.")
	Reserved,    // F1, F2, F7, F8, F11 ("That key is reserved by the emulator.")
	Modified,    // a character key pressed with Ctrl, Alt, or Super
};

struct KeyCaptureResult {
	KeyCapture  outcome = KeyCapture::Unsupported;
	std::string name; // Bound: core::KeyNameFromKeysym's name
};

// A key press in the capture dialog: keyval is the GDK keyval (an X11 keysym), state the modifiers
// held. Escape cancels whatever the modifiers. Otherwise the key's name comes from
// core::KeyNameFromKeysym: no name, or a name the Emulator does not resolve
// (core::IsEmulatorKeyName), is Unsupported; a name core::IsReservedKeyName accepts is Reserved; a
// character key (a name of one character: letters, digits, punctuation, other characters) pressed
// with Ctrl, Alt, Super, Hyper, or Meta is Modified, because the keyval does not say which key
// the user meant without them (the Qt launcher refused such keys as well). Shift and Caps Lock
// change nothing. Named keys (Return, Space, the arrows, the keypad, F-keys) and the modifier keys
// themselves are bound whatever is held.
KeyCaptureResult ClassifyCapturedKey(uint32_t keyval, GdkModifierType state);

// A mouse button press in the capture dialog, by GDK button number: 1 (primary) "Mouse:Left",
// 2 (middle) "Mouse:Middle", 3 (secondary) "Mouse:Right", 8 (back) "Mouse:X1", 9 (forward)
// "Mouse:X2", as the Qt launcher's LeftButton, MiddleButton, RightButton, BackButton, and
// ForwardButton; nullopt for every other button (the press is ignored).
std::optional<std::string> MouseButtonBinding(unsigned button);

// Gamepad capture: inputs that are already held when the capture starts count only after they
// were released (the Qt launcher's rule), so that the button that opened the capture is not bound.
class PadCapture {
public:
	explicit PadCapture(const std::set<std::string>& held = {}): m_held(held) {}

	// A new controller state with these pressed pad sources (ControllerState::raw_pressed): the
	// first core::PadSources() id, in that table's order, that is pressed and no longer held, or
	// nullopt. Released inputs stop being held; ids that are not pad sources are ignored.
	std::optional<std::string> Update(const std::set<std::string>& pressed);

private:
	std::set<std::string> m_held;
};

} // namespace kyty::gtk
