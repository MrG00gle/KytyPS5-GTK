#include "source_check/platformMacroScanner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace KytyTests::SourceCheck {

namespace {

// Built from adjacent string literals so that this file does not contain the names (D30).
constexpr std::array<std::string_view, 7> kForbiddenMacros = {
    "_WIN"
    "32",
    "WIN"
    "32",
    "KYTY_PLATFORM_"
    "WINDOWS",
    "_MSC"
    "_VER",
    "__MINGW"
    "32__",
    "__MINGW"
    "64__",
    "__APP"
    "LE__",
};

constexpr std::array<std::string_view, 6> kConditionalDirectives = {
    "if", "ifdef", "ifndef", "elif", "elifdef", "elifndef",
};

constexpr size_t kBinaryProbeSize = 8192;

bool IsIdentifierStart(char c) {
	const auto u = static_cast<unsigned char>(c);
	return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u == '$' || u >= 0x80;
}

bool IsIdentifierChar(char c) {
	return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

// Whitespace other than the newline.
bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

template <size_t N>
bool Contains(const std::array<std::string_view, N>& values, std::string_view value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

// The text after line splicing (translation phase 2), with the physical offset and line of every
// remaining character.
struct Spliced {
	std::string           text;
	std::vector<size_t>   offset;
	std::vector<uint32_t> line;
};

// Length of the splice at physical offset p (a backslash, optional spaces, tabs, and carriage
// returns, then a newline), or 0.
size_t SpliceLength(std::string_view text, size_t p) {
	if (p >= text.size() || text[p] != '\\') {
		return 0;
	}
	size_t q = p + 1;
	while (q < text.size() && (text[q] == ' ' || text[q] == '\t' || text[q] == '\r')) {
		q++;
	}
	return q < text.size() && text[q] == '\n' ? q + 1 - p : 0;
}

Spliced Splice(std::string_view text) {
	Spliced  s;
	size_t   p    = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
	uint32_t line = 1;
	s.text.reserve(text.size());
	s.offset.reserve(text.size());
	s.line.reserve(text.size());
	while (p < text.size()) {
		if (const size_t length = SpliceLength(text, p); length != 0) {
			p += length;
			line++;
			continue;
		}
		s.text.push_back(text[p]);
		s.offset.push_back(p);
		s.line.push_back(line);
		if (text[p] == '\n') {
			line++;
		}
		p++;
	}
	return s;
}

class Scanner {
public:
	Scanner(std::string_view path, std::string_view physical)
	    : m_path(path), m_physical(physical), m_s(Splice(physical)), m_t(m_s.text) {}

	std::vector<Finding> Run() {
		bool line_start = true;
		while (m_i < m_t.size()) {
			const char c = m_t[m_i];
			if (c == '\n') {
				EndDirective();
				line_start = true;
				m_i++;
			} else if (IsSpace(c)) {
				m_i++;
			} else if (c == '/' && Next() == '/') {
				SkipLineComment();
			} else if (c == '/' && Next() == '*') {
				SkipBlockComment();
			} else if (c == '#' && line_start) {
				line_start = false;
				StartDirective();
			} else {
				line_start = false;
				LexToken();
			}
		}
		EndDirective();
		return std::move(m_findings);
	}

private:
	char Next() const { return m_i + 1 < m_t.size() ? m_t[m_i + 1] : '\0'; }

	// Leaves m_i at the newline that ends the comment (or at the end).
	void SkipLineComment() {
		while (m_i < m_t.size() && m_t[m_i] != '\n') {
			m_i++;
		}
	}

	// An unterminated block comment ends at the end of the text.
	void SkipBlockComment() {
		const size_t end = m_t.find("*/", m_i + 2);
		m_i              = end == std::string::npos ? m_t.size() : end + 2;
	}

	void StartDirective() {
		m_directive_start = m_i;
		m_in_directive    = true;
		m_conditional     = false;
		m_matched         = false;
		m_i++;
		// Whitespace and comments may separate '#' from the directive name.
		while (m_i < m_t.size()) {
			if (IsSpace(m_t[m_i])) {
				m_i++;
			} else if (m_t[m_i] == '/' && Next() == '*') {
				SkipBlockComment();
			} else {
				break;
			}
		}
		if (m_i < m_t.size() && IsIdentifierStart(m_t[m_i])) {
			const size_t begin = m_i;
			while (m_i < m_t.size() && IsIdentifierChar(m_t[m_i])) {
				m_i++;
			}
			m_conditional =
			    Contains(kConditionalDirectives, std::string_view(m_t).substr(begin, m_i - begin));
		}
	}

	void EndDirective() {
		if (m_in_directive && m_matched) {
			std::string text = m_t.substr(m_directive_start, m_i - m_directive_start);
			std::replace_if(
			    text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
			while (!text.empty() && IsSpace(text.back())) {
				text.pop_back();
			}
			m_findings.push_back(
			    Finding {std::string(m_path), m_s.line[m_directive_start], std::move(text)});
		}
		m_in_directive = false;
	}

	void LexToken() {
		const char c = m_t[m_i];
		if (IsIdentifierStart(c)) {
			LexIdentifierOrPrefixedLiteral();
		} else if (IsDigit(c) || (c == '.' && IsDigit(Next()))) {
			LexPpNumber();
		} else if (c == '"' || c == '\'') {
			LexQuoted();
		} else {
			m_i++;
		}
	}

	void LexIdentifierOrPrefixedLiteral() {
		const size_t begin = m_i;
		while (m_i < m_t.size() && IsIdentifierChar(m_t[m_i])) {
			m_i++;
		}
		const std::string_view identifier = std::string_view(m_t).substr(begin, m_i - begin);
		const char             next       = m_i < m_t.size() ? m_t[m_i] : '\0';
		if (next == '"' && (identifier == "R" || identifier == "LR" || identifier == "uR" ||
		                    identifier == "UR" || identifier == "u8R")) {
			LexRawString();
			return;
		}
		if ((next == '"' || next == '\'') &&
		    (identifier == "L" || identifier == "u" || identifier == "U" || identifier == "u8")) {
			LexQuoted();
			return;
		}
		if (m_in_directive && m_conditional && Contains(kForbiddenMacros, identifier)) {
			m_matched = true;
		}
	}

	// [0-9] or .[0-9], then identifier characters, '.', a sign after e, E, p, or P, and digit
	// separators (' followed by an identifier character).
	void LexPpNumber() {
		m_i++;
		while (m_i < m_t.size()) {
			const char c    = m_t[m_i];
			const char prev = m_t[m_i - 1];
			if ((c == '+' || c == '-') &&
			    (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
				m_i++;
			} else if (c == '\'' && IsIdentifierChar(Next())) {
				m_i += 2;
			} else if (IsIdentifierChar(c) || c == '.') {
				m_i++;
			} else {
				break;
			}
		}
	}

	// A string or character literal at m_i. It ends at the closing quote or, unterminated, before
	// the newline.
	void LexQuoted() {
		const char quote = m_t[m_i];
		m_i++;
		while (m_i < m_t.size()) {
			const char c = m_t[m_i];
			if (c == '\\' && m_i + 1 < m_t.size() && m_t[m_i + 1] != '\n') {
				m_i += 2;
			} else if (c == quote) {
				m_i++;
				return;
			} else if (c == '\n') {
				return;
			} else {
				m_i++;
			}
		}
	}

	// A raw string whose opening quote is at m_i. Splices are reverted inside raw strings, so the
	// delimiter and the end are found in the physical text. Without a valid delimiter it is lexed
	// as an ordinary string; unterminated, it runs to the end of the text.
	void LexRawString() {
		const size_t quote = m_s.offset[m_i];
		size_t       p     = quote + 1;
		while (p < m_physical.size() && p - quote - 1 <= 16) {
			const char c = m_physical[p];
			if (c == '(') {
				break;
			}
			if (c == ' ' || c == ')' || c == '\\' || c == '\t' || c == '\v' || c == '\f' ||
			    c == '\n' || c == '"') {
				break;
			}
			p++;
		}
		if (p >= m_physical.size() || m_physical[p] != '(' || p - quote - 1 > 16) {
			LexQuoted();
			return;
		}
		const std::string closing =
		    ")" + std::string(m_physical.substr(quote + 1, p - quote - 1)) + "\"";
		const size_t found = m_physical.find(closing, p + 1);
		const size_t end =
		    found == std::string_view::npos ? m_physical.size() : found + closing.size();
		m_i = static_cast<size_t>(std::lower_bound(m_s.offset.begin(), m_s.offset.end(), end) -
		                          m_s.offset.begin());
	}

	std::string_view     m_path;
	std::string_view     m_physical;
	Spliced              m_s;
	const std::string&   m_t;
	size_t               m_i               = 0;
	bool                 m_in_directive    = false;
	bool                 m_conditional     = false;
	bool                 m_matched         = false;
	size_t               m_directive_start = 0;
	std::vector<Finding> m_findings;
};

void AppendError(std::string* error, const std::string& message) {
	if (error == nullptr) {
		return;
	}
	if (!error->empty()) {
		*error += '\n';
	}
	*error += message;
}

} // namespace

std::span<const std::string_view> ForbiddenMacros() {
	return kForbiddenMacros;
}

std::vector<Finding> ScanText(std::string_view path, std::string_view text) {
	return Scanner(path, text).Run();
}

std::vector<Finding> ScanTree(const std::filesystem::path& root,
                              std::span<const std::string> subdirs, std::string* error) {
	namespace fs = std::filesystem;
	std::vector<fs::path> files;
	for (const auto& subdir: subdirs) {
		const fs::path  dir = root / subdir;
		std::error_code ec;
		const auto      status = fs::symlink_status(dir, ec);
		if (ec || !fs::is_directory(status)) {
			AppendError(error,
			            dir.string() + ": not a directory" + (ec ? " (" + ec.message() + ")" : ""));
			continue;
		}
		for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end;
		     !ec && it != end; it.increment(ec)) {
			std::error_code entry_ec;
			const auto      entry_status = it->symlink_status(entry_ec);
			if (entry_ec) {
				AppendError(error, it->path().string() + ": " + entry_ec.message());
			} else if (fs::is_regular_file(entry_status)) {
				files.push_back(it->path());
			}
		}
		if (ec) {
			AppendError(error, dir.string() + ": cannot list the directory (" + ec.message() + ")");
		}
	}
	std::sort(files.begin(), files.end());

	std::vector<Finding> findings;
	for (const auto& file: files) {
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			AppendError(error, file.string() + ": cannot open the file");
			continue;
		}
		const std::string text((std::istreambuf_iterator<char>(in)),
		                       std::istreambuf_iterator<char>());
		if (in.bad()) {
			AppendError(error, file.string() + ": cannot read the file");
			continue;
		}
		if (text.find('\0') < kBinaryProbeSize) {
			continue;
		}
		auto file_findings = ScanText(file.lexically_relative(root).generic_string(), text);
		std::move(file_findings.begin(), file_findings.end(), std::back_inserter(findings));
	}
	return findings;
}

} // namespace KytyTests::SourceCheck
