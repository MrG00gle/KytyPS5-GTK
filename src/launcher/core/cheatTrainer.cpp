#include "launcher/core/cheatTrainer.h"

#include "launcher/core/text.h"

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace kyty::core {

namespace {

using OrderedJson = nlohmann::ordered_json;

// The fixed key and IV of the MC4 format (upstream's Decrypt), as ASCII bytes.
constexpr std::string_view kMc4Key = "304c6528f659c766110239a51cl5dd9c";
constexpr std::string_view kMc4Iv  = "u@}kzW2u[u(8DWar";

// ---- AES-256 (FIPS 197)

constexpr std::size_t kAesRounds = 14;
using RoundKeys                  = std::array<uint8_t, 16 * (kAesRounds + 1)>;
using AesBlock                   = std::array<uint8_t, 16>;

// Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1.
uint8_t Multiply(uint8_t a, uint8_t b) {
	uint8_t product = 0;
	while (b != 0) {
		if ((b & 1u) != 0) {
			product = static_cast<uint8_t>(product ^ a);
		}
		a = static_cast<uint8_t>((a & 0x80u) != 0 ? (a << 1u) ^ 0x1bu : a << 1u);
		b = static_cast<uint8_t>(b >> 1u);
	}
	return product;
}

uint8_t RotateLeft(uint8_t value, unsigned bits) {
	return static_cast<uint8_t>((value << bits) | (value >> (8u - bits)));
}

struct AesTables {
	std::array<uint8_t, 256> sbox {};
	std::array<uint8_t, 256> inverse {};
};

// The S-box of FIPS 197 section 5.1.1 (the multiplicative inverse in GF(2^8), that is x^254, then
// the affine transformation) and its inverse.
AesTables MakeAesTables() {
	AesTables tables;
	for (unsigned x = 0; x < 256; x++) {
		uint8_t inverse = 0;
		if (x != 0) {
			uint8_t power = 1;
			auto    base  = static_cast<uint8_t>(x);
			for (unsigned exponent = 254; exponent != 0; exponent >>= 1u) {
				if ((exponent & 1u) != 0) {
					power = Multiply(power, base);
				}
				base = Multiply(base, base);
			}
			inverse = power;
		}
		const auto value =
		    static_cast<uint8_t>(inverse ^ RotateLeft(inverse, 1) ^ RotateLeft(inverse, 2) ^
		                         RotateLeft(inverse, 3) ^ RotateLeft(inverse, 4) ^ 0x63u);
		tables.sbox[x]        = value;
		tables.inverse[value] = static_cast<uint8_t>(x);
	}
	return tables;
}

const AesTables& Tables() {
	static const AesTables tables = MakeAesTables();
	return tables;
}

// FIPS 197 section 5.2 for a 256-bit key: 60 words, 4 bytes each.
RoundKeys ExpandKey(const std::array<uint8_t, 32>& key) {
	const auto& sbox = Tables().sbox;
	RoundKeys   words {};
	std::copy(key.begin(), key.end(), words.begin());
	uint8_t round_constant = 1;
	for (std::size_t i = 8; i < words.size() / 4; i++) {
		std::array<uint8_t, 4> word = {words[4 * i - 4], words[4 * i - 3], words[4 * i - 2],
		                               words[4 * i - 1]};
		if (i % 8 == 0) {
			// SubWord(RotWord(word)) xor Rcon.
			word           = {static_cast<uint8_t>(sbox[word[1]] ^ round_constant), sbox[word[2]],
			                  sbox[word[3]], sbox[word[0]]};
			round_constant = Multiply(round_constant, 2);
		} else if (i % 8 == 4) {
			for (auto& byte: word) {
				byte = sbox[byte];
			}
		}
		for (std::size_t j = 0; j < 4; j++) {
			words[4 * i + j] = static_cast<uint8_t>(words[4 * (i - 8) + j] ^ word[j]);
		}
	}
	return words;
}

// The state holds the block column by column: row r of column c is state[r + 4 * c].
void AddRoundKey(AesBlock& state, const RoundKeys& keys, std::size_t round) {
	for (std::size_t i = 0; i < state.size(); i++) {
		state[i] = static_cast<uint8_t>(state[i] ^ keys[16 * round + i]);
	}
}

void InvShiftRows(AesBlock& state) {
	const AesBlock copy = state;
	for (std::size_t row = 1; row < 4; row++) {
		for (std::size_t column = 0; column < 4; column++) {
			state[row + 4 * ((column + row) % 4)] = copy[row + 4 * column];
		}
	}
}

void InvSubBytes(AesBlock& state) {
	const auto& inverse = Tables().inverse;
	for (auto& byte: state) {
		byte = inverse[byte];
	}
}

void InvMixColumns(AesBlock& state) {
	for (std::size_t column = 0; column < 4; column++) {
		uint8_t*   c  = &state[4 * column];
		const auto a0 = c[0];
		const auto a1 = c[1];
		const auto a2 = c[2];
		const auto a3 = c[3];
		c[0] = static_cast<uint8_t>(Multiply(a0, 14) ^ Multiply(a1, 11) ^ Multiply(a2, 13) ^
		                            Multiply(a3, 9));
		c[1] = static_cast<uint8_t>(Multiply(a0, 9) ^ Multiply(a1, 14) ^ Multiply(a2, 11) ^
		                            Multiply(a3, 13));
		c[2] = static_cast<uint8_t>(Multiply(a0, 13) ^ Multiply(a1, 9) ^ Multiply(a2, 14) ^
		                            Multiply(a3, 11));
		c[3] = static_cast<uint8_t>(Multiply(a0, 11) ^ Multiply(a1, 13) ^ Multiply(a2, 9) ^
		                            Multiply(a3, 14));
	}
}

// The inverse cipher of FIPS 197 section 5.3.
void DecryptBlock(AesBlock& state, const RoundKeys& keys) {
	AddRoundKey(state, keys, kAesRounds);
	for (std::size_t round = kAesRounds - 1; round >= 1; round--) {
		InvShiftRows(state);
		InvSubBytes(state);
		AddRoundKey(state, keys, round);
		InvMixColumns(state);
	}
	InvShiftRows(state);
	InvSubBytes(state);
	AddRoundKey(state, keys, 0);
}

// ---- XML, as far as a trainer needs it

struct XmlElement {
	std::string                                      name;
	std::vector<std::pair<std::string, std::string>> attributes;
	std::vector<XmlElement>                          children;
	// The character data directly inside the element (CDATA sections included), in order.
	std::string text;
};

// Deeper elements make a document invalid; a trainer has four levels.
constexpr int kMaxXmlDepth = 256;

// Reads the root element of a document up to its end tag and nothing after it. It checks what
// it reads: the prolog (an XML declaration or processing instructions, comments, one document type
// declaration, whitespace), the tags and their names, unique attributes with quoted values, matched
// end tags, the five predefined entities and character references, comments, CDATA sections, and
// processing instructions. Encodings other than UTF-8 are not supported. Line breaks become LF in
// text, and tabs and line breaks become spaces in attribute values (XML's normalization).
class XmlReader {
public:
	explicit XmlReader(std::string_view text): m_text(text) {}

	std::optional<XmlElement> ReadRoot() {
		constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
		if (m_text.starts_with(kByteOrderMark)) {
			m_pos = kByteOrderMark.size();
		}
		bool doctype = false;
		while (true) {
			SkipSpace();
			if (AtEnd()) {
				return std::nullopt;
			}
			if (StartsWith("<?")) {
				if (!SkipPast("?>")) {
					return std::nullopt;
				}
			} else if (StartsWith("<!--")) {
				if (!SkipPast("-->")) {
					return std::nullopt;
				}
			} else if (StartsWith("<!DOCTYPE")) {
				if (doctype || !SkipDoctype()) {
					return std::nullopt;
				}
				doctype = true;
			} else if (m_text[m_pos] == '<') {
				XmlElement root;
				if (!ReadElement(root, 0)) {
					return std::nullopt;
				}
				return root;
			} else {
				return std::nullopt;
			}
		}
	}

private:
	[[nodiscard]] bool AtEnd() const { return m_pos >= m_text.size(); }
	[[nodiscard]] bool StartsWith(std::string_view prefix) const {
		return m_text.substr(m_pos).starts_with(prefix);
	}
	[[nodiscard]] bool At(char c) const { return !AtEnd() && m_text[m_pos] == c; }

	static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

	static bool IsNameStart(char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' ||
		       static_cast<unsigned char>(c) >= 0x80;
	}

	static bool IsNameChar(char c) {
		return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
	}

	void SkipSpace() {
		while (!AtEnd() && IsSpace(m_text[m_pos])) {
			m_pos++;
		}
	}

	// Moves past the next terminator; false when there is none.
	bool SkipPast(std::string_view terminator) {
		const auto end = m_text.find(terminator, m_pos);
		if (end == std::string_view::npos) {
			return false;
		}
		m_pos = end + terminator.size();
		return true;
	}

	// At "<!DOCTYPE": moves past its '>', skipping quoted strings and an internal subset.
	bool SkipDoctype() {
		m_pos += 2;
		char quote = 0;
		int  depth = 0;
		while (!AtEnd()) {
			const char c = m_text[m_pos++];
			if (quote != 0) {
				quote = c == quote ? 0 : quote;
			} else if (c == '"' || c == '\'') {
				quote = c;
			} else if (c == '[') {
				depth++;
			} else if (c == ']') {
				depth--;
			} else if (c == '>' && depth == 0) {
				return true;
			}
		}
		return false;
	}

	bool ReadName(std::string& name) {
		const auto start = m_pos;
		if (AtEnd() || !IsNameStart(m_text[m_pos])) {
			return false;
		}
		while (!AtEnd() && IsNameChar(m_text[m_pos])) {
			m_pos++;
		}
		name = std::string(m_text.substr(start, m_pos - start));
		return true;
	}

	static void AppendUtf8(std::string& out, uint32_t code_point) {
		if (code_point < 0x80) {
			out += static_cast<char>(code_point);
		} else if (code_point < 0x800) {
			out += static_cast<char>(0xC0u | (code_point >> 6u));
			out += static_cast<char>(0x80u | (code_point & 0x3Fu));
		} else if (code_point < 0x10000) {
			out += static_cast<char>(0xE0u | (code_point >> 12u));
			out += static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu));
			out += static_cast<char>(0x80u | (code_point & 0x3Fu));
		} else {
			out += static_cast<char>(0xF0u | (code_point >> 18u));
			out += static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu));
			out += static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu));
			out += static_cast<char>(0x80u | (code_point & 0x3Fu));
		}
	}

	// The characters of XML's Char production.
	static bool IsXmlChar(uint32_t c) {
		return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
		       (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
	}

	// At '&': appends the character of a predefined entity or a character reference.
	bool ReadReference(std::string& out) {
		constexpr std::size_t kMaxReference = 12; // "#x" and up to 8 digits, or a name
		const auto            end           = m_text.find(';', m_pos + 1);
		if (end == std::string_view::npos || end - m_pos - 1 > kMaxReference) {
			return false;
		}
		const auto reference = m_text.substr(m_pos + 1, end - m_pos - 1);
		m_pos                = end + 1;
		static constexpr std::pair<std::string_view, char> kEntities[] = {
		    {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};
		for (const auto& [name, c]: kEntities) {
			if (reference == name) {
				out += c;
				return true;
			}
		}
		if (reference.size() < 2 || reference[0] != '#') {
			return false;
		}
		const bool hex    = reference[1] == 'x';
		const auto digits = reference.substr(hex ? 2 : 1);
		if (digits.empty() || digits.size() > 8) {
			return false;
		}
		uint64_t code_point = 0;
		for (const char c: digits) {
			unsigned digit = 0;
			if (c >= '0' && c <= '9') {
				digit = static_cast<unsigned>(c - '0');
			} else if (hex && c >= 'a' && c <= 'f') {
				digit = static_cast<unsigned>(c - 'a' + 10);
			} else if (hex && c >= 'A' && c <= 'F') {
				digit = static_cast<unsigned>(c - 'A' + 10);
			} else {
				return false;
			}
			code_point = code_point * (hex ? 16u : 10u) + digit;
		}
		if (code_point > 0x10FFFF || !IsXmlChar(static_cast<uint32_t>(code_point))) {
			return false;
		}
		AppendUtf8(out, static_cast<uint32_t>(code_point));
		return true;
	}

	// At the opening quote.
	bool ReadAttributeValue(std::string& value) {
		const char quote = m_text[m_pos++];
		while (!AtEnd()) {
			const char c = m_text[m_pos];
			if (c == quote) {
				m_pos++;
				return true;
			}
			if (c == '<') {
				return false;
			}
			if (c == '&') {
				if (!ReadReference(value)) {
					return false;
				}
				continue;
			}
			m_pos++;
			if (c == '\r' && At('\n')) {
				m_pos++;
			}
			value += IsSpace(c) ? ' ' : c;
		}
		return false;
	}

	// At the '<' of a start tag: the element with its attributes and content, up to its end tag.
	bool ReadElement(XmlElement& element, int depth) {
		if (depth > kMaxXmlDepth) {
			return false;
		}
		m_pos++;
		if (!ReadName(element.name)) {
			return false;
		}
		while (true) {
			const auto before = m_pos;
			SkipSpace();
			if (StartsWith("/>")) {
				m_pos += 2;
				return true;
			}
			if (At('>')) {
				m_pos++;
				break;
			}
			std::string key;
			std::string value;
			// An attribute follows whitespace.
			if (m_pos == before || !ReadName(key)) {
				return false;
			}
			SkipSpace();
			if (!At('=')) {
				return false;
			}
			m_pos++;
			SkipSpace();
			if (!At('"') && !At('\'')) {
				return false;
			}
			if (!ReadAttributeValue(value)) {
				return false;
			}
			const bool repeated =
			    std::any_of(element.attributes.begin(), element.attributes.end(),
			                [&key](const auto& other) { return other.first == key; });
			if (repeated) {
				return false;
			}
			element.attributes.emplace_back(std::move(key), std::move(value));
		}
		while (!AtEnd()) {
			const char c = m_text[m_pos];
			if (c == '&') {
				if (!ReadReference(element.text)) {
					return false;
				}
			} else if (c != '<') {
				m_pos++;
				if (c == '\r' && At('\n')) {
					m_pos++;
				}
				element.text += c == '\r' ? '\n' : c;
			} else if (StartsWith("</")) {
				m_pos += 2;
				std::string name;
				if (!ReadName(name) || name != element.name) {
					return false;
				}
				SkipSpace();
				if (!At('>')) {
					return false;
				}
				m_pos++;
				return true;
			} else if (StartsWith("<!--")) {
				if (!SkipPast("-->")) {
					return false;
				}
			} else if (StartsWith("<![CDATA[")) {
				m_pos += 9;
				const auto end = m_text.find("]]>", m_pos);
				if (end == std::string_view::npos) {
					return false;
				}
				element.text.append(m_text.substr(m_pos, end - m_pos));
				m_pos = end + 3;
			} else if (StartsWith("<?")) {
				if (!SkipPast("?>")) {
					return false;
				}
			} else {
				XmlElement child;
				if (!ReadElement(child, depth + 1)) {
					return false;
				}
				element.children.push_back(std::move(child));
			}
		}
		return false;
	}

	std::string_view m_text;
	std::size_t      m_pos = 0;
};

// The attribute's value as valid UTF-8; "" when the element has none (QXmlStreamAttributes::value).
std::string Attribute(const XmlElement& element, std::string_view key) {
	for (const auto& [name, value]: element.attributes) {
		if (name == key) {
			return SanitizeUtf8(value);
		}
	}
	return {};
}

std::string WithoutDashes(std::string text) {
	text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
	return text;
}

} // namespace

std::string DecodeBase64Lenient(std::string_view text) {
	std::string bytes;
	bytes.reserve(text.size() / 4 * 3 + 3);
	uint32_t bits  = 0;
	unsigned count = 0;
	for (const char c: text) {
		uint32_t value = 0;
		if (c >= 'A' && c <= 'Z') {
			value = static_cast<uint32_t>(c - 'A');
		} else if (c >= 'a' && c <= 'z') {
			value = static_cast<uint32_t>(c - 'a' + 26);
		} else if (c >= '0' && c <= '9') {
			value = static_cast<uint32_t>(c - '0' + 52);
		} else if (c == '+') {
			value = 62;
		} else if (c == '/') {
			value = 63;
		} else {
			continue;
		}
		bits = (bits << 6u) | value;
		count += 6;
		if (count >= 8) {
			count -= 8;
			bytes += static_cast<char>((bits >> count) & 0xFFu);
			bits &= (1u << count) - 1u;
		}
	}
	return bytes;
}

void DecryptAes256Cbc(std::span<uint8_t> data, const std::array<uint8_t, 32>& key,
                      const std::array<uint8_t, 16>& iv) {
	const auto keys     = ExpandKey(key);
	AesBlock   previous = iv;
	for (std::size_t offset = 0; offset + 16 <= data.size(); offset += 16) {
		AesBlock cipher;
		std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), 16, cipher.begin());
		AesBlock plain = cipher;
		DecryptBlock(plain, keys);
		for (std::size_t i = 0; i < plain.size(); i++) {
			data[offset + i] = static_cast<uint8_t>(plain[i] ^ previous[i]);
		}
		previous = cipher;
	}
}

std::optional<std::string> ConvertCheatTrainer(std::string_view bytes, bool encrypted) {
	std::string decrypted;
	if (encrypted) {
		decrypted = DecodeBase64Lenient(bytes);
		std::array<uint8_t, 32> key {};
		std::array<uint8_t, 16> iv {};
		std::copy(kMc4Key.begin(), kMc4Key.end(), key.begin());
		std::copy(kMc4Iv.begin(), kMc4Iv.end(), iv.begin());
		DecryptAes256Cbc(std::span(reinterpret_cast<uint8_t*>(decrypted.data()), decrypted.size()),
		                 key, iv);
		bytes = decrypted;
	}
	const auto root = XmlReader(bytes).ReadRoot();
	if (!root || root->name != "Trainer") {
		return std::nullopt;
	}
	auto mods = OrderedJson::array();
	for (const auto& cheat: root->children) {
		if (cheat.name != "Cheat") {
			continue;
		}
		auto memory = OrderedJson::array();
		for (const auto& line: cheat.children) {
			auto write = OrderedJson::object();
			for (const auto& field: line.children) {
				// Qt's readElementText() fails on an element inside a value.
				if (!field.children.empty()) {
					return std::nullopt;
				}
				const auto value = std::string(TrimWhitespace(SanitizeUtf8(field.text)));
				if (field.name == "Offset") {
					write["offset"] = value;
				} else if (field.name == "ValueOn") {
					write["on"] = WithoutDashes(value);
				} else if (field.name == "ValueOff") {
					write["off"] = WithoutDashes(value);
				} else if (field.name == "Section" && value != "0") {
					// Section-relative and absolute writes are refused on import.
					write["section"] = value;
				} else if (field.name == "Absolute") {
					write["absolute"] = true;
				}
			}
			memory.push_back(std::move(write));
		}
		auto mod      = OrderedJson::object();
		mod["name"]   = Attribute(cheat, "Text");
		mod["memory"] = std::move(memory);
		mods.push_back(std::move(mod));
	}
	auto document       = OrderedJson::object();
	document["name"]    = Attribute(*root, "Game");
	document["id"]      = Attribute(*root, "Cusa");
	document["version"] = Attribute(*root, "Version");
	document["process"] = Attribute(*root, "Process");
	document["credits"] = OrderedJson::array({Attribute(*root, "Moder")});
	document["mods"]    = std::move(mods);
	return document.dump(-1, ' ', false, OrderedJson::error_handler_t::replace);
}

Expected<CheatDocument, CheatProblem> ParseCheatTrainer(std::string_view bytes, bool encrypted,
                                                        std::string_view file_name) {
	const auto json = ConvertCheatTrainer(bytes, encrypted);
	if (!json) {
		CheatProblem problem;
		problem.kind    = CheatProblemKind::InvalidXml;
		problem.file    = std::string(file_name);
		problem.message = problem.file + ": Invalid cheat XML.";
		return MakeUnexpected(std::move(problem));
	}
	return ParseCheatDocument(*json, file_name);
}

} // namespace kyty::core
