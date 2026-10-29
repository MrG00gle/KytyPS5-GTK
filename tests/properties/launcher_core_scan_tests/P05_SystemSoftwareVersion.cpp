// Feature: linux-gtk-modernization, Property 5: System software version formatting
//
// Validates Requirement 7.8. Half of the cases encode a valid requiredSystemSoftwareVersion ("0x"
// or "0X" as in the Qt launcher, six decimal digits MMmmpp with pp = 00 half of the time, ten hex
// digits of either case), the other half break one in some way or use arbitrary text
// (scanGenerators.h). Both FormatSystemSoftwareVersion and ParseParamJson (where the value is
// trimmed first) must give int(MM).mm, plus .pp when pp is not 00, for the valid form and "" for
// anything else; a character-by-character reference decides which strings have the valid form.

#include "launcher/core/gameMetadata.h"
#include "launcher/core/version.h"
#include "support/property.h"
#include "support/scanGenerators.h"

#include <string>

namespace {

namespace ScanGen = KytyTests::ScanGen;

bool IsDecimal(char c) {
	return c >= '0' && c <= '9';
}

bool IsHex(char c) {
	return IsDecimal(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string Reference(const std::string& raw) {
	if (raw.size() != 18 || raw[0] != '0' || (raw[1] != 'x' && raw[1] != 'X')) {
		return "";
	}
	for (size_t i = 2; i < 8; i++) {
		if (!IsDecimal(raw[i])) {
			return "";
		}
	}
	for (size_t i = 8; i < 18; i++) {
		if (!IsHex(raw[i])) {
			return "";
		}
	}
	const int major = (raw[2] - '0') * 10 + (raw[3] - '0');
	auto      text  = std::to_string(major) + "." + raw.substr(4, 2);
	if (raw.substr(6, 2) != "00") {
		text += "." + raw.substr(6, 2);
	}
	return text;
}

// ASCII whitespace trimmed from both ends; the generated text is ASCII apart from the byte 0xC3, so
// this equals TrimWhitespace on every input whose JSON parses.
std::string TrimAscii(const std::string& text) {
	const char* space = " \t\n\v\f\r";
	const auto  begin = text.find_first_not_of(space);
	if (begin == std::string::npos) {
		return "";
	}
	return text.substr(begin, text.find_last_not_of(space) - begin + 1);
}

// The JSON string literal for text (quotes, backslashes, and control characters escaped).
std::string JsonString(const std::string& text) {
	std::string out = "\"";
	for (const char c: text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			static const char* digits = "0123456789abcdef";
			out += "\\u00";
			out += digits[(c >> 4) & 0xF];
			out += digits[c & 0xF];
		} else {
			out += c;
		}
	}
	return out + "\"";
}

} // namespace

KYTY_PROPERTY(5, "System software version formatting") {
	return rc::check(name, []() {
		const bool  valid = ScanGen::OneIn(2);
		std::string raw;
		std::string expected;
		if (valid) {
			const auto version = ScanGen::ValidSystemSoftwareVersion();
			raw                = version.raw;
			expected           = version.expected;
			RC_ASSERT(Reference(raw) == expected);
		} else {
			raw      = ScanGen::OtherSystemSoftwareVersion();
			expected = Reference(raw);
		}
		RC_LOG() << "raw '" << raw << "' expected '" << expected << "'\n";
		RC_ASSERT(kyty::core::FormatSystemSoftwareVersion(raw) == expected);

		// Through param.json: the value is trimmed (TrimWhitespace) before formatting.
		const std::string padding = ScanGen::OneIn(2) ? " \t" : "";
		const auto        json =
		    "{\"requiredSystemSoftwareVersion\":" + JsonString(padding + raw + padding) + "}";
		const auto metadata = kyty::core::ParseParamJson(json, "fallback");
		if (raw.find('\xC3') != std::string::npos) {
			// The only non-ASCII byte the generators use, never followed by a continuation byte:
			// invalid UTF-8 makes the JSON unparsable.
			RC_ASSERT(!metadata.metadata_available);
		} else {
			RC_ASSERT(metadata.metadata_available);
			RC_ASSERT(metadata.firmware_version == Reference(TrimAscii(raw)));
		}
		RC_TAG(expected.empty() ? "empty" : "formatted");
	});
}
