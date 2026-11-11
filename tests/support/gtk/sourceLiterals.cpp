#include "support/gtk/sourceLiterals.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

namespace KytyTests {

namespace {

bool IsIdentifier(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Skips white space and comments from position.
size_t SkipSpace(const std::string& text, size_t position) {
	while (position < text.size()) {
		if (std::isspace(static_cast<unsigned char>(text[position])) != 0) {
			position++;
		} else if (text.compare(position, 2, "//") == 0) {
			position = text.find('\n', position);
			position = position == std::string::npos ? text.size() : position;
		} else if (text.compare(position, 2, "/*") == 0) {
			position = text.find("*/", position);
			position = position == std::string::npos ? text.size() : position + 2;
		} else {
			break;
		}
	}
	return position;
}

// The string literal at position (adjacent literals joined), and the position after it; nullopt
// when no literal starts there.
std::optional<std::pair<std::string, size_t>> Literal(const std::string& text, size_t position) {
	std::string value;
	bool        found = false;
	position          = SkipSpace(text, position);
	while (position < text.size() && text[position] == '"') {
		found = true;
		for (position++; position < text.size() && text[position] != '"'; position++) {
			if (text[position] == '\\' && position + 1 < text.size()) {
				const char escaped = text[++position];
				value += escaped == 'n' ? '\n' : escaped;
			} else {
				value += text[position];
			}
		}
		position = SkipSpace(text, position + 1);
	}
	if (!found) {
		return std::nullopt;
	}
	return std::make_pair(value, position);
}

} // namespace

std::set<std::string> TranslatableLiterals(const std::filesystem::path& file) {
	std::ifstream input(file, std::ios::binary);
	if (!input.good()) {
		std::fprintf(stderr, "TranslatableLiterals: cannot read %s\n", file.c_str());
		std::abort();
	}
	std::stringstream buffer;
	buffer << input.rdbuf();
	const std::string     text = buffer.str();
	std::set<std::string> literals;
	for (size_t i = 0; i < text.size(); i++) {
		if (text.compare(i, 2, "//") == 0 || text.compare(i, 2, "/*") == 0) {
			i = SkipSpace(text, i) - 1;
			continue;
		}
		if (text[i] == '"' || text[i] == '\'') {
			// A literal outside a translation call: skip it, escapes included.
			const char quote = text[i];
			for (i++; i < text.size() && text[i] != quote; i++) {
				i += text[i] == '\\' ? 1 : 0;
			}
			continue;
		}
		if (!IsIdentifier(text[i]) || (i > 0 && IsIdentifier(text[i - 1]))) {
			continue;
		}
		size_t end = i;
		while (end < text.size() && IsIdentifier(text[end])) {
			end++;
		}
		const std::string name = text.substr(i, end - i);
		const size_t      open = SkipSpace(text, end);
		const bool        known =
		    name == "_" || name == "N_" || name == "NGettext" || name == "NGettextNoop";
		if (!known || open >= text.size() || text[open] != '(') {
			i = end - 1;
			continue;
		}
		auto first = Literal(text, open + 1);
		if (first) {
			literals.insert(first->first);
			if ((name == "NGettext" || name == "NGettextNoop") && first->second < text.size() &&
			    text[first->second] == ',') {
				if (auto second = Literal(text, first->second + 1)) {
					literals.insert(second->first);
				}
			}
		}
		i = end - 1;
	}
	return literals;
}

} // namespace KytyTests
