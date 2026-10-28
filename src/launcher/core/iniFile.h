#pragma once

// The QSettings-compatible INI codec for Kyty.ini (Requirements 9.6, 9.7; design "Settings_Store
// (Kyty.ini) schema"; plan D19). Parse() reads a file the way QSettings::IniFormat (Qt 6) reads it,
// and Serialize() writes what QSettings writes for the same keys and values, so a file that the Qt
// launcher wrote re-serializes byte for byte. Ported from Qt's qsettings.cpp; the corner cases were
// confirmed against QSettings in Qt 6.11.2.
//
// Keys are paths with '/' separators, as in the QSettings API: "Launcher/game_dirs",
// "GameConfigurations/1/name". The part before the first '/' is the file's [section], and a key
// without '/' belongs to [General]. In the file the rest of the key uses '\' for '/', and every
// character other than an ASCII letter, a digit, '_', '-', and '.' is written as %XX (up to U+00FF)
// or %UXXXX (one per UTF-16 code unit). A section whose name is "general" in any letter case is
// written as [%General], which reads back as "General", as Qt does.
//
// Serialize() orders sections by name and keys by name within a section, comparing UTF-16 code
// units as QString does, separates sections with an empty line, and writes UTF-8 without a byte
// order mark, with LF line ends. Every key and value survives a rewrite, including the ones no
// caller knows about.
//
// Values (IniValue):
// - String: Qt's escapes \0 \a \b \t \n \v \f \r, \xH for the other control characters, \" and \\;
//   quoted when it contains ',', ';', or '=' or its escaped form starts or ends with a space; a
//   leading '@' is written as "@@", and text containing U+0000 as @String(...). Reading also
//   accepts \? \' \xHHHH (16 bits, more digits wrap) and octal \NNN escapes, skips an unknown
//   escape, joins a line ending in '\', strips unquoted leading and trailing spaces and tabs and
//   an unquoted ';' comment, and drops a U+FEFF that is the first character the value's UTF-8
//   decoder sees (QStringDecoder skips an initial byte order mark).
// - StringList: two or more strings, each written as a String, joined with ", ". QSettings writes
//   a one-element list exactly like a string and an empty list as @Invalid(), and reads them back
//   as a string and an invalid value, so FromStringList() stores those as String and Invalid: the
//   model holds what the file can tell apart. GetStringList() reads a String as a one-element list
//   and Invalid as an empty list, as QVariant::toStringList() does.
// - Invalid: @Invalid(), an invalid QVariant.
// - Opaque: any other @Name(...) value (@ByteArray, @Variant, @DateTime, @Rect, ...), and a list
//   with such an element, kept as the exact text after '=' (without unquoted surrounding spaces)
//   and written back unchanged. Qt itself would re-encode a @ByteArray and lose a @Variant it
//   cannot decode.
//
// Ill-formed UTF-8 in a value or key reads as U+FFFD per byte that cannot start or continue a
// sequence, as Qt's decoder does (including its handling of a sequence cut by an escape, a quote,
// or a comma). Keys and values from the API are first passed through SanitizeUtf8().

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

enum class IniValueKind { String, StringList, Invalid, Opaque };

struct IniValue {
	IniValueKind             kind = IniValueKind::Invalid;
	std::string              text; // String: the decoded UTF-8 text; Opaque: the raw INI text
	std::vector<std::string> list; // StringList: the decoded elements, two or more

	static IniValue FromString(std::string_view text);
	// Zero elements give Invalid and one element gives String (see above).
	static IniValue FromStringList(std::vector<std::string> elements);
	static IniValue Invalid() { return {}; }
	// An Opaque @ByteArray(...) with the bytes encoded as QSettings encodes a QByteArray.
	static IniValue FromByteArray(std::string_view bytes);
	// The value Parse() reads from the line "key=<raw>", or nothing when raw is not one complete
	// value on its own (it ends inside quotes or after a '\', or a newline or an unquoted ';'
	// would end the line early).
	static std::optional<IniValue> FromRaw(std::string_view raw);

	// The text Serialize() writes after "key=".
	std::string Raw() const;

	friend bool operator==(const IniValue&, const IniValue&) = default;
};

class IniFile {
public:
	// Reads the bytes of a file. It never fails: lines that QSettings ignores are ignored, and
	// HasFormatError() is true when QSettings would report QSettings::FormatError (a line without
	// '=' that is not a comment, a section header without ']', or a section name containing '/').
	// Keys keep the exact form Qt gives them, so a key from an empty name ("[Sec]" with "=x" gives
	// "Sec/", "[]" with "x=1" gives "/x") is kept and rewritten, but no API call reaches it.
	static IniFile Parse(std::string_view bytes);

	std::string Serialize() const;

	bool HasFormatError() const { return m_format_error; }

	// The lookups and setters take API keys: invalid UTF-8 is replaced, and leading, trailing, and
	// repeated '/' are removed, as QSettings does. A key that is empty after that is never found
	// and never set (the setters return false).
	const IniValue* Find(std::string_view key) const;
	// String: the text; Invalid: "" (as QVariant::toString()); StringList and Opaque: nothing.
	std::optional<std::string> GetString(std::string_view key) const;
	// StringList: the elements; String: one element; Invalid: none; Opaque: nothing.
	std::optional<std::vector<std::string>> GetStringList(std::string_view key) const;
	// Any kind: the text Serialize() writes after "key=".
	std::optional<std::string> GetRaw(std::string_view key) const;

	// Stores the value in its canonical form (FromString, FromStringList). An Opaque value must be
	// what FromRaw() reads from its text, else nothing is stored and the result is false.
	bool Set(std::string_view key, IniValue value);
	bool SetString(std::string_view key, std::string_view value);
	bool SetStringList(std::string_view key, std::vector<std::string> values);
	// Stores what FromRaw(raw) reads; false when raw is not one complete value.
	bool SetRaw(std::string_view key, std::string_view raw);
	// Removes the key and every key below it ("key/..."); an empty key removes every key.
	void Remove(std::string_view key);

	// Arrays as QSettings::beginReadArray/beginWriteArray store them: element i (0-based) of array
	// A has its keys under "A/<i + 1>/", and "A/size" holds the element count.
	// ArraySize() is that count, or 0 when "A/size" is missing, negative, or not an integer.
	int  ArraySize(std::string_view array) const;
	bool SetArraySize(std::string_view array, int size);
	// "<array>/<index + 1>/<key>".
	static std::string ArrayKey(std::string_view array, int index, std::string_view key);

	// Every key and value, keyed by the stored key.
	const std::map<std::string, IniValue>& Entries() const { return m_entries; }

	// Equal entries (HasFormatError() is not compared).
	friend bool operator==(const IniFile& a, const IniFile& b) {
		return a.m_entries == b.m_entries;
	}

private:
	std::map<std::string, IniValue> m_entries;
	bool                            m_format_error = false;
};

} // namespace kyty::core
