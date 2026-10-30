#include "launcher/core/keyNames.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <array>
#include <optional>

namespace kyty::core {

namespace {

struct NamedKey {
	uint32_t         keysym;
	std::string_view name;
};

// X11 keysyms (keysymdef.h) of the non-character keys the Qt editor bound.
constexpr std::array kNamedKeys = {
    NamedKey {0xFF08, "Backspace"},    NamedKey {0xFF09, "Tab"},
    NamedKey {0xFF0D, "Return"},       NamedKey {0xFF13, "Pause"},
    NamedKey {0xFF14, "ScrollLock"},   NamedKey {0xFF50, "Home"},
    NamedKey {0xFF51, "Left"},         NamedKey {0xFF52, "Up"},
    NamedKey {0xFF53, "Right"},        NamedKey {0xFF54, "Down"},
    NamedKey {0xFF55, "PageUp"},       NamedKey {0xFF56, "PageDown"},
    NamedKey {0xFF57, "End"},          NamedKey {0xFF61, "PrintScreen"},
    NamedKey {0xFF63, "Insert"},       NamedKey {0xFF7F, "Numlock"},
    NamedKey {0xFF8D, "Keypad Enter"}, NamedKey {0xFFAA, "Keypad *"},
    NamedKey {0xFFAB, "Keypad +"},     NamedKey {0xFFAC, "Keypad ,"},
    NamedKey {0xFFAD, "Keypad -"},     NamedKey {0xFFAE, "Keypad ."},
    NamedKey {0xFFAF, "Keypad /"},     NamedKey {0xFFB0, "Keypad 0"},
    NamedKey {0xFFB1, "Keypad 1"},     NamedKey {0xFFB2, "Keypad 2"},
    NamedKey {0xFFB3, "Keypad 3"},     NamedKey {0xFFB4, "Keypad 4"},
    NamedKey {0xFFB5, "Keypad 5"},     NamedKey {0xFFB6, "Keypad 6"},
    NamedKey {0xFFB7, "Keypad 7"},     NamedKey {0xFFB8, "Keypad 8"},
    NamedKey {0xFFB9, "Keypad 9"},     NamedKey {0xFFBD, "Keypad ="},
    NamedKey {0xFFE1, "Left Shift"},   NamedKey {0xFFE2, "Left Shift"},
    NamedKey {0xFFE3, "Left Ctrl"},    NamedKey {0xFFE4, "Left Ctrl"},
    NamedKey {0xFFE5, "CapsLock"},     NamedKey {0xFFE7, "Left GUI"},
    NamedKey {0xFFE8, "Left GUI"},     NamedKey {0xFFE9, "Left Alt"},
    NamedKey {0xFFEA, "Left Alt"},     NamedKey {0xFFFF, "Delete"},
};

constexpr uint32_t kFirstFunctionKey  = 0xFFBE; // XK_F1
constexpr uint32_t kFunctionKeyCount  = 24;     // up to XK_F24
constexpr uint32_t kUnicodeKeysymBase = 0x01000000;
constexpr uint32_t kMaxCodePoint      = 0x10FFFF;

// The UTF-8 encoding of a code point from U+0080 (CharacterName names ASCII itself).
std::string Utf8(uint32_t code_point) {
	std::string text;
	if (code_point < 0x800) {
		text += static_cast<char>(0xC0 | (code_point >> 6u));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else if (code_point < 0x10000) {
		text += static_cast<char>(0xE0 | (code_point >> 12u));
		text += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else {
		text += static_cast<char>(0xF0 | (code_point >> 18u));
		text += static_cast<char>(0x80 | ((code_point >> 12u) & 0x3Fu));
		text += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		text += static_cast<char>(0x80 | (code_point & 0x3Fu));
	}
	return text;
}

// The name of a character key, as Qt's QKeySequence named the key of a character.
std::string CharacterName(uint32_t code_point) {
	if (code_point == ' ') {
		return "Space";
	}
	if (code_point >= 'a' && code_point <= 'z') {
		return std::string(1, static_cast<char>(code_point - 'a' + 'A'));
	}
	if (code_point > ' ' && code_point < 0x7F) {
		return std::string(1, static_cast<char>(code_point));
	}
	if (code_point <= 0xA0 || (code_point >= 0xD800 && code_point <= 0xDFFF) ||
	    code_point > kMaxCodePoint) {
		return {}; // control characters, no-break space, surrogates
	}
	// Latin-1 small letters (except U+00F7 and U+00FF, whose capitals are not Latin-1 or do not
	// exist) are named by their capital, as Qt::Key_Agrave ... Qt::Key_Thorn.
	if (code_point >= 0xE0 && code_point <= 0xFE && code_point != 0xF7) {
		return Utf8(code_point - 0x20);
	}
	return Utf8(code_point);
}

std::optional<SDL_Keycode> KeyFromName(std::string_view name) {
	if (name.empty() || name.find('\0') != std::string_view::npos) {
		return std::nullopt;
	}
	const auto key = SDL_GetKeyFromName(std::string(name).c_str());
	return key != SDLK_UNKNOWN ? std::optional(key) : std::nullopt;
}

} // namespace

std::string KeyNameFromKeysym(uint32_t keysym) {
	if (keysym >= kFirstFunctionKey && keysym < kFirstFunctionKey + kFunctionKeyCount) {
		return "F" + std::to_string(keysym - kFirstFunctionKey + 1);
	}
	for (const auto& key: kNamedKeys) {
		if (key.keysym == keysym) {
			return std::string(key.name);
		}
	}
	if (keysym <= 0xFF) {
		return CharacterName(keysym); // Latin-1 keysyms are their code points
	}
	if (keysym >= kUnicodeKeysymBase && keysym - kUnicodeKeysymBase <= kMaxCodePoint) {
		return CharacterName(keysym - kUnicodeKeysymBase);
	}
	return {};
}

bool IsEmulatorKeyName(std::string_view name) {
	return KeyFromName(name).has_value();
}

bool IsReservedKeyName(std::string_view name) {
	const auto key = KeyFromName(name);
	if (!key) {
		return false;
	}
	switch (*key) {
		case SDLK_ESCAPE:
		case SDLK_F1:
		case SDLK_F2:
		case SDLK_F7:
		case SDLK_F8:
		case SDLK_F11: return true;
		default: return false;
	}
}

} // namespace kyty::core
