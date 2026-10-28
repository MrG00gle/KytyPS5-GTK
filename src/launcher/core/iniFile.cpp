#include "launcher/core/iniFile.h"

#include "launcher/core/text.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

// Ported from Qt 6's qsettings.cpp (QConfFileSettingsPrivate::readIniFile, readIniLine,
// readIniSection, writeIniFile; QSettingsPrivate::iniEscapedKey, iniUnescapedKey,
// iniEscapedString, iniUnescapedStringList, variantToString, stringToVariant, normalizedKey) and
// qstringconverter.cpp (the UTF-8 decoder).

namespace kyty::core {

namespace {

constexpr size_t   kNone        = std::string_view::npos;
constexpr char16_t kReplacement = 0xFFFD;

// ---- UTF-8 and UTF-16

enum class Utf8Status { Ok, Error, EndOfString };

struct Utf8Sequence {
	Utf8Status status     = Utf8Status::Error;
	char32_t   code_point = 0;
	size_t     length     = 0;
};

bool IsContinuation(unsigned char byte) {
	return (byte & 0xC0u) == 0x80u;
}

// The sequence at bytes[0] (bytes is not empty) as Qt's UTF-8 decoder (QUtf8Functions::fromUtf8)
// sees it: Error for a byte that cannot start a sequence, a byte that does not continue it, an
// overlong form, a surrogate, or a value above U+10FFFF; EndOfString when bytes ends inside a
// sequence whose bytes so far could continue it. Non-characters are valid.
Utf8Sequence DecodeUtf8(std::string_view bytes) {
	const auto          byte = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
	const unsigned char lead = byte(0);
	if (lead < 0x80) {
		return {Utf8Status::Ok, lead, 1};
	}
	if (lead <= 0xC1 || lead >= 0xF5) {
		return {};
	}
	size_t   needed     = 4;
	char32_t minimum    = 0x10000;
	char32_t code_point = lead & 0x07u;
	if (lead < 0xE0) {
		needed     = 2;
		minimum    = 0x80;
		code_point = lead & 0x1Fu;
	} else if (lead < 0xF0) {
		needed     = 3;
		minimum    = 0x800;
		code_point = lead & 0x0Fu;
	}
	const size_t available = bytes.size() - 1;
	if (available < needed - 1) {
		for (size_t i = 1; i <= available && i <= 2; i++) {
			if (!IsContinuation(byte(i))) {
				return {};
			}
		}
		return {Utf8Status::EndOfString, 0, 0};
	}
	for (size_t i = 1; i < needed; i++) {
		if (!IsContinuation(byte(i))) {
			return {};
		}
		code_point = (code_point << 6u) | (byte(i) & 0x3Fu);
	}
	if (code_point < minimum || (code_point >= 0xD800 && code_point <= 0xDFFF) ||
	    code_point > 0x10FFFF) {
		return {};
	}
	return {Utf8Status::Ok, code_point, needed};
}

void AppendUtf16(std::u16string& out, char32_t code_point) {
	if (code_point < 0x10000) {
		out += static_cast<char16_t>(code_point);
	} else {
		code_point -= 0x10000;
		out += static_cast<char16_t>(0xD800 + (code_point >> 10u));
		out += static_cast<char16_t>(0xDC00 + (code_point & 0x3FFu));
	}
}

void AppendUtf8(std::string& out, char32_t code_point) {
	if (code_point < 0x80) {
		out += static_cast<char>(code_point);
	} else if (code_point < 0x800) {
		out += static_cast<char>(0xC0 | (code_point >> 6u));
		out += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else if (code_point < 0x10000) {
		out += static_cast<char>(0xE0 | (code_point >> 12u));
		out += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (code_point & 0x3Fu));
	} else {
		out += static_cast<char>(0xF0 | (code_point >> 18u));
		out += static_cast<char>(0x80 | ((code_point >> 12u) & 0x3Fu));
		out += static_cast<char>(0x80 | ((code_point >> 6u) & 0x3Fu));
		out += static_cast<char>(0x80 | (code_point & 0x3Fu));
	}
}

// UTF-16 to UTF-8; a surrogate without its pair becomes U+FFFD.
std::string Utf16ToUtf8(std::u16string_view units) {
	std::string out;
	out.reserve(units.size());
	for (size_t i = 0; i < units.size(); i++) {
		char32_t code_point = units[i];
		if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < units.size() &&
		    units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
			code_point = 0x10000 + ((code_point - 0xD800) << 10u) + (units[i + 1] - 0xDC00);
			i++;
		} else if (code_point >= 0xD800 && code_point <= 0xDFFF) {
			code_point = kReplacement;
		}
		AppendUtf8(out, code_point);
	}
	return out;
}

// The first code point of text (removed from it); U+FFFD for a byte that does not start a valid
// sequence. Text from the model is valid UTF-8.
char32_t PopCodePoint(std::string_view& text) {
	const auto sequence = DecodeUtf8(text);
	if (sequence.status != Utf8Status::Ok) {
		text.remove_prefix(1);
		return kReplacement;
	}
	text.remove_prefix(sequence.length);
	return sequence.code_point;
}

std::u32string CodePoints(std::string_view text) {
	std::u32string code_points;
	code_points.reserve(text.size());
	while (!text.empty()) {
		code_points += PopCodePoint(text);
	}
	return code_points;
}

// QString::fromUtf8: a leading byte order mark is skipped, and each byte that cannot start or
// continue a sequence becomes U+FFFD, as does each byte of a sequence cut by the end.
std::u16string DecodeUtf8Whole(std::string_view bytes) {
	if (bytes.starts_with("\xEF\xBB\xBF")) {
		bytes.remove_prefix(3);
	}
	std::u16string out;
	while (!bytes.empty()) {
		const auto sequence = DecodeUtf8(bytes);
		if (sequence.status == Utf8Status::Ok) {
			AppendUtf16(out, sequence.code_point);
			bytes.remove_prefix(sequence.length);
		} else if (sequence.status == Utf8Status::Error) {
			out += kReplacement;
			bytes.remove_prefix(1);
		} else {
			out.append(bytes.size(), kReplacement);
			break;
		}
	}
	return out;
}

// QStringDecoder(Utf8) with the default flags, fed the raw pieces of one value in order: U+FEFF as
// the first character it decodes is skipped, a sequence cut at the end of a piece waits for the
// next piece (and is dropped when none follows), and when the waiting bytes and the next piece do
// not form a sequence, one U+FFFD is written and the first byte of that piece is skipped as well.
class Utf8StreamDecoder {
public:
	void Decode(std::string_view piece, std::u16string& out) {
		if (!m_started || !m_pending.empty()) {
			std::string head = m_pending;
			head.append(piece.substr(0, 4 - m_pending.size()));
			const auto first = DecodeUtf8(head);
			if (first.status == Utf8Status::EndOfString) {
				m_pending = head;
				return;
			}
			size_t consumed = 1;
			if (first.status == Utf8Status::Error) {
				out += kReplacement;
			} else {
				if (m_started || first.code_point != 0xFEFF) {
					AppendUtf16(out, first.code_point);
				}
				consumed = first.length - m_pending.size();
			}
			m_started = true;
			m_pending.clear();
			piece.remove_prefix(consumed);
		}
		while (!piece.empty()) {
			const auto sequence = DecodeUtf8(piece);
			if (sequence.status == Utf8Status::EndOfString) {
				m_pending = piece;
				return;
			}
			if (sequence.status == Utf8Status::Error) {
				out += kReplacement;
				piece.remove_prefix(1);
			} else {
				AppendUtf16(out, sequence.code_point);
				piece.remove_prefix(sequence.length);
			}
		}
	}

private:
	std::string m_pending;
	bool        m_started = false;
};

// ---- ASCII helpers

bool IsAsciiSpace(char c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

// QByteArrayView::trimmed().
std::string_view TrimAscii(std::string_view text) {
	while (!text.empty() && IsAsciiSpace(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && IsAsciiSpace(text.back())) {
		text.remove_suffix(1);
	}
	return text;
}

char AsciiLower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		       return AsciiLower(x) == AsciiLower(y);
	       });
}

int HexDigitValue(char32_t c) {
	if (c >= '0' && c <= '9') {
		return static_cast<int>(c - '0');
	}
	if (c >= 'a' && c <= 'f') {
		return static_cast<int>(c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F') {
		return static_cast<int>(c - 'A' + 10);
	}
	return -1;
}

int OctDigitValue(char32_t c) {
	return c >= '0' && c <= '7' ? static_cast<int>(c - '0') : -1;
}

bool IsAsciiAlphaNumeric(char32_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

constexpr char kUpperHex[] = "0123456789ABCDEF";
constexpr char kLowerHex[] = "0123456789abcdef";

// ---- Ordering

// QString comparison order: UTF-16 code units, which is code point order except that U+E000-U+FFFF
// sort after the supplementary planes (whose surrogate code units are below U+E000).
uint32_t Utf16Rank(char32_t code_point) {
	return code_point >= 0xE000 && code_point <= 0xFFFF ? code_point + 0x110000 : code_point;
}

bool Utf16Less(std::string_view a, std::string_view b) {
	while (!a.empty() && !b.empty()) {
		const char32_t x = PopCodePoint(a);
		const char32_t y = PopCodePoint(b);
		if (x != y) {
			return Utf16Rank(x) < Utf16Rank(y);
		}
	}
	return a.empty() && !b.empty();
}

struct Utf16Order {
	bool operator()(const std::string& a, const std::string& b) const { return Utf16Less(a, b); }
};

// ---- Keys

// QSettingsPrivate::normalizedKey: leading, trailing, and repeated '/' removed.
std::string NormalizeKey(std::string_view key) {
	std::string result;
	size_t      i = 0;
	while (i < key.size()) {
		if (key[i] == '/') {
			i++;
			continue;
		}
		size_t end = key.find('/', i);
		if (end == kNone) {
			end = key.size();
		}
		if (!result.empty()) {
			result += '/';
		}
		result.append(key.substr(i, end - i));
		i = end;
	}
	return result;
}

std::string ApiKey(std::string_view key) {
	return NormalizeKey(SanitizeUtf8(key));
}

// QString::toUShort(&ok, 16) on the digits of a %XX or %UXXXX escape: surrounding whitespace, a
// '+', and a 0x prefix are accepted.
std::optional<char16_t> ParseEscapeDigits(std::u16string_view text) {
	const auto is_space = [](char16_t c) { return c == u' ' || (c >= u'\t' && c <= u'\r'); };
	while (!text.empty() && is_space(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && is_space(text.back())) {
		text.remove_suffix(1);
	}
	if (!text.empty() && text.front() == u'+') {
		text.remove_prefix(1);
	}
	if (text.size() > 2 && text[0] == u'0' && (text[1] == u'x' || text[1] == u'X')) {
		text.remove_prefix(2);
	}
	if (text.empty()) {
		return std::nullopt;
	}
	uint32_t value = 0;
	for (const char16_t c: text) {
		const int digit = HexDigitValue(c);
		if (digit < 0) {
			return std::nullopt;
		}
		value = value * 16 + static_cast<uint32_t>(digit);
	}
	return static_cast<char16_t>(value);
}

// QSettingsPrivate::iniUnescapedKey: '\' becomes '/', %XX and %UXXXX become that UTF-16 code unit,
// and a '%' that does not start a valid escape is kept.
std::string UnescapeKey(std::string_view raw) {
	const std::u16string decoded = DecodeUtf8Whole(raw);
	std::u16string       result;
	size_t               i = 0;
	while (i < decoded.size()) {
		const char16_t c = decoded[i];
		if (c == u'\\') {
			result += u'/';
			i++;
			continue;
		}
		if (c != u'%' || i + 1 == decoded.size()) {
			result += c;
			i++;
			continue;
		}
		size_t first  = i + 1;
		size_t digits = 2;
		if (decoded[first] == u'U') {
			first++;
			digits = 4;
		}
		const auto value =
		    first + digits <= decoded.size()
		        ? ParseEscapeDigits(std::u16string_view(decoded).substr(first, digits))
		        : std::nullopt;
		if (!value) {
			result += u'%';
			i++;
			continue;
		}
		result += *value;
		i = first + digits;
	}
	return Utf16ToUtf8(result);
}

// QSettingsPrivate::iniEscapedKey.
void AppendEscapedKey(std::string& out, std::string_view key) {
	for (const char32_t c: CodePoints(key)) {
		if (c == '/') {
			out += '\\';
		} else if (IsAsciiAlphaNumeric(c) || c == '_' || c == '-' || c == '.') {
			out += static_cast<char>(c);
		} else if (c <= 0xFF) {
			out += '%';
			out += kUpperHex[c >> 4u];
			out += kUpperHex[c & 0xFu];
		} else {
			std::u16string units;
			AppendUtf16(units, c);
			for (const char16_t unit: units) {
				out += "%U";
				for (int shift = 12; shift >= 0; shift -= 4) {
					out += kUpperHex[(unit >> shift) & 0xFu];
				}
			}
		}
	}
}

// ---- Lines (QConfFileSettingsPrivate::readIniLine)

struct IniLine {
	size_t start  = 0;
	size_t length = 0;
	size_t equals = kNone; // position in the data of the first '=' outside quotes
};

enum : uint8_t { kSpace = 1, kSpecial = 2 };

uint8_t CharTraits(char c) {
	switch (c) {
		case '\t':
		case ' ': return kSpace;
		case '\n':
		case '\r': return kSpace | kSpecial;
		case '"':
		case ';':
		case '=':
		case '\\': return kSpecial;
		default: return 0;
	}
}

// The next line from pos: leading whitespace and lines that start with ';' are skipped; the line
// ends at a CR or LF outside quotes, or at a ';' outside quotes (the rest is a comment); a '\'
// takes the next character, and a CR LF or LF CR pair after it, into the line. False at the end.
bool ReadIniLine(std::string_view data, size_t& pos, IniLine& line) {
	const size_t size      = data.size();
	bool         in_quotes = false;
	line.equals            = kNone;
	line.start             = pos;
	while (line.start < size && (CharTraits(data[line.start]) & kSpace) != 0) {
		line.start++;
	}
	size_t i = line.start;
	while (i < size) {
		const char c = data[i++];
		if ((CharTraits(c) & kSpecial) == 0) {
			continue;
		}
		if (c == '=') {
			if (!in_quotes && line.equals == kNone) {
				line.equals = i - 1;
			}
		} else if (c == '\n' || c == '\r') {
			if (i == line.start + 1) {
				line.start++;
			} else if (!in_quotes) {
				i--;
				break;
			}
		} else if (c == '\\') {
			if (i < size) {
				const char next = data[i++];
				if (i < size) {
					const char after = data[i];
					if ((next == '\n' && after == '\r') || (next == '\r' && after == '\n')) {
						i++;
					}
				}
			}
		} else if (c == '"') {
			in_quotes = !in_quotes;
		} else if (i == line.start + 1) { // ';' starting a line: a comment line
			while (i < size && data[i] != '\n' && data[i] != '\r') {
				i++;
			}
			while (i < size && (CharTraits(data[i]) & kSpace) != 0) {
				i++;
			}
			line.start = i;
		} else if (!in_quotes) { // ';' after text: the rest of the line is a comment
			i--;
			break;
		}
	}
	pos         = i;
	line.length = i - line.start;
	return line.length > 0;
}

// ---- Values

struct Unescaped {
	bool                        is_list = false;
	std::u16string              text;
	std::vector<std::u16string> list;

	friend bool operator==(const Unescaped&, const Unescaped&) = default;
};

std::optional<char16_t> NamedEscape(char c) {
	switch (c) {
		case 'a': return u'\a';
		case 'b': return u'\b';
		case 'f': return u'\f';
		case 'n': return u'\n';
		case 'r': return u'\r';
		case 't': return u'\t';
		case 'v': return u'\v';
		case '"': return u'"';
		case '?': return u'?';
		case '\'': return u'\'';
		case '\\': return u'\\';
		default: return std::nullopt;
	}
}

void ChopTrailingSpaces(std::u16string& text, size_t limit) {
	while (text.size() > limit && (text.back() == u' ' || text.back() == u'\t')) {
		text.pop_back();
	}
}

Unescaped Finish(Unescaped&& result) {
	if (result.is_list) {
		result.list.push_back(result.text);
	}
	return std::move(result);
}

// QSettingsPrivate::iniUnescapedStringList: the text after '=' as one string, or as a list when it
// has a ',' outside quotes. Unquoted leading spaces and tabs are skipped and trailing ones removed
// (unless the value had a quote or ends right after an escape), quotes are removed, and escapes
// are decoded; raw text goes through one Utf8StreamDecoder.
Unescaped UnescapeValue(std::string_view str) {
	Unescaped         result;
	std::u16string&   text = result.text;
	Utf8StreamDecoder decoder;
	bool              in_quotes      = false;
	bool              current_quoted = false;
	size_t            i              = 0;
	const auto        skip_spaces    = [&] {
		while (i < str.size() && (str[i] == ' ' || str[i] == '\t')) {
			i++;
		}
	};
	skip_spaces();
	size_t chop_limit = 0;
	while (i < str.size()) {
		const char c = str[i];
		if (c == '\\') {
			i++;
			if (i >= str.size()) {
				return Finish(std::move(result));
			}
			const char code = str[i++];
			if (const auto named = NamedEscape(code)) {
				text += *named;
			} else if (code == 'x' || OctDigitValue(code) >= 0) {
				const bool hex   = code == 'x';
				const auto digit = [&](char d) {
					return hex ? HexDigitValue(d) : OctDigitValue(d);
				};
				if (hex && i >= str.size()) {
					return Finish(std::move(result));
				}
				if (!hex || digit(str[i]) >= 0) {
					auto value = static_cast<char16_t>(hex ? 0 : OctDigitValue(code));
					while (i < str.size() && digit(str[i]) >= 0) {
						value = static_cast<char16_t>((value << (hex ? 4u : 3u)) + digit(str[i]));
						i++;
					}
					text += value;
					if (i >= str.size()) {
						return Finish(std::move(result));
					}
				}
			} else if (code == '\n' || code == '\r') {
				// A line continuation; a CR LF or LF CR pair counts as one line end.
				if (i < str.size() && (str[i] == '\n' || str[i] == '\r') && str[i] != code) {
					i++;
				}
			}
			// Any other escaped character is skipped.
			chop_limit = text.size();
			continue;
		}
		if (c == '"') {
			i++;
			current_quoted = true;
			in_quotes      = !in_quotes;
			if (!in_quotes) {
				skip_spaces();
				chop_limit = text.size();
			}
			continue;
		}
		if (c == ',' && !in_quotes) {
			if (!current_quoted) {
				ChopTrailingSpaces(text, chop_limit);
			}
			if (!result.is_list) {
				result.is_list = true;
				result.list.clear();
			}
			result.list.push_back(text);
			text.clear();
			current_quoted = false;
			i++;
			skip_spaces();
			chop_limit = 0;
			continue;
		}
		size_t end = i + 1;
		while (end < str.size() && str[end] != '\\' && str[end] != '"' && str[end] != ',') {
			end++;
		}
		decoder.Decode(str.substr(i, end - i), text);
		i = end;
	}
	if (!current_quoted) {
		ChopTrailingSpaces(text, chop_limit);
	}
	return Finish(std::move(result));
}

// QSettingsPrivate::stringToVariant for one decoded string, with every @Name(...) form other than
// @String(...) and @Invalid() kept opaque (plan D19).
struct TextValue {
	IniValueKind kind = IniValueKind::String;
	std::string  text; // String only
};

TextValue ClassifyText(std::string text) {
	if (text.starts_with('@')) {
		if (text.ends_with(')')) {
			if (text.starts_with("@String(")) {
				return {IniValueKind::String, text.substr(8, text.size() - 9)};
			}
			if (text == "@Invalid()") {
				return {IniValueKind::Invalid, {}};
			}
			if (!text.starts_with("@@") && text.find('(') != std::string::npos) {
				return {IniValueKind::Opaque, {}};
			}
		}
		if (text.starts_with("@@")) {
			text.erase(0, 1);
		}
	}
	return {IniValueKind::String, std::move(text)};
}

// The value of the text after '=' in a line.
IniValue ParseValue(std::string_view bytes) {
	// UnescapeValue() skips leading spaces and tabs as well, so an opaque value is kept without
	// them.
	while (!bytes.empty() && (bytes.front() == ' ' || bytes.front() == '\t')) {
		bytes.remove_prefix(1);
	}
	const Unescaped unescaped = UnescapeValue(bytes);
	IniValue        value;
	if (!unescaped.is_list) {
		auto text = ClassifyText(Utf16ToUtf8(unescaped.text));
		if (text.kind != IniValueKind::Opaque) {
			value.kind = text.kind;
			value.text = std::move(text.text);
			return value;
		}
	} else {
		// QSettingsPrivate::stringListToVariantList: an element starting with "@@" loses one '@';
		// a list with an element that is not a string as a whole stays opaque.
		std::vector<std::string> elements;
		for (const auto& element: unescaped.list) {
			auto text = ClassifyText(Utf16ToUtf8(element));
			if (text.kind != IniValueKind::String) {
				break;
			}
			elements.push_back(std::move(text.text));
		}
		if (elements.size() == unescaped.list.size()) {
			value.kind = IniValueKind::StringList;
			value.list = std::move(elements);
			return value;
		}
	}
	// Opaque: the raw text, without trailing spaces and tabs when they do not change the value
	// (they can, after a quote: "a"b  reads as "ab  ").
	size_t end = bytes.size();
	while (end > 0 && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t')) {
		end--;
	}
	if (end < bytes.size() && UnescapeValue(bytes.substr(0, end)) == unescaped) {
		bytes = bytes.substr(0, end);
	}
	value.kind = IniValueKind::Opaque;
	value.text = std::string(bytes);
	return value;
}

bool StartsWithAscii(std::u32string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() &&
	       std::equal(prefix.begin(), prefix.end(), text.begin(),
	                  [](char a, char32_t b) { return static_cast<char32_t>(a) == b; });
}

void AppendHexEscape(std::string& out, char32_t c) {
	out += "\\x";
	std::string digits;
	do {
		digits.insert(digits.begin(), kLowerHex[c & 0xFu]);
		c >>= 4u;
	} while (c != 0);
	out += digits;
}

// QSettingsPrivate::iniEscapedString: escapes, then quotes when the text has ',', ';', or '=' or
// the escaped text starts or ends with a space. A hex digit after a \x or \0 escape is escaped
// too, so that the reader does not take it as part of the escape. Text starting with @ByteArray(,
// @Variant(, or @DateTime( holds bytes: characters from U+007F are escaped instead of written as
// UTF-8.
void AppendEscapedString(std::string& out, std::u32string_view text) {
	const bool use_utf8 =
	    !(StartsWithAscii(text, "@ByteArray(") || StartsWithAscii(text, "@Variant(") ||
	      StartsWithAscii(text, "@DateTime("));
	const size_t start                = out.size();
	bool         needs_quotes         = false;
	bool         escape_next_if_digit = false;
	for (const char32_t c: text) {
		if (c == ';' || c == ',' || c == '=') {
			needs_quotes = true;
		}
		if (escape_next_if_digit && HexDigitValue(c) >= 0) {
			AppendHexEscape(out, c);
			continue;
		}
		escape_next_if_digit = false;
		switch (c) {
			case 0:
				out += "\\0";
				escape_next_if_digit = true;
				break;
			case '\a': out += "\\a"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			case '\v': out += "\\v"; break;
			case '"':
			case '\\':
				out += '\\';
				out += static_cast<char>(c);
				break;
			default:
				if (c <= 0x1F || (c >= 0x7F && !use_utf8)) {
					AppendHexEscape(out, c);
					escape_next_if_digit = true;
				} else if (use_utf8) {
					AppendUtf8(out, c);
				} else {
					out += static_cast<char>(c);
				}
				break;
		}
	}
	if (needs_quotes || (start < out.size() && (out[start] == ' ' || out.back() == ' '))) {
		out.insert(start, 1, '"');
		out += '"';
	}
}

// QSettingsPrivate::variantToString for a string: @String(...) when it contains U+0000, else a
// leading '@' doubled.
std::u32string VariantText(std::string_view text) {
	std::u32string code_points = CodePoints(text);
	if (code_points.find(U'\0') != std::u32string::npos) {
		return U"@String(" + code_points + U")";
	}
	if (!code_points.empty() && code_points.front() == U'@') {
		code_points.insert(code_points.begin(), U'@');
	}
	return code_points;
}

// ---- Sections

void ReadSection(const std::string& section, std::string_view text,
                 std::map<std::string, IniValue>& entries, bool& format_error) {
	size_t  pos = 0;
	IniLine line;
	while (ReadIniLine(text, pos, line)) {
		const auto content = text.substr(line.start, line.length);
		if (line.equals == kNone) {
			if (!content.starts_with(';')) {
				format_error = true;
			}
			continue;
		}
		const size_t equals = line.equals - line.start;
		// Qt 6 keeps a key that is empty after trimming: "=x" in [Sec] is the key "Sec/".
		std::string key = section + UnescapeKey(TrimAscii(content.substr(0, equals)));
		entries.insert_or_assign(std::move(key), ParseValue(content.substr(equals + 1)));
	}
}

} // namespace

// ---- IniValue

IniValue IniValue::FromString(std::string_view text) {
	IniValue value;
	value.kind = IniValueKind::String;
	value.text = SanitizeUtf8(text);
	return value;
}

IniValue IniValue::FromStringList(std::vector<std::string> elements) {
	if (elements.empty()) {
		return Invalid();
	}
	if (elements.size() == 1) {
		return FromString(elements.front());
	}
	IniValue value;
	value.kind = IniValueKind::StringList;
	for (auto& element: elements) {
		element = SanitizeUtf8(element);
	}
	value.list = std::move(elements);
	return value;
}

IniValue IniValue::FromByteArray(std::string_view bytes) {
	std::u32string text = U"@ByteArray(";
	for (const char byte: bytes) {
		text += static_cast<char32_t>(static_cast<unsigned char>(byte));
	}
	text += U')';
	IniValue value;
	value.kind = IniValueKind::Opaque;
	AppendEscapedString(value.text, text);
	return value;
}

std::optional<IniValue> IniValue::FromRaw(std::string_view raw) {
	std::string data = "k=";
	data.append(raw);
	data += '\n';
	size_t  pos = 0;
	IniLine line;
	if (!ReadIniLine(data, pos, line) || line.start != 0 || line.equals != 1 ||
	    line.length != data.size() - 1) {
		return std::nullopt;
	}
	return ParseValue(std::string_view(data).substr(2, raw.size()));
}

std::string IniValue::Raw() const {
	std::string out;
	switch (kind) {
		case IniValueKind::String: AppendEscapedString(out, VariantText(text)); break;
		case IniValueKind::StringList:
			if (list.empty()) {
				out = "@Invalid()";
			}
			for (size_t i = 0; i < list.size(); i++) {
				if (i != 0) {
					out += ", ";
				}
				AppendEscapedString(out, VariantText(list[i]));
			}
			break;
		case IniValueKind::Invalid: out = "@Invalid()"; break;
		case IniValueKind::Opaque: out = text; break;
	}
	return out;
}

// ---- IniFile

IniFile IniFile::Parse(std::string_view bytes) {
	IniFile file;
	if (bytes.starts_with("\xEF\xBB\xBF")) {
		bytes.remove_prefix(3);
	}
	// The text of each section, sections with the same name joined, as Qt collects it. Qt then
	// reads the sections in name order, so of two keys that end up with the same path the one
	// from the later section in that order wins ("[a\b]" "k=" over "[a]" "b\k=").
	std::map<std::string, std::string, Utf16Order> sections;
	std::string                                    current; // "" for [General], else "<name>/"
	size_t                                         current_start = 0;
	size_t                                         pos           = 0;
	IniLine                                        line;
	const auto                                     flush = [&](size_t end) {
		auto& text = sections[current];
		if (!text.empty()) {
			text += '\n';
		}
		text.append(bytes.substr(current_start, end - current_start));
	};
	while (ReadIniLine(bytes, pos, line)) {
		const auto content = bytes.substr(line.start, line.length);
		if (!content.starts_with('[')) {
			continue;
		}
		flush(line.start);
		size_t close = content.find(']');
		if (close == kNone) {
			file.m_format_error = true;
			close               = content.size();
		}
		const auto name = TrimAscii(content.substr(1, close - 1));
		if (EqualsIgnoreAsciiCase(name, "general")) {
			current.clear();
		} else {
			current = EqualsIgnoreAsciiCase(name, "%general") ? std::string(name.substr(1))
			                                                  : UnescapeKey(name);
			if (current.find('/') != std::string::npos) {
				file.m_format_error = true; // as QSettings in Qt 6.11 reports it
			}
			current += '/';
		}
		current_start = pos;
	}
	flush(line.start);
	for (const auto& [section, text]: sections) {
		ReadSection(section, text, file.m_entries, file.m_format_error);
	}
	return file;
}

std::string IniFile::Serialize() const {
	struct Item {
		std::string_view   section;
		std::string_view   name;
		const std::string* key   = nullptr;
		const IniValue*    value = nullptr;
	};
	std::vector<Item> items;
	items.reserve(m_entries.size());
	for (const auto& [key, value]: m_entries) {
		const std::string_view path  = key;
		const size_t           slash = path.find('/');
		if (slash == kNone) {
			items.push_back({{}, path, &key, &value});
		} else {
			items.push_back({path.substr(0, slash), path.substr(slash + 1), &key, &value});
		}
	}
	std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
		if (a.section != b.section) {
			return Utf16Less(a.section, b.section);
		}
		if (a.name != b.name) {
			return Utf16Less(a.name, b.name);
		}
		return Utf16Less(*a.key, *b.key);
	});
	std::string out;
	for (size_t i = 0; i < items.size(); i++) {
		const auto& item = items[i];
		// "x" and "/x" are both "x" in [General]; Qt writes the value of the later key.
		if (i + 1 < items.size() && items[i + 1].section == item.section &&
		    items[i + 1].name == item.name) {
			continue;
		}
		if (i == 0 || items[i - 1].section != item.section) {
			std::string header;
			AppendEscapedKey(header, item.section);
			if (header.empty()) {
				header = "General";
			} else if (EqualsIgnoreAsciiCase(header, "general")) {
				header = "%General";
			}
			if (!out.empty()) {
				out += '\n';
			}
			out += '[';
			out += header;
			out += "]\n";
		}
		AppendEscapedKey(out, item.name);
		out += '=';
		out += item.value->Raw();
		out += '\n';
	}
	return out;
}

const IniValue* IniFile::Find(std::string_view key) const {
	const std::string normalized = ApiKey(key);
	if (normalized.empty()) {
		return nullptr;
	}
	const auto found = m_entries.find(normalized);
	return found == m_entries.end() ? nullptr : &found->second;
}

std::optional<std::string> IniFile::GetString(std::string_view key) const {
	const IniValue* value = Find(key);
	if (value == nullptr) {
		return std::nullopt;
	}
	switch (value->kind) {
		case IniValueKind::String: return value->text;
		case IniValueKind::Invalid: return std::string();
		case IniValueKind::StringList:
		case IniValueKind::Opaque: break;
	}
	return std::nullopt;
}

std::optional<std::vector<std::string>> IniFile::GetStringList(std::string_view key) const {
	const IniValue* value = Find(key);
	if (value == nullptr) {
		return std::nullopt;
	}
	switch (value->kind) {
		case IniValueKind::String: return std::vector<std::string> {value->text};
		case IniValueKind::StringList: return value->list;
		case IniValueKind::Invalid: return std::vector<std::string> {};
		case IniValueKind::Opaque: break;
	}
	return std::nullopt;
}

std::optional<std::string> IniFile::GetRaw(std::string_view key) const {
	const IniValue* value = Find(key);
	if (value == nullptr) {
		return std::nullopt;
	}
	return value->Raw();
}

bool IniFile::Set(std::string_view key, IniValue value) {
	std::string normalized = ApiKey(key);
	if (normalized.empty()) {
		return false;
	}
	std::optional<IniValue> canonical;
	switch (value.kind) {
		case IniValueKind::String: canonical = IniValue::FromString(value.text); break;
		case IniValueKind::StringList:
			canonical = IniValue::FromStringList(std::move(value.list));
			break;
		case IniValueKind::Invalid: canonical = IniValue::Invalid(); break;
		case IniValueKind::Opaque: {
			auto parsed = IniValue::FromRaw(value.text);
			if (parsed && parsed->kind == IniValueKind::Opaque && parsed->text == value.text) {
				canonical = std::move(parsed);
			}
			break;
		}
	}
	if (!canonical) {
		return false;
	}
	m_entries.insert_or_assign(std::move(normalized), std::move(*canonical));
	return true;
}

bool IniFile::SetString(std::string_view key, std::string_view value) {
	return Set(key, IniValue::FromString(value));
}

bool IniFile::SetStringList(std::string_view key, std::vector<std::string> values) {
	return Set(key, IniValue::FromStringList(std::move(values)));
}

bool IniFile::SetRaw(std::string_view key, std::string_view raw) {
	auto value = IniValue::FromRaw(raw);
	return value && Set(key, std::move(*value));
}

void IniFile::Remove(std::string_view key) {
	const std::string normalized = ApiKey(key);
	if (normalized.empty()) {
		m_entries.clear();
		return;
	}
	m_entries.erase(normalized);
	const std::string prefix = normalized + "/";
	auto              it     = m_entries.lower_bound(prefix);
	while (it != m_entries.end() && it->first.starts_with(prefix)) {
		it = m_entries.erase(it);
	}
}

int IniFile::ArraySize(std::string_view array) const {
	const auto text = GetString(std::string(array) + "/size");
	if (!text) {
		return 0;
	}
	std::string_view digits = TrimAscii(*text);
	if (digits.starts_with('+')) {
		digits.remove_prefix(1);
	}
	int        size   = 0;
	const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), size);
	if (digits.empty() || result.ec != std::errc() || result.ptr != digits.data() + digits.size() ||
	    size < 0) {
		return 0;
	}
	return size;
}

bool IniFile::SetArraySize(std::string_view array, int size) {
	return SetString(std::string(array) + "/size", std::to_string(size));
}

std::string IniFile::ArrayKey(std::string_view array, int index, std::string_view key) {
	std::string result(array);
	result += '/';
	result += std::to_string(index + 1);
	result += '/';
	result.append(key);
	return result;
}

} // namespace kyty::core
