#include "launcher/core/text.h"

#include <SDL3/SDL_stdinc.h>
#include <cstddef>
#include <cstdint>

namespace kyty::core {

namespace {

struct Decoded {
	char32_t code_point = 0;
	size_t   length     = 0; // bytes of the valid sequence, or 0 when it is ill-formed
	size_t   invalid    = 0; // bytes of the maximal subpart when it is ill-formed
};

// Decodes the UTF-8 sequence at text[0] (text is not empty), following the well-formed byte
// sequences of the Unicode Standard, table 3-7.
Decoded DecodeFirst(std::string_view text) {
	const auto    byte = [&](size_t index) { return static_cast<uint8_t>(text[index]); };
	const uint8_t lead = byte(0);
	if (lead < 0x80) {
		return {lead, 1, 0};
	}
	size_t  length     = 0;
	uint8_t second_min = 0x80;
	uint8_t second_max = 0xBF;
	if (lead >= 0xC2 && lead <= 0xDF) {
		length = 2;
	} else if (lead == 0xE0) {
		length     = 3;
		second_min = 0xA0;
	} else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
		length = 3;
	} else if (lead == 0xED) {
		length     = 3;
		second_max = 0x9F;
	} else if (lead == 0xF0) {
		length     = 4;
		second_min = 0x90;
	} else if (lead >= 0xF1 && lead <= 0xF3) {
		length = 4;
	} else if (lead == 0xF4) {
		length     = 4;
		second_max = 0x8F;
	} else {
		// A continuation byte, C0, C1, or F5-FF: never part of a well-formed sequence.
		return {0, 0, 1};
	}

	char32_t code_point = lead & (0xFF >> (length + 1));
	for (size_t i = 1; i < length; i++) {
		const uint8_t min = i == 1 ? second_min : 0x80;
		const uint8_t max = i == 1 ? second_max : 0xBF;
		if (i >= text.size() || byte(i) < min || byte(i) > max) {
			return {0, 0, i};
		}
		code_point = (code_point << 6) | (byte(i) & 0x3F);
	}
	return {code_point, length, 0};
}

bool IsWhiteSpace(char32_t c) {
	return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
	       (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
	       c == 0x205F || c == 0x3000;
}

// The byte length of the whitespace code point that ends text, or 0. White_Space code points take
// one to three bytes, and a lead byte is never a continuation byte, so each candidate start is
// unambiguous.
size_t TrailingWhiteSpaceLength(std::string_view text) {
	for (size_t length = 1; length <= 3 && length <= text.size(); length++) {
		const auto tail    = text.substr(text.size() - length);
		const auto decoded = DecodeFirst(tail);
		if (decoded.length == length) {
			return IsWhiteSpace(decoded.code_point) ? length : 0;
		}
	}
	return 0;
}

} // namespace

std::string_view TrimWhitespace(std::string_view text) {
	while (!text.empty()) {
		const auto decoded = DecodeFirst(text);
		if (decoded.length == 0 || !IsWhiteSpace(decoded.code_point)) {
			break;
		}
		text.remove_prefix(decoded.length);
	}
	while (!text.empty()) {
		const size_t length = TrailingWhiteSpaceLength(text);
		if (length == 0) {
			break;
		}
		text.remove_suffix(length);
	}
	return text;
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) {
		return true;
	}
	const std::string haystack_text(haystack);
	const std::string needle_text(needle);
	return SDL_strcasestr(haystack_text.c_str(), needle_text.c_str()) != nullptr;
}

int CompareCaseInsensitive(std::string_view a, std::string_view b) {
	const std::string a_text(a);
	const std::string b_text(b);
	const int         result = SDL_strcasecmp(a_text.c_str(), b_text.c_str());
	return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

std::string SanitizeUtf8(std::string_view bytes) {
	std::string result;
	result.reserve(bytes.size());
	while (!bytes.empty()) {
		const auto decoded = DecodeFirst(bytes);
		if (decoded.length != 0) {
			result.append(bytes.substr(0, decoded.length));
			bytes.remove_prefix(decoded.length);
		} else {
			result.append("\xEF\xBF\xBD");
			bytes.remove_prefix(decoded.invalid);
		}
	}
	return result;
}

} // namespace kyty::core
