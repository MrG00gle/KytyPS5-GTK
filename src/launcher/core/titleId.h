#pragma once

// Title IDs as the launcher compares and stores them. Game_Settings_File keys
// (gameSettingsFile.h), Compatibility_Database keys (compatibility.h), and cheat files (cheats.h)
// all use NormalizeTitleId, so one title ID matches in every place.

#include <string>
#include <string_view>

namespace kyty::core {

// The title ID with TrimWhitespace applied, then its ASCII letters upper-cased; other bytes are
// kept ("  ppsa01234 " gives "PPSA01234").
std::string NormalizeTitleId(std::string_view title_id);

} // namespace kyty::core
