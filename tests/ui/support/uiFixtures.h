#pragma once

// Game fixtures of the UI tests (task 22.1; phase 7 plan D16), ported from the phase 6 visual
// harness. They write into a case's temporary tree (uiContext.h) and fail the case (uiHarness.h)
// when something cannot be written.

#include <filesystem>
#include <string>
#include <string_view>

namespace KytyUi {

// A param.json with the title, the title ID, the version, and a system software version.
std::string ParamJson(std::string_view title, std::string_view title_id, std::string_view version);

// A width x height PNG with a diagonal gradient in one of six hues (0-5) and a light frame.
bool WritePng(const std::filesystem::path& path, int width, int height, int hue);

// The fixture library under <root>/games, returned (empty after a Fail):
// - "Alpha Game" (PPSA01001, 01.000) and "Beta/Beta Quest" (PPSA01002, 01.020), with a 512x512
//   icon0.png and a 960x540 pic0.png each;
// - a game in a directory named with quotes, a backslash, and Unicode (PPSA01003, 02.000);
// - "Broken Metadata", whose param.json does not parse (the title falls back to the directory
//   name, Requirement 7.9);
// - "Gamma Archive.zar" (PPSA01004), packed from a staging directory through core::ArchiveJob.
std::filesystem::path WriteFixtures(const std::filesystem::path& root);

// count generated games in folder: "<folder>/Synthetic Game <n>" titled "Synthetic Game <n>" with
// title ID PPSA2<nnnn> (n from 1), each with param.json and eboot.bin, and with_icons a 64x64
// icon0.png. Returns the number written (count unless a write failed, after a Fail).
int WriteSyntheticGames(const std::filesystem::path& folder, int count, bool with_icons = false);

} // namespace KytyUi
