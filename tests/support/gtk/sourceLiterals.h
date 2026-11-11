#pragma once

// The msgids of a GTK launcher source file, read from the source: the first string literal of
// every _(), N_(), NGettext(), and NGettextNoop() call (adjacent literals joined; the escapes these
// sources use: \" \\ \n), and the second one of the plural calls. Comments are skipped. The GTK
// mapping tests check with it that a text they show is one a translator gets (phase 7 plan D28,
// phase 9 D8).

#include <filesystem>
#include <set>
#include <string>

namespace KytyTests {

// Every msgid of the file; aborts with a message when the file cannot be read.
std::set<std::string> TranslatableLiterals(const std::filesystem::path& file);

} // namespace KytyTests
