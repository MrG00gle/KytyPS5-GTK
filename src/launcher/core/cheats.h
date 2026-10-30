#pragma once

// Cheat files (task 16.2; design.md, "Cheat file"; Requirements 11.4 and 11.5): which games have
// cheats and where their cheat file is. The Emulator reads that file with --game-patch
// (emulatorArguments.h). Ported from the Qt launcher's Cheats::IsSupportedTitleId and
// Cheats::PlanPath (src/launcher/src/cheatFile.cpp).

#include <filesystem>
#include <string_view>

namespace kyty::core {

// True when NormalizeTitleId(title_id) (titleId.h) is "PPSA" followed by exactly five ASCII
// digits: the games that get the cheats dialog (Requirement 11.4).
bool IsCheatTitleId(std::string_view title_id);

// The game's cheat file, CheatDirectory(data_dir) / (NormalizeTitleId(title_id) + ".json")
// (dataPaths.h: <data_dir>/_Patches/<TITLE_ID>.json, Requirement 11.5). An empty path when
// IsCheatTitleId(title_id) is false, as Qt's PlanPath, so no other title ID names a file.
std::filesystem::path CheatFilePath(const std::filesystem::path& data_dir,
                                    std::string_view             title_id);

} // namespace kyty::core
