#pragma once

// Platform macro source check (Requirements 5.7, 5.10). The scanner reports every #if, #ifdef,
// #ifndef, #elif, #elifdef, and #elifndef directive whose condition names one of the seven macros
// of Requirement 5.7 as a whole identifier.
//
// It is a textual scan, not a preprocessor:
// - Backslash-newline splices are removed first (a backslash followed by spaces, tabs, or carriage
//   returns and then a newline is a splice too, as in Clang and GCC); physical line numbers are
//   kept.
// - Line and block comments, string and character literals (with escapes and encoding prefixes),
//   raw strings (no splicing inside them), and pp-numbers (so 1'000 opens no character literal) are
//   lexed and never searched.
// - A directive is a '#' that is the first token of a logical line; comments may come before or
//   after it. Its condition is the rest of the logical line without comments and literals.
//   Identifier characters are [A-Za-z0-9_$] and every byte >= 0x80.
// - The digraph %: is not recognized. Conditionals inside skipped groups are reported as well.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace KytyTests::SourceCheck {

// The seven macro names of Requirement 5.7. Tests use this list instead of spelling the names, so
// that no file under src/ or tests/ contains them as whole words.
std::span<const std::string_view> ForbiddenMacros();

struct Finding {
	std::string path;      // as passed to ScanText(), or relative to the ScanTree() root
	uint32_t    line = 0;  // 1-based physical line of the '#'
	std::string directive; // the logical directive line, newlines inside comments shown as spaces
};

// Scans one file's contents. A leading UTF-8 byte order mark is skipped.
std::vector<Finding> ScanText(std::string_view path, std::string_view text);

// Scans every regular file under root/<subdir> for each subdir, without following symlinks, and
// skips files with a NUL byte in their first 8 KiB. Files are scanned in path order. I/O errors
// (a missing subdir, an unreadable directory or file) are appended to *error, one per line, and the
// scan continues with the next file or subdir.
std::vector<Finding> ScanTree(const std::filesystem::path& root,
                              std::span<const std::string> subdirs, std::string* error);

} // namespace KytyTests::SourceCheck
