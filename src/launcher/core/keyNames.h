#pragma once

// Key names for the input mapping editor's keyboard capture (task 16.4; Requirement 11.14; plan
// D24). A keyboard binding is stored as "<control>=<key name>" and the Emulator resolves the name
// with SDL_GetKeyFromName (src/graphics/presentation/window/hostInput.cpp), so the names are SDL
// key names. Ported from the Qt launcher's KeyName and KeypadName
// (src/launcher/src/inputMappingDialog.cpp), keyed by X11 keysyms, which are GDK keyvals.

#include <cstdint>
#include <string>
#include <string_view>

namespace kyty::core {

// The name the Emulator knows the key by, or "" when the editor does not bind the key:
// - printable ASCII: the character, letters upper-cased ("A" for a and A), "Space" for space;
// - Latin-1 characters (keysyms 0xA1-0xFF) and Unicode keysyms (0x01000000 + code point): the
//   character in UTF-8, Latin-1 letters upper-cased as Qt named them (SDL resolves a shifted
//   character to its unshifted key);
// - Return, Backspace, Tab, Insert, Delete, Home, End, PageUp, PageDown, the arrows (Left, Right,
//   Up, Down), CapsLock, Numlock, ScrollLock, Pause, PrintScreen, F1-F24;
// - either Shift, Control, Alt, or Meta key: "Left Shift", "Left Ctrl", "Left Alt", "Left GUI"
//   (the Emulator treats the right-hand keys as the left-hand ones);
// - the keypad digits and operators with NumLock on: "Keypad 0"-"Keypad 9", "Keypad Enter",
//   "Keypad /", "Keypad *", "Keypad -", "Keypad +", "Keypad .", "Keypad =", "Keypad ,".
// Escape (the capture dialog's cancel key), dead keys, and other function keys give "". Qt refused
// a character key pressed with a modifier; the capture dialog keeps that rule, because a keysym
// does not say which modifiers were held.
std::string KeyNameFromKeysym(uint32_t keysym);

// True when the Emulator resolves name to a key (SDL_GetKeyFromName, which needs no SDL_Init);
// false for "" and for a name with a NUL byte.
bool IsEmulatorKeyName(std::string_view name);

// True for a name of a key the Emulator reserves (Escape, F1, F2, F7, F8, F11): it refuses a
// mapping that binds one, and the capture dialog says "That key is reserved by the emulator.".
bool IsReservedKeyName(std::string_view name);

} // namespace kyty::core
