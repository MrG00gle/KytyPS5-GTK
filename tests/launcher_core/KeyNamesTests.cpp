// Example tests of the input mapping editor's key names (src/launcher/core/keyNames.h, task 16.4;
// Requirement 11.14): every name KeyNameFromKeysym gives (all keysyms up to 0xFFFF and a sample of
// Unicode keysyms) is a name the Emulator resolves with SDL_GetKeyFromName; letters, digits,
// Latin-1 and other characters, function keys, modifiers, the keypad, and keys without a name; the
// Emulator's reserved keys.

#include "launcher/core/keyNames.h"
#include "support/noDisplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kyty::core::IsEmulatorKeyName;
using kyty::core::IsReservedKeyName;
using kyty::core::KeyNameFromKeysym;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "KeyNamesTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Hex(uint32_t value) {
	char text[16];
	std::snprintf(text, sizeof(text), "0x%X", value);
	return text;
}

void CheckName(uint32_t keysym, std::string_view expected) {
	const auto name = KeyNameFromKeysym(keysym);
	Check(name == expected,
	      Hex(keysym) + " is '" + std::string(expected) + "', got '" + name + "'");
}

void TestEveryNameResolves() {
	std::size_t named = 0;
	for (uint32_t keysym = 0; keysym <= 0xFFFF; keysym++) {
		const auto name = KeyNameFromKeysym(keysym);
		if (!name.empty()) {
			named++;
			Check(IsEmulatorKeyName(name),
			      "the Emulator resolves '" + name + "' (" + Hex(keysym) + ")");
		}
	}
	// Printable ASCII (95 with Space), Latin-1 from U+00A1 (95), the 44 named keys, and F1-F24.
	Check(named == 95 + 95 + 44 + 24, "named keysyms: " + std::to_string(named));
	std::size_t unicode = 0;
	for (uint32_t code_point = 0; code_point <= 0x10FFFF;
	     code_point += code_point < 0x3000 ? 1 : 97) {
		const auto name = KeyNameFromKeysym(0x01000000 + code_point);
		if (!name.empty()) {
			unicode++;
			Check(IsEmulatorKeyName(name), "the Emulator resolves U+" + Hex(code_point));
		}
	}
	Check(unicode > 10000, "Unicode keysyms are named: " + std::to_string(unicode));
}

void TestCharacters() {
	CheckName('a', "A");
	CheckName('A', "A");
	CheckName('z', "Z");
	CheckName('0', "0");
	CheckName('9', "9");
	CheckName(' ', "Space");
	CheckName(',', ",");
	CheckName('=', "=");
	CheckName('"', "\"");
	CheckName('~', "~");
	CheckName(0xE9, "\xC3\x89");               // e acute: its capital, as Qt::Key_Eacute
	CheckName(0xC9, "\xC3\x89");               // E acute
	CheckName(0xDF, "\xC3\x9F");               // sharp s has no Latin-1 capital
	CheckName(0xF7, "\xC3\xB7");               // division sign
	CheckName(0xFF, "\xC3\xBF");               // y diaeresis: its capital is not Latin-1
	CheckName(0xA1, "\xC2\xA1");               // inverted exclamation mark
	CheckName(0x01000444, "\xD1\x84");         // Cyrillic ef, as typed
	CheckName(0x010020AC, "\xE2\x82\xAC");     // euro sign
	CheckName(0x0101F600, "\xF0\x9F\x98\x80"); // a four-byte character
	CheckName(0x01000061, "A");                // a Unicode keysym of an ASCII letter
}

void TestNamedKeys() {
	for (const auto& [keysym, name]: std::vector<std::pair<uint32_t, std::string_view>> {
	         {0xFF08, "Backspace"},  {0xFF09, "Tab"},         {0xFF0D, "Return"},
	         {0xFF13, "Pause"},      {0xFF14, "ScrollLock"},  {0xFF50, "Home"},
	         {0xFF51, "Left"},       {0xFF52, "Up"},          {0xFF53, "Right"},
	         {0xFF54, "Down"},       {0xFF55, "PageUp"},      {0xFF56, "PageDown"},
	         {0xFF57, "End"},        {0xFF61, "PrintScreen"}, {0xFF63, "Insert"},
	         {0xFF7F, "Numlock"},    {0xFFE5, "CapsLock"},    {0xFFFF, "Delete"},
	         {0xFFE1, "Left Shift"}, {0xFFE2, "Left Shift"},  {0xFFE3, "Left Ctrl"},
	         {0xFFE4, "Left Ctrl"},  {0xFFE9, "Left Alt"},    {0xFFEA, "Left Alt"},
	         {0xFFE7, "Left GUI"},   {0xFFE8, "Left GUI"},    {0xFF8D, "Keypad Enter"},
	         {0xFFAA, "Keypad *"},   {0xFFAB, "Keypad +"},    {0xFFAC, "Keypad ,"},
	         {0xFFAD, "Keypad -"},   {0xFFAE, "Keypad ."},    {0xFFAF, "Keypad /"},
	         {0xFFBD, "Keypad ="},   {0xFFB0, "Keypad 0"},    {0xFFB9, "Keypad 9"},
	         {0xFFBE, "F1"},         {0xFFC9, "F12"},         {0xFFD5, "F24"},
	     }) {
		CheckName(keysym, name);
	}
	for (uint32_t digit = 0; digit <= 9; digit++) {
		CheckName(0xFFB0 + digit, "Keypad " + std::to_string(digit));
	}
	for (uint32_t key = 1; key <= 24; key++) {
		CheckName(0xFFBE + key - 1, "F" + std::to_string(key));
	}
}

void TestUnnamedKeys() {
	for (const uint32_t keysym: {
	         0x0u,        // no symbol
	         0x1Fu,       // a control character
	         0x7Fu,       // delete character
	         0xA0u,       // no-break space
	         0xFF1Bu,     // Escape cancels the capture dialog
	         0xFE50u,     // a dead key
	         0xFE20u,     // ISO_Left_Tab (Shift+Tab; Qt's Key_Backtab had no name)
	         0xFFEBu,     // Super_L
	         0xFF95u,     // KP_Home (keypad with NumLock off)
	         0xFFD6u,     // F25
	         0xFF00u,     // unassigned
	         0x0100D800u, // a surrogate
	         0x01110000u, // beyond U+10FFFF
	         0x20000000u, // not a keysym
	         0x0100001Fu, // a Unicode keysym of a control character
	     }) {
		CheckName(keysym, "");
	}
}

void TestEmulatorNames() {
	for (const std::string_view name: {"A", "a", "Return", "left shift", "Keypad Enter", "F24",
	                                   "Escape", "\xC3\x89", "!", "Numlock"}) {
		Check(IsEmulatorKeyName(name), "'" + std::string(name) + "' is a key name");
	}
	for (const std::string_view name: std::vector<std::string_view> {
	         "", "NotAKey", "Mouse:Left", "Left  Shift", "AB", std::string_view("A\0", 2), " J"}) {
		Check(!IsEmulatorKeyName(name), "'" + std::string(name) + "' is not a key name");
	}
	for (const std::string_view name: {"Escape", "F1", "f1", "F2", "F7", "F8", "F11"}) {
		Check(IsReservedKeyName(name), "'" + std::string(name) + "' is reserved");
	}
	for (const std::string_view name: {"F3", "F6", "F12", "A", "", "NotAKey", "Mouse:Left"}) {
		Check(!IsReservedKeyName(name), "'" + std::string(name) + "' is not reserved");
	}
	Check(IsReservedKeyName(KeyNameFromKeysym(0xFFBE)), "the F1 keysym names a reserved key");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("KeyNamesTests");
	TestEveryNameResolves();
	TestCharacters();
	TestNamedKeys();
	TestUnnamedKeys();
	TestEmulatorNames();
	std::printf("KeyNamesTests: all tests passed\n");
	return 0;
}
