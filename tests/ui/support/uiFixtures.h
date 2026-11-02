#pragma once

// Game fixtures of the UI tests (task 22.1; phase 7 plan D16), ported from the phase 6 visual
// harness. They write into a case's temporary tree (uiContext.h) and fail the case (uiHarness.h)
// when something cannot be written.

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// The content of Alpha Game (PPSA01001 of WriteFixtures(), below games) for the content windows,
// built with KytyTests::ContentGen; false after a Fail:
// - the trophy package sce_sys/trophy2/trophy00.ucp of Alpha Game ("Alpha Game Trophies"): "Gold
//   Rush" (Gold, earned: its unlock file for user_id is in data_dir), "Silver Lining" (Silver, not
//   earned), and the hidden "Secret Ending" (Bronze, not earned: shown masked, with a Reveal
//   button);
// - the add-on package <addons>/alpha-soundtrack ("Alpha Soundtrack", with an icon);
// - the local cheat file <data_dir>/_Patches/PPSA01001.json with the cheats "Infinite Health"
//   (enabled) and "Max Coins" (disabled).
bool WriteGameContent(const std::filesystem::path& games, const std::filesystem::path& data_dir,
                      const std::filesystem::path& addons, int user_id);

// The HEN Cheats Collection indexes for WriteGameContent's Alpha Game, as (core::CheatIndexUrl,
// body) pairs: the JSON index lists Alpha Game's files PPSA01001_01.020_alpha.json and
// PPSA01001_01.000_alpha.json and a file of Beta Quest; the MC4 and the SHN index one file each of
// Alpha Game (listed, but disabled: only JSON files can be previewed).
std::vector<std::pair<std::string, std::string>> CheatIndexes();

// A control that WriteGameContent's content (and CheatIndexes()) gives a registry window, such as
// a trophy row or a cheat's check box: page is the view stack page that shows it ("" for a window
// without a view stack), what says what it is, and match tells it among the window's interactive
// controls (uiDriver.h InteractiveControls; by type and ControlName).
struct ContentControl {
	std::string                     page;
	std::string                     what;
	std::function<bool(GtkWidget*)> match;
};

// The content controls of the registry window id (dialogRegistry.h) opened for Alpha Game
// ("trophy-window", "additional-content-window", "cheats-window") or over a library with it
// ("trophy-overview-window"); empty for every other id.
std::vector<ContentControl> GameContentControls(std::string_view id);

} // namespace KytyUi
