#pragma once

// RapidCheck generators for the configuration properties (Properties 12, 13, and 15-20, tasks
// 13.6-13.14). Header only, and only for property TUs: RapidCheck needs exceptions, which the test
// support library is built without. The generator functions draw values with operator* on
// generators, so call them only inside a property.
//
// Text is valid UTF-8 from Alphabet(): ASCII letters and digits, the characters Kyty.ini and JSON
// quote or escape (, ; = " \ @ # [ ] % ' / space tab), C0 control characters including U+0000,
// U+007F, Latin-1, Cyrillic, CJK, line and paragraph separators, U+FFFD, non-characters, and
// non-BMP characters, plus now and then a uniform draw of any Unicode scalar value. U+FEFF is left
// out everywhere: QSettings (and IniFile, which matches it) drops a U+FEFF that is the first
// character a value's decoder sees, so Qt cannot round-trip it either (review-u5 finding 2; P14's
// exclusion).

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/iniFile.h"
#include "launcher/core/settingsStore.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <rapidcheck.h>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace KytyTests::ConfigGen {

// A uniform choice from [min, max). rc::gen::inRange alone narrows its range at small sizes.
template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

inline bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& PickOf(const std::vector<T>& values) {
	return values[Choose<size_t>(0, values.size())];
}

inline std::string Utf8(char32_t c) {
	std::string out;
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xC0 | (c >> 6u));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else if (c < 0x10000) {
		out += static_cast<char>(0xE0 | (c >> 12u));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	} else {
		out += static_cast<char>(0xF0 | (c >> 18u));
		out += static_cast<char>(0x80 | ((c >> 12u) & 0x3Fu));
		out += static_cast<char>(0x80 | ((c >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (c & 0x3Fu));
	}
	return out;
}

// Characters that are not Unicode White_Space.
inline const std::vector<char32_t>& Visible() {
	static const std::vector<char32_t> chars = {
	    U'a', U'B',   U'z',   U'K',   U'0',   U'7',   U'_',   U'-',   U'.',   U',',    U';',
	    U'=', U'"',   U'\\',  U'@',   U'#',   U'[',   U']',   U'%',   U'\'',  U'/',    U'(',
	    U')', 0x00E9, 0x00FC, 0x0416, 0x30B2, 0x4E2D, 0xFFFD, 0xFFFF, 0xE000, 0x1F3AE, 0x10FFFF};
	return chars;
}

// Every character of Visible(), White_Space characters, and control characters.
inline const std::vector<char32_t>& Alphabet() {
	static const std::vector<char32_t> chars = [] {
		auto all = Visible();
		for (const char32_t c: {U' ', U' ', U'\t', U'\n', U'\r', char32_t {0}, char32_t {0x01},
		                        char32_t {0x1F}, char32_t {0x7F}, char32_t {0x85}, char32_t {0xA0},
		                        char32_t {0x2028}, char32_t {0x2029}, char32_t {0x3000}}) {
			all.push_back(c);
		}
		return all;
	}();
	return chars;
}

// Any Unicode scalar value except U+FEFF.
inline char32_t AnyScalar() {
	while (true) {
		const auto c = Choose<char32_t>(0, 0x110000);
		if ((c < 0xD800 || c > 0xDFFF) && c != 0xFEFF) {
			return c;
		}
	}
}

inline std::string Text(const std::vector<char32_t>& alphabet, int min_length, int max_length) {
	std::string text;
	const int   length = Choose(min_length, max_length + 1);
	for (int i = 0; i < length; i++) {
		text += Utf8(OneIn(10) ? AnyScalar() : PickOf(alphabet));
	}
	return text;
}

// Text that TrimWhitespace leaves non-empty: it starts and ends with a visible character.
inline std::string VisibleText(int max_length) {
	std::string text = Utf8(PickOf(Visible()));
	if (max_length > 1 && OneIn(2)) {
		text += Text(Alphabet(), 0, max_length - 2);
		text += Utf8(PickOf(Visible()));
	}
	return text;
}

inline std::string AnyText(int max_length = 12) {
	return Text(Alphabet(), 0, max_length);
}

inline std::string Whitespace() {
	static const std::vector<std::string> spaces = {"",         "", " ", "  ", "\t", "\xE3\x80\x80",
	                                                "\xC2\xA0 "};
	return PickOf(spaces);
}

// ---- Valid field values

// 1-16 UTF-8 bytes after trimming, with whitespace around now and then.
inline std::string ValidUserName() {
	const auto  limit = Choose<size_t>(1, kyty::core::kMaxUserNameBytes + 1);
	std::string name  = Utf8(PickOf(Visible()));
	while (name.size() > limit) {
		name = Utf8(Choose<char32_t>(U'a', U'z' + 1));
	}
	const int count = Choose(0, 16);
	for (int i = 0; i < count; i++) {
		const auto next = Utf8(OneIn(6) ? char32_t {U' '} : PickOf(Visible()));
		if (name.size() + next.size() > limit) {
			break;
		}
		name += next;
	}
	if (name.back() == ' ') {
		name.back() = 'x';
	}
	return Whitespace() + name + Whitespace();
}

inline int32_t ValidUserId() {
	switch (Choose(0, 4)) {
		case 0: {
			static const std::vector<int32_t> edges = {0,   1,    253,
			                                           256, 1000, kyty::core::kMaxUserId};
			return PickOf(edges);
		}
		default: {
			auto id = Choose<int32_t>(0, kyty::core::kMaxUserId + 1);
			return id == 254 || id == 255 ? 1000 : id;
		}
	}
}

// A non-empty relative path without a ".." component.
inline std::string ValidElf() {
	static const std::vector<std::string> common = {"eboot.bin", "bin/eboot_alt.bin", "./eboot.bin",
	                                                "a..b/c", "dir/.../x.elf"};
	if (OneIn(2)) {
		return PickOf(common);
	}
	std::vector<char32_t> no_slash;
	for (const char32_t c: Alphabet()) {
		if (c != U'/') {
			no_slash.push_back(c);
		}
	}
	std::string elf;
	const int   segments = Choose(1, 4);
	for (int i = 0; i < segments; i++) {
		auto segment = Text(no_slash, 1, 6);
		while (segment.empty() || segment.front() == '/') {
			segment = "x";
		}
		if (segment == "..") {
			segment = "...";
		}
		elf += (i == 0 ? "" : "/") + segment;
	}
	// A uniform draw may have put a '/' into a segment.
	const auto* field = kyty::core::FindGameConfigurationField("elf");
	return field->check(kyty::core::FieldValue(elf)) ? std::string("eboot.bin") : elf;
}

// Entries "Name=Value" with both sides non-empty after trimming; the name has no '='.
inline std::vector<std::string> ValidMapping() {
	std::vector<std::string> mapping;
	const int                count = Choose(0, 5);
	for (int i = 0; i < count; i++) {
		std::string entry_name = VisibleText(6);
		for (auto& c: entry_name) {
			if (c == '=') {
				c = 'e';
			}
		}
		mapping.push_back(Whitespace() + entry_name + Whitespace() + "=" + Whitespace() +
		                  VisibleText(8) + Whitespace());
	}
	return mapping;
}

inline std::string ValidPath(bool required) {
	return required || OneIn(2) ? VisibleText(10) : AnyText(10);
}

// A configuration that Validate() accepts. With paths_required, the three output paths are never
// empty, so that any combination with another valid configuration stays valid.
inline kyty::core::GameConfiguration ValidConfiguration(bool paths_required = false) {
	using kyty::core::LogDirection;
	kyty::core::GameConfiguration c;
	c.screen_resolution  = static_cast<kyty::core::ScreenResolution>(Choose(0, 4));
	c.user_name          = ValidUserName();
	c.user_id            = ValidUserId();
	c.audio_input_device = AnyText();
	c.present_mode       = static_cast<kyty::core::PresentMode>(Choose(0, 3));
	c.gpu_index          = OneIn(3) ? -1 : Choose<int32_t>(-1, std::numeric_limits<int32_t>::max());
	c.fullscreen_enabled = OneIn(2);
	c.hdr_enabled        = OneIn(2);
	c.hide_cursor_enabled         = OneIn(2);
	c.readback_linear_images      = OneIn(2);
	c.readback_tiled_images       = OneIn(2);
	c.sync_raw_image_buffers      = OneIn(2);
	c.tessellation_enabled        = OneIn(2);
	c.trophy_enabled              = OneIn(2);
	c.trophy_duration_seconds     = Choose(5, 11);
	c.perf_overlay_enabled        = OneIn(2);
	c.skip_notice_screen          = OneIn(2);
	c.network_connected           = OneIn(2);
	c.vblank_frequency            = Choose(30, 361);
	c.console_language            = Choose(0, kyty::core::kMaxConsoleLanguage + 1);
	c.vulkan_validation_enabled   = OneIn(2);
	c.shader_validation_enabled   = OneIn(2);
	c.shader_optimization_type    = static_cast<kyty::core::ShaderOptimizationType>(Choose(0, 3));
	c.shader_log_direction        = static_cast<LogDirection>(Choose(0, 3));
	c.command_buffer_dump_enabled = OneIn(2);
	c.printf_direction            = static_cast<LogDirection>(Choose(0, 3));
	c.shader_log_folder = ValidPath(paths_required || c.shader_log_direction == LogDirection::File);
	c.command_buffer_dump_folder = ValidPath(paths_required || c.command_buffer_dump_enabled);
	c.printf_output_file   = ValidPath(paths_required || c.printf_direction == LogDirection::File);
	c.profiler_enabled     = OneIn(2);
	c.renderdoc_enabled    = OneIn(2);
	c.amd_cpu_enabled      = OneIn(2);
	c.elf                  = ValidElf();
	c.input_mapping_custom = OneIn(2);
	c.host_input_mapping   = ValidMapping();
	return c;
}

inline std::string ValidColor() {
	if (OneIn(3)) {
		return {};
	}
	static const std::string digits = "0123456789abcdefABCDEF";
	std::string              color  = "#";
	for (int i = 0; i < 6; i++) {
		color += digits[Choose<size_t>(0, digits.size())];
	}
	return color;
}

inline kyty::core::ControllerSettings ValidController() {
	return {ValidColor(), Choose(0, 101), Choose(0, 101),
	        Choose(0, kyty::core::kMaxKeyboardPlayer + 1)};
}

// ---- Settings_Store state

// Keys outside the schema, in schema groups and others; never "general" in another letter case.
inline const std::vector<std::string>& UnknownKeys() {
	static const std::vector<std::string> keys = {"top_level",
	                                              "Extra/a",
	                                              "Extra/b c",
	                                              "Extra/x%y",
	                                              "Launcher/future_key",
	                                              "MainDialog/other",
	                                              "LauncherWindow/x",
	                                              "GlobalConfiguration/red_zone_protection_enabled",
	                                              "GlobalConfiguration/name",
	                                              "GlobalConfiguration/custom_settings",
	                                              "ConfigurationEditDialog/geometry",
	                                              "GameConfigurations/note",
	                                              "General/legacy"};
	return keys;
}

// Element keys outside the schema.
inline const std::vector<std::string>& UnknownElementKeys() {
	static const std::vector<std::string> keys = {"red_zone_protection_enabled", "future_key",
	                                              "extra/nested"};
	return keys;
}

inline kyty::core::IniValue UnknownValue() {
	switch (Choose(0, 4)) {
		case 0: return kyty::core::IniValue::FromString(AnyText());
		case 1: {
			std::vector<std::string> list;
			const int                size = Choose(0, 4);
			for (int i = 0; i < size; i++) {
				list.push_back(AnyText(6));
			}
			return kyty::core::IniValue::FromStringList(std::move(list));
		}
		case 2: {
			std::string bytes;
			const int   size = Choose(0, 12);
			for (int i = 0; i < size; i++) {
				bytes += static_cast<char>(Choose(0, 256));
			}
			return kyty::core::IniValue::FromByteArray(bytes);
		}
		default: return kyty::core::IniValue::Invalid();
	}
}

// Valid settings: 0-3 Game_Folders, any add-ons folder and view, a grid icon width of 128-320 in
// steps of 16, raw theme and geometry values or none, a window size, a valid Global_Configuration
// (input_mapping_custom false: it is not stored globally) and controller, 0-4 games with distinct
// game paths (absolute, or now and then relative as an old Qt launcher stored them), and unknown
// keys at the top and in the elements.
inline kyty::core::LauncherSettings ValidSettings() {
	kyty::core::LauncherSettings s;
	const int                    folders = Choose(0, 4);
	for (int i = 0; i < folders; i++) {
		s.game_folders.push_back(OneIn(4) ? AnyText() : "/" + AnyText());
	}
	s.addons_dir = AnyText();
	s.grid_view  = OneIn(2);
	s.grid_icon_width =
	    kyty::core::kMinGridIconWidth + kyty::core::kGridIconWidthStep * Choose(0, 13);
	s.check_updates_on_startup = OneIn(2);
	if (!OneIn(4)) {
		s.theme = std::to_string(Choose(0, 3));
	}
	if (OneIn(2)) {
		std::string bytes;
		const int   size = Choose(0, 40);
		for (int i = 0; i < size; i++) {
			bytes += static_cast<char>(Choose(0, 256));
		}
		s.geometry = kyty::core::IniValue::FromByteArray(bytes).Raw();
	}
	s.window                      = {Choose(1, 5000), Choose(1, 5000), OneIn(2)};
	s.global                      = ValidConfiguration();
	s.global.input_mapping_custom = false;
	s.controller                  = ValidController();
	const int games               = Choose(0, 5);
	for (int i = 0; i < games; i++) {
		kyty::core::GameSettingsEntry entry;
		entry.name      = AnyText();
		entry.basedir   = AnyText();
		entry.game_path = (OneIn(5) ? "Legacy/" : "/games/") + std::to_string(i) + "/" + AnyText(6);
		entry.configuration = ValidConfiguration();
		const int unknown   = Choose(0, 3);
		for (int u = 0; u < unknown; u++) {
			entry.unknown.insert_or_assign(PickOf(UnknownElementKeys()), UnknownValue());
		}
		s.games.push_back(std::move(entry));
	}
	const int unknown = Choose(0, 5);
	for (int u = 0; u < unknown; u++) {
		s.unknown.Set(PickOf(UnknownKeys()), UnknownValue());
	}
	return s;
}

// A GameEntry for a stored element (its absolute path, or the legacy relative path it matches).
inline kyty::core::GameEntry EntryFor(const kyty::core::GameSettingsEntry& settings) {
	kyty::core::GameEntry entry;
	if (!settings.game_path.empty() && settings.game_path.front() == '/') {
		entry.path = settings.game_path;
	} else {
		entry.path                 = "/scanned/" + settings.game_path;
		entry.legacy_relative_path = settings.game_path;
	}
	entry.title_name = settings.name;
	return entry;
}

// ---- Game_Settings_File text

// A JSON string literal for valid UTF-8 text.
inline std::string JsonQuote(std::string_view text) {
	static const char kHex[] = "0123456789abcdef";
	std::string       out    = "\"";
	for (const char c: text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (byte < 0x20) {
			out += "\\u00";
			out += kHex[byte >> 4u];
			out += kHex[byte & 0xFu];
		} else {
			out += c;
		}
	}
	return out + "\"";
}

// A title ID that is not empty after trimming, in mixed letter case, with whitespace around now
// and then.
inline std::string TitleId() {
	std::string core;
	if (OneIn(2)) {
		core = OneIn(2) ? "PPSA" : "ppsa";
		for (int i = 0; i < 5; i++) {
			core += static_cast<char>('0' + Choose(0, 10));
		}
	} else {
		core = VisibleText(10);
	}
	return Whitespace() + core + Whitespace();
}

// The JSON text of a field value in one of the forms an import accepts: the text form as a string
// (an enum also as its legacy integer index, a resolution as "1280x720"), a native boolean or
// integer for those kinds, and an array of strings for a string list.
inline std::string EncodeField(const kyty::core::FieldDescriptor& field,
                               const kyty::core::FieldValue&      value) {
	using kyty::core::FieldKind;
	switch (field.kind) {
		case FieldKind::Bool:
			return OneIn(2) ? field.format(value) : JsonQuote(field.format(value));
		case FieldKind::Int: return OneIn(2) ? field.format(value) : JsonQuote(field.format(value));
		case FieldKind::Enum: {
			const auto index = std::get<int64_t>(value);
			if (OneIn(3)) {
				return JsonQuote(std::to_string(index));
			}
			if (field.name == "screen_resolution" && OneIn(2)) {
				std::string legacy = field.format(value).substr(1);
				for (auto& c: legacy) {
					if (c == 'X') {
						c = 'x';
					}
				}
				return JsonQuote(legacy);
			}
			return JsonQuote(field.format(value));
		}
		case FieldKind::String: return JsonQuote(std::get<std::string>(value));
		case FieldKind::StringList: {
			std::string out = "[";
			for (const auto& element: std::get<std::vector<std::string>>(value)) {
				out += (out.size() > 1 ? ", " : "") + JsonQuote(element);
			}
			return out + "]";
		}
	}
	return {};
}

// A JSON object from member keys and JSON texts.
inline std::string JsonObject(const std::vector<std::pair<std::string, std::string>>& members) {
	std::string out = "{";
	for (const auto& [key, json]: members) {
		out += (out.size() > 1 ? ", " : "") + JsonQuote(key) + ": " + json;
	}
	return out + "}";
}

// ---- Invalid values

// Values that a field's text form never reads as a valid value of the field, as raw INI values:
// unparsable text for each kind, plus the field's rule breakers (Requirement 9.4 and the
// gpu_index, console_language, and elf rules).
inline kyty::core::IniValue InvalidStoredValue(std::string_view field, kyty::core::FieldKind kind) {
	using kyty::core::FieldKind;
	using kyty::core::IniValue;
	const auto pick = [](const std::vector<std::string>& texts) {
		return IniValue::FromString(PickOf(texts));
	};
	if (field == "user_id" && OneIn(2)) {
		return pick({"-1", "254", "255", "2147482880"});
	}
	if (field == "vblank_frequency" && OneIn(2)) {
		return pick({"29", "361", "0"});
	}
	if (field == "trophy_duration_seconds" && OneIn(2)) {
		return pick({"4", "11"});
	}
	if (field == "gpu_index" && OneIn(2)) {
		return pick({"-2", "2147483648"});
	}
	if (field == "console_language" && OneIn(2)) {
		return pick({"-1", "30"});
	}
	if (field == "user_name" && OneIn(2)) {
		return pick({"", "   ", "\xE3\x80\x80", "0123456789abcdefg",
		             "\xC3\xA9\xC3\xA9\xC3\xA9\xC3"
		             "\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
		             "\xC3\xA9x"});
	}
	if (field == "elf" && OneIn(2)) {
		return pick({"", "/eboot.bin", "..", "../eboot.bin", "bin/../eboot.bin"});
	}
	if ((field == "controller_color") && OneIn(2)) {
		return pick({"red", "#12345", "#1234567", "#12345g", "0070d1"});
	}
	switch (kind) {
		case FieldKind::Bool: return pick({"maybe", "TRUE", "1", "", "yes"});
		case FieldKind::Int: return pick({"abc", "1.5", "", "+5", "0x10", "99999999999999999999"});
		case FieldKind::Enum: return pick({"Foo", "", "-1", "9", "r1280x720", "mailbox"});
		case FieldKind::String:
		case FieldKind::StringList: break;
	}
	// Not a string (String) or not a list (StringList).
	if (kind == FieldKind::String && OneIn(2)) {
		return IniValue::FromStringList({"a", "b"});
	}
	return IniValue::FromByteArray("x");
}

} // namespace KytyTests::ConfigGen
