// Example tests of the Settings_Store (src/launcher/core/settingsStore.h, task 13.3; Requirements
// 7.11, 7.17, 9.2, 9.3, 9.6, 9.7, 9.13, 12.8): each Qt fixture in tests/fixtures/settings loads
// with the values the fixtures and the phase 1 report state and no defaulted key; a re-save keeps
// every key, including unknown keys and groups; missing, unparsable, and invalid values get their
// defaults and are reported, optional keys are not; injected faults at each AtomicFileStep leave
// the existing file byte-identical; the custom settings lifecycle; and
// ResolveEffectiveConfiguration.

#include "launcher/core/atomicFile.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/iniFile.h"
#include "launcher/core/settingsStore.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::AtomicFileStep;
using kyty::core::ControllerSettings;
using kyty::core::GameConfiguration;
using kyty::core::GameEntry;
using kyty::core::GameSettingsEntry;
using kyty::core::IniFile;
using kyty::core::IniValue;
using kyty::core::LauncherSettings;
using kyty::core::LoadReport;
using kyty::core::LogDirection;
using kyty::core::PresentMode;
using kyty::core::ScreenResolution;
using kyty::core::ShaderOptimizationType;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "SettingsStoreTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Join(const std::vector<std::string>& values) {
	std::string text;
	for (const auto& value: values) {
		text += "[" + value + "]";
	}
	return text;
}

fs::path FixturePath(const std::string& name) {
	return fs::path(KYTY_TEST_SOURCE_DIR) / "tests/fixtures/settings" / name;
}

std::string ReadBytes(const fs::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "cannot read " + path.string());
	return *bytes;
}

std::vector<std::string> Sorted(std::vector<std::string> values) {
	std::sort(values.begin(), values.end());
	return values;
}

void CheckDefaulted(const LoadReport& report, std::vector<std::string> expected,
                    const std::string& what) {
	Check(Sorted(report.defaulted_keys) == Sorted(std::move(expected)),
	      what + ": defaulted keys " + Join(report.defaulted_keys));
}

LoadReport LoadFixture(const std::string& name) {
	auto report = kyty::core::LoadSettings(FixturePath(name));
	Check(report.has_value(), name + " loads");
	CheckDefaulted(*report, {}, name);
	return std::move(*report);
}

// The fixture with edit applied to its keys, loaded.
LoadReport LoadEdited(const std::string& name, const std::function<void(IniFile&)>& edit) {
	auto file = IniFile::Parse(ReadBytes(FixturePath(name)));
	edit(file);
	return kyty::core::ParseSettings(file.Serialize());
}

GameEntry Entry(const std::string& path, const std::string& legacy = {},
                const std::string& title = "Game") {
	GameEntry entry;
	entry.path                 = path;
	entry.title_name           = title;
	entry.legacy_relative_path = legacy;
	return entry;
}

const std::vector<std::string> kMapping = {"Cross=J",        "Circle=L",        "Square=K",
                                           "Triangle=I",     "L3=Left Shift",   "R3=Left Ctrl",
                                           "Options=Return", "Pad:Cross=South", "Pad:Circle=East"};
const std::vector<std::string> kSpecialMapping = {"Cross=J",  "L3=Left Shift", "Square=,",
                                                  "Circle=;", "Triangle==",    "R3=\\",
                                                  "L1='",     "TouchPad=\"",   "@Share=\xC3\xBC"};

// The Qt launcher writes these four into [GlobalConfiguration] too; they are unknown keys here.
void CheckQtGlobalExtras(const LauncherSettings& settings, const std::string& what) {
	for (const char* key: {"GlobalConfiguration/basedir", "GlobalConfiguration/game_path",
	                       "GlobalConfiguration/name"}) {
		Check(settings.unknown.GetString(key) == std::optional<std::string>(""),
		      what + ": unknown " + key);
	}
	Check(settings.unknown.GetString("GlobalConfiguration/custom_settings") ==
	          std::optional<std::string>("false"),
	      what + ": unknown GlobalConfiguration/custom_settings");
}

// ---- The Qt fixtures

void TestPortable() {
	const auto  report = LoadFixture("qt6-portable.ini");
	const auto& s      = report.settings;
	Check(s.game_folders == std::vector<std::string> {"/home/user/Games/PS5"}, "portable folders");
	Check(s.addons_dir.empty() && !s.grid_view && s.grid_icon_width == 224, "portable launcher");
	Check(s.check_updates_on_startup && s.theme == std::optional<std::string>("0") &&
	          !s.geometry.has_value(),
	      "portable main dialog");
	Check(s.window == kyty::core::LauncherWindowState {}, "portable window defaults");
	Check(s.global == GameConfiguration {}, "portable global configuration is the default");
	Check(s.controller == ControllerSettings {}, "portable controller defaults");
	Check(s.games.empty(), "portable has no game configuration");
	Check(s.unknown.Entries().size() == 4, "portable unknown keys");
	CheckQtGlobalExtras(s, "portable");
}

void TestUserStore() {
	const auto  report = LoadFixture("qt6-user-store.ini");
	const auto& s      = report.settings;
	Check(s.game_folders ==
	          std::vector<std::string> {"/home/user/Games/PS5", "/mnt/storage/PS5 Games"},
	      "user-store folders");
	Check(s.addons_dir == "/home/user/Games/PS5-Addons", "user-store addons");
	Check(s.grid_view && s.grid_icon_width == 256, "user-store grid");
	Check(s.check_updates_on_startup && s.theme == std::optional<std::string>("1"),
	      "user-store main dialog");
	Check(s.geometry.has_value() && s.geometry->starts_with("@ByteArray(\\x1\\xd9\\xd0\\xcb") &&
	          s.geometry->ends_with("\\x2\\xf7)"),
	      "user-store geometry kept as raw text");
	GameConfiguration global;
	global.host_input_mapping = kMapping;
	Check(s.global == global, "user-store global configuration");
	Check(s.games.size() == 1, "user-store has one game");
	const auto& game = s.games[0];
	Check(game.name == "Astro Bot" && game.basedir == "/home/user/Games/PS5/PPSA01234" &&
	          game.game_path == "/home/user/Games/PS5/PPSA01234" && game.unknown.empty(),
	      "user-store game identity");
	GameConfiguration custom;
	custom.screen_resolution  = ScreenResolution::R1920X1080;
	custom.vblank_frequency   = 120;
	custom.host_input_mapping = kMapping;
	Check(game.configuration == custom, "user-store game configuration");
	Check(s.unknown.Entries().size() == 4, "user-store unknown keys");
}

void TestGlobalAllFields() {
	const auto        report = LoadFixture("qt6-global-all-fields.ini");
	const auto&       s      = report.settings;
	GameConfiguration g;
	g.amd_cpu_enabled             = true;
	g.audio_input_device          = "USB Audio Device Analog Stereo";
	g.command_buffer_dump_enabled = true;
	g.command_buffer_dump_folder  = "logs/command-buffers";
	g.console_language            = 18;
	g.fullscreen_enabled          = true;
	g.gpu_index                   = 1;
	g.hdr_enabled                 = true;
	g.hide_cursor_enabled         = true;
	g.host_input_mapping          = kMapping;
	g.network_connected           = true;
	g.perf_overlay_enabled        = true;
	g.present_mode                = PresentMode::Immediate;
	g.printf_direction            = LogDirection::Console;
	g.printf_output_file          = "logs/kyty-printf.txt";
	g.profiler_enabled            = true;
	g.readback_linear_images      = true;
	g.readback_tiled_images       = true;
	g.renderdoc_enabled           = true;
	g.screen_resolution           = ScreenResolution::R3840X2160;
	g.shader_log_direction        = LogDirection::File;
	g.shader_log_folder           = "logs/shaders";
	g.shader_optimization_type    = ShaderOptimizationType::Size;
	g.shader_validation_enabled   = true;
	g.skip_notice_screen          = true;
	g.sync_raw_image_buffers      = true;
	g.tessellation_enabled        = true;
	g.trophy_duration_seconds     = 9;
	g.trophy_enabled              = false;
	g.user_id                     = 1256;
	g.user_name                   = "Player2";
	g.vblank_frequency            = 144;
	g.vulkan_validation_enabled   = true;
	Check(s.global == g, "all-fields global configuration");
	Check(s.controller == ControllerSettings {"#0070d1", 75, 30, 2}, "all-fields controller");
	Check(s.game_folders == std::vector<std::string> {"/home/user/Games/PS5", "/mnt/storage/PS5",
	                                                  "/run/media/user/USB/PS5"},
	      "all-fields folders");
	Check(s.addons_dir == "/home/user/Games/PS5-Addons" && s.grid_view && s.grid_icon_width == 288,
	      "all-fields launcher");
	Check(!s.check_updates_on_startup && s.theme == std::optional<std::string>("2") &&
	          s.geometry.has_value(),
	      "all-fields main dialog");
	Check(s.games.empty(), "all-fields has no game");
	Check(s.unknown.Entries().size() == 5 &&
	          s.unknown.Find("ConfigurationEditDialog/geometry") != nullptr,
	      "all-fields keeps the settings editor geometry as an unknown key");
	CheckQtGlobalExtras(s, "all-fields");
}

void TestGameEntries() {
	const auto  report = LoadFixture("qt6-game-entries.ini");
	const auto& s      = report.settings;
	Check(s.games.size() == 4, "game-entries has 4 games");

	GameConfiguration zar;
	zar.console_language     = 0;
	zar.printf_direction     = LogDirection::File;
	zar.printf_output_file   = "demons-souls.log";
	zar.screen_resolution    = ScreenResolution::R2560X1440;
	zar.shader_log_direction = LogDirection::Console;
	Check(s.games[0].name == "Demon's Souls" &&
	          s.games[0].game_path == "/home/user/Games/PS5/Demons Souls.zar" &&
	          s.games[0].basedir == "/home/user/Games/PS5/Demons Souls.zar" &&
	          s.games[0].configuration == zar,
	      "game-entries: the .zar entry with an empty mapping");

	GameConfiguration astro;
	astro.gpu_index                = 0;
	astro.hdr_enabled              = true;
	astro.present_mode             = PresentMode::Fifo;
	astro.screen_resolution        = ScreenResolution::R1920X1080;
	astro.shader_optimization_type = ShaderOptimizationType::None;
	astro.trophy_duration_seconds  = 7;
	astro.vblank_frequency         = 120;
	astro.host_input_mapping       = kMapping;
	Check(s.games[1].name == "Astro Bot" &&
	          s.games[1].game_path == "/home/user/Games/PS5/PPSA01234" &&
	          s.games[1].configuration == astro,
	      "game-entries: the custom entry with the global mapping");

	GameConfiguration stale;
	stale.host_input_mapping = kMapping;
	Check(s.games[2].name == "Stale Entry" &&
	          s.games[2].game_path == "/home/user/Games/PS5/PPSA03690" &&
	          s.games[2].configuration == stale,
	      "game-entries: custom_settings=false loads as a custom entry");
	Check(kyty::core::HasCustomSettings(s, Entry("/home/user/Games/PS5/PPSA03690")),
	      "the custom_settings=false entry is custom");

	GameConfiguration legacy;
	legacy.elf                = "bin/eboot_alt.bin";
	legacy.user_id            = 0;
	legacy.host_input_mapping = kMapping;
	Check(s.games[3].name == "Legacy Game" && s.games[3].game_path == "Legacy/PPSA05678" &&
	          s.games[3].basedir == "/home/user/Games/PS5/Legacy/PPSA05678" &&
	          s.games[3].configuration == legacy,
	      "game-entries: the relative-path legacy entry");

	Check(s.game_folders == std::vector<std::string> {"/home/user/Games/PS5"} &&
	          s.addons_dir == "/home/user/Games/PS5-Addons",
	      "game-entries launcher");
	for (const auto& game: s.games) {
		Check(!game.configuration.input_mapping_custom && game.unknown.empty(),
		      "Qt entries have no input_mapping_custom and no unknown key");
	}
}

void TestSpecialValues() {
	const auto        report      = LoadFixture("qt6-special-values.ini");
	const auto&       s           = report.settings;
	const std::string mic         = "Микрофон (USB), \"Front\"; L=R \\ #1";
	const std::string dumps       = "dumps, \"cb\"";
	const std::string printf_file = "@kyty=log;1.txt";
	const std::string shaders     = "logs\\shaders;v=1";
	const std::string user        = "@Kyty, \"=;\\é";

	Check(s.addons_dir == "/games/add-ons, \"DLC\"; a=b\\c ü", "special addons_dir");
	Check(s.game_folders == std::vector<std::string> {"/games/PS5, extra", "/games/\"quoted\" dir",
	                                                  "/games/a=b;c", "/games/back\\slash\\dir",
	                                                  "/games/ゲーム/Ünïcødé", "/games/@at"},
	      "special folders");
	GameConfiguration global;
	global.audio_input_device         = mic;
	global.command_buffer_dump_folder = dumps;
	global.host_input_mapping         = kSpecialMapping;
	global.printf_output_file         = printf_file;
	global.shader_log_folder          = shaders;
	global.user_name                  = user;
	Check(s.global == global, "special global configuration");
	Check(s.controller.color == "#ff8800", "special controller color");

	Check(s.games.size() == 4, "special has 4 games");
	auto expected = global;
	Check(s.games[0].name == "@Leading at" && s.games[0].game_path == "/games/@at/PPSA00001" &&
	          s.games[0].configuration == expected,
	      "special game 1");
	expected.audio_input_device = "@Mic";
	expected.host_input_mapping = {"Square=,"};
	Check(s.games[1].name == "Astro, \"Bot\"; Rescue=1 \\ ✓" &&
	          s.games[1].game_path == "/games/PS5, extra/\"Q\" a=b;c\\d/PPSA09876" &&
	          s.games[1].basedir == s.games[1].game_path && s.games[1].configuration == expected,
	      "special game 2: a one-element list with a comma");
	expected.audio_input_device = mic;
	expected.host_input_mapping = kSpecialMapping;
	Check(s.games[2].name == "Line1\nLine2\tTabbed  Trailing space " &&
	          s.games[2].game_path == "/games/PS5, extra/PPSA00003" &&
	          s.games[2].configuration == expected,
	      "special game 3: control characters and spaces in the name");
	expected.host_input_mapping = {" leading space", "trailing space ", "@", "@@double"};
	expected.user_name          = " Kyty ";
	Check(s.games[3].name == "ゲーム・テスト Ünïcødé" &&
	          s.games[3].game_path == "/games/ゲーム/Ünïcødé/PPSA00002" &&
	          s.games[3].configuration == expected,
	      "special game 4: mapping entries are loaded as stored, the user name untrimmed");
	Check(kyty::core::Validate(s.games[3].configuration)->field == "host_input_mapping",
	      "special game 4's mapping fails Validate() only when saved from an editor");
	Check(s.unknown.Find("ConfigurationEditDialog/geometry") != nullptr && s.geometry.has_value(),
	      "special geometries");
}

void TestEmptyLists() {
	const auto  report = LoadFixture("qt6-empty-lists.ini");
	const auto& s      = report.settings;
	Check(s.game_folders.empty() && s.addons_dir.empty(), "empty-lists launcher");
	Check(s.global == GameConfiguration {}, "empty-lists global configuration");
	Check(s.games.size() == 1 && s.games[0].name.empty() &&
	          s.games[0].game_path == "/home/user/Games/PS5/PPSA04321" &&
	          s.games[0].configuration == GameConfiguration {},
	      "empty-lists game with an empty name and mapping");
	Check(s.theme == std::optional<std::string>("0") && !s.geometry, "empty-lists main dialog");
}

void TestLegacyGameDir() {
	const auto  report = LoadFixture("qt6-legacy-game-dir.ini");
	const auto& s      = report.settings;
	Check(s.game_folders == std::vector<std::string> {"/home/user/Games/PS5, legacy"},
	      "legacy game_dir becomes the first Game_Folder");
	Check(s.addons_dir.empty() && !s.grid_view && s.grid_icon_width == 224,
	      "a legacy store has no addons or grid keys: defaults, not reported");
	Check(s.games.size() == 1 && s.games[0].game_path == "PPSA01234" &&
	          s.games[0].basedir == "/home/user/Games/PS5, legacy/PPSA01234" &&
	          s.games[0].name == "Astro Bot",
	      "legacy relative game_path");
	const auto game = Entry("/home/user/Games/PS5, legacy/PPSA01234", "PPSA01234");
	Check(kyty::core::FindGameSettings(s, game) == &s.games[0],
	      "the relative entry matches legacy_relative_path");
	const auto saved = IniFile::Parse(kyty::core::SerializeSettings(s));
	Check(saved.Find("Launcher/game_dir") == nullptr &&
	          saved.GetStringList("Launcher/game_dirs") == s.game_folders,
	      "a save writes game_dirs and drops game_dir");
}

const std::vector<std::string> kFixtures = {"qt6-empty-lists.ini",       "qt6-game-entries.ini",
                                            "qt6-global-all-fields.ini", "qt6-legacy-game-dir.ini",
                                            "qt6-portable.ini",          "qt6-special-values.ini",
                                            "qt6-user-store.ini"};

// A re-save keeps every key of the fixture with its value, except the Qt migrations, and adds only
// the new keys; loading the re-saved file gives the same settings.
void TestResaveKeepsKeys() {
	for (const auto& name: kFixtures) {
		const std::string bytes    = ReadBytes(FixturePath(name));
		const auto        original = IniFile::Parse(bytes);
		const auto        report   = kyty::core::ParseSettings(bytes);
		const std::string resaved  = kyty::core::SerializeSettings(report.settings);
		const auto        file     = IniFile::Parse(resaved);
		const bool        legacy   = original.Find("Launcher/game_dir") != nullptr;
		for (const auto& [key, value]: original.Entries()) {
			if (key == "Launcher/game_dir") {
				Check(file.Find(key) == nullptr, name + ": game_dir is dropped");
				continue;
			}
			const IniValue* saved = file.Find(key);
			Check(saved != nullptr, name + ": the re-save keeps " + key);
			if (key.starts_with("GameConfigurations/") && key.ends_with("/custom_settings")) {
				Check(file.GetString(key) == std::optional<std::string>("true"),
				      name + ": " + key + " is written as true");
			} else {
				Check(*saved == value, name + ": the re-save keeps the value of " + key);
			}
		}
		for (const auto& [key, value]: file.Entries()) {
			if (original.Find(key) != nullptr) {
				continue;
			}
			const bool window = key.starts_with("LauncherWindow/");
			const bool mapping_custom =
			    key.starts_with("GameConfigurations/") && key.ends_with("/input_mapping_custom");
			const bool migrated = legacy && key.starts_with("Launcher/");
			Check(window || mapping_custom || migrated,
			      name + ": the re-save adds only new keys, " + "not " + key);
		}
		const auto again = kyty::core::ParseSettings(resaved);
		CheckDefaulted(again, {}, name + " re-saved");
		Check(again.settings == report.settings,
		      name + ": loading the re-save gives equal settings");
		Check(kyty::core::SerializeSettings(again.settings) == resaved,
		      name + ": a second re-save gives the same bytes");
	}

	// The portable store re-saves byte for byte, plus the new [LauncherWindow] group.
	const std::string portable = ReadBytes(FixturePath("qt6-portable.ini"));
	std::string       expected = portable;
	const auto        at       = expected.find("[MainDialog]");
	Check(at != std::string::npos, "portable has [MainDialog]");
	expected.insert(at, "[LauncherWindow]\nheight=720\nmaximized=false\nwidth=1100\n\n");
	Check(kyty::core::SerializeSettings(kyty::core::ParseSettings(portable).settings) == expected,
	      "portable re-save bytes");
}

void TestUnknownKeysKept() {
	auto report = LoadEdited("qt6-user-store.ini", [](IniFile& file) {
		file.SetString("top_level", "1");
		file.SetString("Extra/foo", "bar");
		file.SetStringList("Extra/list", {"a", "b"});
		file.Set("Extra/blob", IniValue::FromByteArray(std::string("\0\x01\xff", 3)));
		file.SetString("Launcher/future_key", "x");
		file.SetString("GlobalConfiguration/red_zone_protection_enabled", "true");
		file.SetString("GlobalConfiguration/input_mapping_custom", "true");
		file.SetString("GameConfigurations/1/red_zone_protection_enabled", "true");
		file.SetString("GameConfigurations/1/future/nested", "y");
		file.SetString("GameConfigurations/foo", "bar");
		file.SetString("GameConfigurations/2/name", "beyond size");
	});
	CheckDefaulted(report, {}, "unknown keys");
	const auto& s = report.settings;
	for (const char* key: {"top_level", "Extra/foo", "Extra/list", "Extra/blob",
	                       "Launcher/future_key", "GlobalConfiguration/red_zone_protection_enabled",
	                       "GlobalConfiguration/input_mapping_custom", "GameConfigurations/foo"}) {
		Check(s.unknown.Find(key) != nullptr, std::string("unknown key ") + key);
	}
	Check(!s.global.input_mapping_custom,
	      "the Global_Configuration does not read input_mapping_custom");
	Check(s.unknown.Find("GameConfigurations/2/name") == nullptr,
	      "an element above size is dropped, as Qt drops it");
	Check(s.games.size() == 1 && s.games[0].unknown.size() == 2 &&
	          s.games[0].unknown.at("red_zone_protection_enabled") ==
	              IniValue::FromString("true") &&
	          s.games[0].unknown.at("future/nested") == IniValue::FromString("y"),
	      "element unknown keys");

	const auto saved = IniFile::Parse(kyty::core::SerializeSettings(s));
	for (const auto& [key, value]: s.unknown.Entries()) {
		Check(saved.Find(key) != nullptr && *saved.Find(key) == value, "re-save keeps " + key);
	}
	Check(saved.GetString("GameConfigurations/1/red_zone_protection_enabled") ==
	              std::optional<std::string>("true") &&
	          saved.GetString("GameConfigurations/1/future/nested") ==
	              std::optional<std::string>("y"),
	      "re-save keeps the element unknown keys");

	// The element's unknown keys move with it.
	auto moved = s;
	moved.games.insert(moved.games.begin(), GameSettingsEntry {"First", "/a", "/a", {}, {}});
	const auto saved_moved = IniFile::Parse(kyty::core::SerializeSettings(moved));
	Check(saved_moved.GetString("GameConfigurations/2/red_zone_protection_enabled") ==
	              std::optional<std::string>("true") &&
	          saved_moved.Find("GameConfigurations/1/red_zone_protection_enabled") == nullptr &&
	          saved_moved.ArraySize("GameConfigurations") == 2,
	      "element unknown keys follow their element");

	// Schema keys in LauncherSettings::unknown never override the model.
	auto injected = s;
	injected.unknown.SetString("Launcher/grid_icon_width", "999");
	injected.unknown.SetString("Launcher/game_dir", "/old");
	injected.unknown.SetString("GameConfigurations/7/name", "stale");
	injected.unknown.SetString("GameConfigurations/size", "9");
	const auto saved_injected = IniFile::Parse(kyty::core::SerializeSettings(injected));
	Check(saved_injected.GetString("Launcher/grid_icon_width") ==
	              std::optional<std::string>("256") &&
	          saved_injected.Find("Launcher/game_dir") == nullptr &&
	          saved_injected.Find("GameConfigurations/7/name") == nullptr &&
	          saved_injected.ArraySize("GameConfigurations") == 1,
	      "schema keys come from the model only");
}

// ---- Defaults and reports (Requirement 9.6)

void TestInvalidValuesDefaulted() {
	struct Case {
		std::string                                  what;
		std::function<void(IniFile&)>                edit;
		std::vector<std::string>                     defaulted;
		std::function<bool(const LauncherSettings&)> holds;
	};
	const std::vector<Case> cases = {
	    {"missing user_id",
	     [](IniFile& f) { f.Remove("GlobalConfiguration/user_id"); },
	     {"GlobalConfiguration/user_id"},
	     [](const LauncherSettings& s) { return s.global.user_id == 1000; }},
	    {"user_id 254",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/user_id", "254"); },
	     {"GlobalConfiguration/user_id"},
	     [](const LauncherSettings& s) { return s.global.user_id == 1000; }},
	    {"user_id 0",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/user_id", "0"); },
	     {},
	     [](const LauncherSettings& s) { return s.global.user_id == 0; }},
	    {"user_id beyond int32",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/user_id", "4294967296"); },
	     {"GlobalConfiguration/user_id"},
	     [](const LauncherSettings& s) { return s.global.user_id == 1000; }},
	    {"vblank text",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/vblank_frequency", "fast"); },
	     {"GlobalConfiguration/vblank_frequency"},
	     [](const LauncherSettings& s) { return s.global.vblank_frequency == 60; }},
	    {"vblank 361",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/vblank_frequency", "361"); },
	     {"GlobalConfiguration/vblank_frequency"},
	     [](const LauncherSettings& s) { return s.global.vblank_frequency == 60; }},
	    {"trophy 4",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/trophy_duration_seconds", "4"); },
	     {"GlobalConfiguration/trophy_duration_seconds"},
	     [](const LauncherSettings& s) { return s.global.trophy_duration_seconds == 6; }},
	    {"empty user name",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/user_name", " "); },
	     {"GlobalConfiguration/user_name"},
	     [](const LauncherSettings& s) { return s.global.user_name == "Kyty"; }},
	    {"user name as a list",
	     [](IniFile& f) { f.SetStringList("GlobalConfiguration/user_name", {"a", "b"}); },
	     {"GlobalConfiguration/user_name"},
	     [](const LauncherSettings& s) { return s.global.user_name == "Kyty"; }},
	    {"empty shader log folder while logging to a file",
	     [](IniFile& f) {
		     f.SetString("GlobalConfiguration/shader_log_direction", "File");
		     f.SetString("GlobalConfiguration/shader_log_folder", "");
	     },
	     {"GlobalConfiguration/shader_log_folder"},
	     [](const LauncherSettings& s) {
		     return s.global.shader_log_direction == LogDirection::File &&
		            s.global.shader_log_folder == "_Shaders";
	     }},
	    {"empty shader log folder while silent",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/shader_log_folder", ""); },
	     {},
	     [](const LauncherSettings& s) { return s.global.shader_log_folder.empty(); }},
	    {"empty dump folder while dumping",
	     [](IniFile& f) {
		     f.SetString("GlobalConfiguration/command_buffer_dump_enabled", "true");
		     f.SetString("GlobalConfiguration/command_buffer_dump_folder", "");
	     },
	     {"GlobalConfiguration/command_buffer_dump_folder"},
	     [](const LauncherSettings& s) {
		     return s.global.command_buffer_dump_folder == "_Buffers";
	     }},
	    {"empty printf file while printing to a file",
	     [](IniFile& f) {
		     f.SetString("GlobalConfiguration/printf_direction", "File");
		     f.SetString("GlobalConfiguration/printf_output_file", "");
	     },
	     {"GlobalConfiguration/printf_output_file"},
	     [](const LauncherSettings& s) { return s.global.printf_output_file == "_kyty.txt"; }},
	    {"gpu_index -2",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/gpu_index", "-2"); },
	     {"GlobalConfiguration/gpu_index"},
	     [](const LauncherSettings& s) { return s.global.gpu_index == -1; }},
	    {"console_language 30",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/console_language", "30"); },
	     {"GlobalConfiguration/console_language"},
	     [](const LauncherSettings& s) { return s.global.console_language == 1; }},
	    {"absolute elf",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/elf", "/bin/sh"); },
	     {"GlobalConfiguration/elf"},
	     [](const LauncherSettings& s) { return s.global.elf == "eboot.bin"; }},
	    {"legacy resolution",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/screen_resolution", "1920x1080"); },
	     {},
	     [](const LauncherSettings& s) {
		     return s.global.screen_resolution == ScreenResolution::R1920X1080;
	     }},
	    {"resolution index",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/screen_resolution", "3"); },
	     {},
	     [](const LauncherSettings& s) {
		     return s.global.screen_resolution == ScreenResolution::R3840X2160;
	     }},
	    {"unknown present mode",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/present_mode", "Vsync"); },
	     {"GlobalConfiguration/present_mode"},
	     [](const LauncherSettings& s) { return s.global.present_mode == PresentMode::Mailbox; }},
	    {"bool text",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/hdr_enabled", "yes"); },
	     {"GlobalConfiguration/hdr_enabled"},
	     [](const LauncherSettings& s) { return !s.global.hdr_enabled; }},
	    {"opaque mapping",
	     [](IniFile& f) {
		     f.Set("GlobalConfiguration/host_input_mapping", IniValue::FromByteArray("x"));
	     },
	     {"GlobalConfiguration/host_input_mapping"},
	     [](const LauncherSettings& s) { return s.global.host_input_mapping.empty(); }},
	    {"mapping entries without '=' load as stored",
	     [](IniFile& f) { f.SetStringList("GlobalConfiguration/host_input_mapping", {"a", "b"}); },
	     {},
	     [](const LauncherSettings& s) {
		     return s.global.host_input_mapping == std::vector<std::string> {"a", "b"};
	     }},
	    {"controller clamped",
	     [](IniFile& f) {
		     f.SetString("GlobalConfiguration/controller_speaker_volume", "150");
		     f.SetString("GlobalConfiguration/controller_vibration_intensity", "-5");
		     f.SetString("GlobalConfiguration/controller_keyboard_player", "9223372036854775807");
	     },
	     {},
	     [](const LauncherSettings& s) {
		     return s.controller.speaker_volume == 100 && s.controller.vibration_intensity == 0 &&
		            s.controller.keyboard_player == 4;
	     }},
	    {"controller unparsable",
	     [](IniFile& f) {
		     f.SetString("GlobalConfiguration/controller_speaker_volume", "loud");
		     f.SetString("GlobalConfiguration/controller_keyboard_player", "");
		     f.SetString("GlobalConfiguration/controller_vibration_intensity",
		                 "99999999999999999999");
	     },
	     {"GlobalConfiguration/controller_speaker_volume",
	      "GlobalConfiguration/controller_keyboard_player",
	      "GlobalConfiguration/controller_vibration_intensity"},
	     [](const LauncherSettings& s) { return s.controller == ControllerSettings {}; }},
	    {"controller color",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/controller_color", "red"); },
	     {"GlobalConfiguration/controller_color"},
	     [](const LauncherSettings& s) { return s.controller.color.empty(); }},
	    {"upper-case controller color",
	     [](IniFile& f) { f.SetString("GlobalConfiguration/controller_color", "#ABCDEF"); },
	     {},
	     [](const LauncherSettings& s) { return s.controller.color == "#ABCDEF"; }},
	    {"grid width 128",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "128"); },
	     {},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 128; }},
	    {"grid width 320",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "320"); },
	     {},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 320; }},
	    {"grid width 144",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "144"); },
	     {},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 144; }},
	    {"grid width 112",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "112"); },
	     {"Launcher/grid_icon_width"},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 224; }},
	    {"grid width 336",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "336"); },
	     {"Launcher/grid_icon_width"},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 224; }},
	    {"grid width 130",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "130"); },
	     {"Launcher/grid_icon_width"},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 224; }},
	    {"grid width text",
	     [](IniFile& f) { f.SetString("Launcher/grid_icon_width", "big"); },
	     {"Launcher/grid_icon_width"},
	     [](const LauncherSettings& s) { return s.grid_icon_width == 224; }},
	    {"grid view text",
	     [](IniFile& f) { f.SetString("Launcher/grid_view", "maybe"); },
	     {"Launcher/grid_view"},
	     [](const LauncherSettings& s) { return !s.grid_view; }},
	    {"missing game_dirs",
	     [](IniFile& f) { f.Remove("Launcher/game_dirs"); },
	     {"Launcher/game_dirs"},
	     [](const LauncherSettings& s) { return s.game_folders.empty(); }},
	    {"opaque game_dirs",
	     [](IniFile& f) { f.Set("Launcher/game_dirs", IniValue::FromByteArray("x")); },
	     {"Launcher/game_dirs"},
	     [](const LauncherSettings& s) { return s.game_folders.empty(); }},
	    {"empty game_dirs with game_dir",
	     [](IniFile& f) {
		     f.SetStringList("Launcher/game_dirs", {});
		     f.SetString("Launcher/game_dir", "/legacy");
	     },
	     {},
	     [](const LauncherSettings& s) {
		     return s.game_folders == std::vector<std::string> {"/legacy"};
	     }},
	    {"game_dirs win over game_dir",
	     [](IniFile& f) { f.SetString("Launcher/game_dir", "/legacy"); },
	     {},
	     [](const LauncherSettings& s) {
		     return s.game_folders == std::vector<std::string> {"/home/user/Games/PS5"};
	     }},
	    {"missing addons_dir",
	     [](IniFile& f) { f.Remove("Launcher/addons_dir"); },
	     {"Launcher/addons_dir"},
	     [](const LauncherSettings& s) { return s.addons_dir.empty(); }},
	    {"missing update check",
	     [](IniFile& f) { f.Remove("MainDialog/check_updates_on_startup"); },
	     {"MainDialog/check_updates_on_startup"},
	     [](const LauncherSettings& s) { return s.check_updates_on_startup; }},
	    {"missing theme is kept absent",
	     [](IniFile& f) { f.Remove("MainDialog/theme"); },
	     {},
	     [](const LauncherSettings& s) { return !s.theme.has_value(); }},
	    {"window values",
	     [](IniFile& f) {
		     f.SetString("LauncherWindow/width", "800");
		     f.SetString("LauncherWindow/height", "600");
		     f.SetString("LauncherWindow/maximized", "true");
	     },
	     {},
	     [](const LauncherSettings& s) {
		     return s.window == kyty::core::LauncherWindowState {800, 600, true};
	     }},
	    {"invalid window values",
	     [](IniFile& f) {
		     f.SetString("LauncherWindow/width", "0");
		     f.SetString("LauncherWindow/height", "tall");
		     f.SetString("LauncherWindow/maximized", "maybe");
	     },
	     {"LauncherWindow/width", "LauncherWindow/height", "LauncherWindow/maximized"},
	     [](const LauncherSettings& s) { return s.window == kyty::core::LauncherWindowState {}; }},
	};
	for (const auto& c: cases) {
		const auto report = LoadEdited("qt6-portable.ini", c.edit);
		CheckDefaulted(report, c.defaulted, c.what);
		Check(c.holds(report.settings), c.what + ": values");
	}

	// A legacy store reports its optional Launcher keys only when they are present and invalid.
	const auto legacy = LoadEdited("qt6-legacy-game-dir.ini", [](IniFile& f) {
		f.SetString("Launcher/grid_icon_width", "100");
	});
	CheckDefaulted(legacy, {"Launcher/grid_icon_width"}, "invalid width in a legacy store");
	const auto none = LoadEdited("qt6-portable.ini", [](IniFile& f) {
		f.Remove("Launcher/game_dirs");
		f.Remove("Launcher/addons_dir");
	});
	CheckDefaulted(none, {"Launcher/game_dirs", "Launcher/addons_dir"},
	               "without game_dir the store is not a legacy store");
}

void TestGameElements() {
	const std::string fixture = "qt6-user-store.ini";

	auto r = LoadEdited(fixture, [](IniFile& f) {
		f.SetString("GameConfigurations/1/input_mapping_custom", "true");
	});
	CheckDefaulted(r, {}, "input_mapping_custom true");
	Check(r.settings.games[0].configuration.input_mapping_custom, "input_mapping_custom read");

	r = LoadEdited(fixture, [](IniFile& f) {
		f.SetString("GameConfigurations/1/input_mapping_custom", "maybe");
	});
	CheckDefaulted(r, {"GameConfigurations/1/input_mapping_custom"},
	               "invalid input_mapping_custom");

	r = LoadEdited(fixture, [](IniFile& f) {
		f.Remove("GameConfigurations/1/vblank_frequency");
		f.SetString("GameConfigurations/1/custom_settings", "maybe");
		f.SetStringList("GameConfigurations/1/name", {"a", "b"});
	});
	CheckDefaulted(r,
	               {"GameConfigurations/1/vblank_frequency", "GameConfigurations/1/custom_settings",
	                "GameConfigurations/1/name"},
	               "element keys");
	Check(r.settings.games.size() == 1 &&
	          r.settings.games[0].configuration.vblank_frequency == 60 &&
	          r.settings.games[0].name.empty(),
	      "element defaults; the element is still custom");

	r = LoadEdited(fixture, [](IniFile& f) { f.Remove("GameConfigurations/1/custom_settings"); });
	CheckDefaulted(r, {"GameConfigurations/1/custom_settings"}, "missing custom_settings");
	Check(r.settings.games.size() == 1, "an element without custom_settings is still custom");

	for (const bool remove: {true, false}) {
		r = LoadEdited(fixture, [remove](IniFile& f) {
			if (remove) {
				f.Remove("GameConfigurations/1/game_path");
			} else {
				f.SetString("GameConfigurations/1/game_path", "");
			}
		});
		CheckDefaulted(r, {"GameConfigurations/1/game_path"}, "element without a game_path");
		Check(r.settings.games.empty() &&
		          r.settings.unknown.Find("GameConfigurations/1/name") == nullptr,
		      "an element without a game_path is dropped");
	}

	r = LoadEdited(fixture, [](IniFile& f) { f.Remove("GameConfigurations/size"); });
	CheckDefaulted(r, {"GameConfigurations/size"}, "missing size");
	Check(r.settings.games.empty(), "without size there is no element");
	for (const char* size: {"x", "-1", "1.0"}) {
		r = LoadEdited(fixture,
		               [size](IniFile& f) { f.SetString("GameConfigurations/size", size); });
		CheckDefaulted(r, {"GameConfigurations/size"}, std::string("size ") + size);
		Check(r.settings.games.empty(), std::string("size ") + size + " gives no element");
	}

	// Gaps and duplicates.
	r = LoadEdited(fixture, [](IniFile& f) {
		f.SetString("GameConfigurations/size", "3");
		const auto copy = f.Entries();
		for (const auto& [key, value]: copy) {
			if (key.starts_with("GameConfigurations/1/")) {
				f.Set("GameConfigurations/3/" + key.substr(21), value);
			}
		}
		f.SetString("GameConfigurations/3/name", "Later");
		f.SetString("GameConfigurations/3/vblank_frequency", "30");
	});
	CheckDefaulted(r, {}, "gap and duplicate");
	Check(r.settings.games.size() == 1 && r.settings.games[0].name == "Later" &&
	          r.settings.games[0].configuration.vblank_frequency == 30,
	      "of two elements with the same game_path the later one is kept; element 2 is a gap");

	r = LoadEdited(fixture, [](IniFile& f) {
		f.SetString("GameConfigurations/size", "2");
		f.SetString("GameConfigurations/2/game_path", "/other");
	});
	Check(r.settings.games.size() == 2 && r.settings.games[1].game_path == "/other",
	      "a second element with only game_path");
	// name, basedir, custom_settings, and 34 fields (input_mapping_custom is optional).
	Check(r.defaulted_keys.size() == 37 &&
	          std::all_of(
	              r.defaulted_keys.begin(), r.defaulted_keys.end(),
	              [](const std::string& key) { return key.starts_with("GameConfigurations/2/"); }),
	      "its other 37 keys are reported: " + Join(r.defaulted_keys));
}

// ---- Files and injected faults (Requirement 12.8)

const char* StepName(AtomicFileStep step) {
	switch (step) {
		case AtomicFileStep::CreateTemp: return "CreateTemp";
		case AtomicFileStep::Write: return "Write";
		case AtomicFileStep::Sync: return "Sync";
		case AtomicFileStep::Close: return "Close";
		case AtomicFileStep::Rename: return "Rename";
	}
	return "?";
}

std::set<std::string> Names(const fs::path& directory) {
	std::set<std::string> names;
	std::error_code       ec;
	for (const auto& entry: fs::directory_iterator(directory, ec)) {
		names.insert(entry.path().filename().string());
	}
	Check(!ec, "list " + directory.string());
	return names;
}

void TestSaveAndFaults() {
	const TempDir     dir("kyty_settings_store");
	const std::string bytes = ReadBytes(FixturePath("qt6-game-entries.ini"));
	const auto        ini   = dir.WriteFile("Kyty.ini", bytes);

	auto loaded = kyty::core::LoadSettings(ini);
	Check(loaded.has_value() && loaded->defaulted_keys.empty(), "load the copied fixture");
	auto settings             = loaded->settings;
	settings.grid_icon_width  = 160;
	settings.window.maximized = true;

	for (const auto step: {AtomicFileStep::CreateTemp, AtomicFileStep::Write, AtomicFileStep::Sync,
	                       AtomicFileStep::Close, AtomicFileStep::Rename}) {
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](AtomicFileStep current) { return current == step ? EIO : 0; });
		const auto result = kyty::core::SaveSettings(ini, settings);
		kyty::core::SetAtomicFileFaultForTesting({});
		Check(!result.has_value(), std::string("a fault at ") + StepName(step) + " fails the save");
		// The launcher shows IoError::path (the Settings_Store) and the message (the reason).
		Check(result.error().error_number == EIO && result.error().path == ini &&
		          result.error().message.find(std::generic_category().message(EIO)) !=
		              std::string::npos,
		      std::string("the error names the file and the reason at ") + StepName(step));
		Check(ReadBytes(ini) == bytes,
		      std::string("the file is unchanged after a fault at ") + StepName(step));
		Check(Names(dir.Path()) == std::set<std::string> {"Kyty.ini"},
		      std::string("no temporary file is left after ") + StepName(step));
	}

	Check(kyty::core::SaveSettings(ini, settings).has_value(), "the save succeeds without a fault");
	const auto reloaded = kyty::core::LoadSettings(ini);
	Check(reloaded.has_value() && reloaded->defaulted_keys.empty() &&
	          reloaded->settings == settings,
	      "the saved settings load back equal");

	const auto missing = kyty::core::LoadSettings(dir.Path() / "missing.ini");
	Check(!missing.has_value() && missing.error().error_number == ENOENT, "a missing file");
	const auto directory = kyty::core::LoadSettings(dir.Path());
	Check(!directory.has_value() && directory.error().error_number == EISDIR, "a directory");
	const auto no_dir = kyty::core::SaveSettings(dir.Path() / "none" / "Kyty.ini", settings);
	Check(!no_dir.has_value() && no_dir.error().error_number == ENOENT,
	      "a save into a missing directory fails");

	// Default settings round-trip as well.
	const LauncherSettings defaults;
	const auto report = kyty::core::ParseSettings(kyty::core::SerializeSettings(defaults));
	CheckDefaulted(report, {}, "default settings");
	Check(report.settings == defaults, "default settings round-trip");
}

// ---- Custom settings (Requirements 9.2, 9.3, 9.13)

void TestCustomSettingsLifecycle() {
	auto settings             = LoadFixture("qt6-game-entries.ini").settings;
	settings.global.user_name = "Global";
	const auto before         = settings;
	const auto fresh          = Entry("/home/user/Games/PS5/PPSA07777", "PPSA07777", "Fresh");

	Check(!kyty::core::HasCustomSettings(settings, fresh), "a new game has no custom settings");
	auto seeded = kyty::core::EnableCustomSettings(settings, fresh);
	Check(seeded == settings.global, "enabling seeds from the Global_Configuration");
	Check(settings == before, "enabling does not change the store");

	auto invalid      = seeded;
	invalid.user_id   = 255;
	const auto failed = kyty::core::SaveCustomSettings(settings, fresh, invalid);
	Check(!failed.has_value() && failed.error().field == "user_id",
	      "an invalid save names the field");
	Check(settings == before, "an invalid save changes nothing");

	seeded.vblank_frequency = 90;
	Check(kyty::core::SaveCustomSettings(settings, fresh, seeded).has_value(), "a valid save");
	const auto* stored = kyty::core::FindGameSettings(settings, fresh);
	Check(stored != nullptr && stored->configuration == seeded && stored->name == "Fresh" &&
	          stored->game_path == fresh.path.string() && stored->basedir == fresh.path.string(),
	      "the save stores exactly those values and marks the game custom");
	Check(settings.games.size() == before.games.size() + 1, "the new element is appended");
	Check(kyty::core::EnableCustomSettings(settings, fresh) == seeded,
	      "enabling again opens the custom configuration");

	// Overwriting keeps the position and the element's unknown keys.
	const auto astro = Entry("/home/user/Games/PS5/PPSA01234", "PPSA01234", "Astro Bot 2");
	settings.games[1].unknown.emplace("extra", IniValue::FromString("1"));
	auto astro_config        = settings.games[1].configuration;
	astro_config.hdr_enabled = false;
	Check(kyty::core::SaveCustomSettings(settings, astro, astro_config).has_value(), "overwrite");
	Check(settings.games[1].configuration == astro_config &&
	          settings.games[1].name == "Astro Bot 2" && settings.games[1].unknown.size() == 1,
	      "an overwrite keeps the position and the unknown keys");

	// A legacy relative element is found through legacy_relative_path and saved as absolute.
	const auto legacy = Entry("/home/user/Games/PS5/Legacy/PPSA05678", "Legacy/PPSA05678");
	Check(kyty::core::FindGameSettings(settings, legacy) == &settings.games[3],
	      "the legacy element is found");
	auto legacy_config = kyty::core::EnableCustomSettings(settings, legacy);
	Check(legacy_config.elf == "bin/eboot_alt.bin", "the legacy configuration opens");
	Check(kyty::core::SaveCustomSettings(settings, legacy, legacy_config).has_value(),
	      "save the legacy game");
	Check(settings.games[3].game_path == legacy.path.string(), "the save rewrites game_path");

	// Clearing removes the element.
	const auto count = settings.games.size();
	Check(kyty::core::ClearCustomSettings(settings, fresh), "clear");
	Check(!kyty::core::HasCustomSettings(settings, fresh) && settings.games.size() == count - 1,
	      "clearing removes the element");
	Check(!kyty::core::ClearCustomSettings(settings, fresh), "clearing twice finds nothing");
	const auto effective = kyty::core::ResolveEffectiveConfiguration(settings, fresh, std::nullopt);
	Check(effective.configuration == settings.global,
	      "a cleared game runs with the global settings");
	const auto saved = IniFile::Parse(kyty::core::SerializeSettings(settings));
	for (const auto& [key, value]: saved.Entries()) {
		Check(!(key.ends_with("/game_path") && value == IniValue::FromString(fresh.path.string())),
		      "no element is written for the cleared game");
	}

	// Clearing finds legacy elements too.
	auto       qt        = LoadFixture("qt6-game-entries.ini").settings;
	const auto legacy_qt = Entry("/elsewhere/PPSA05678", "Legacy/PPSA05678");
	Check(kyty::core::ClearCustomSettings(qt, legacy_qt) && qt.games.size() == 3,
	      "clearing a legacy element");

	// Two elements for one game (an absolute and a legacy one): saving keeps one.
	auto duplicate = LoadFixture("qt6-game-entries.ini").settings;
	duplicate.games.push_back(duplicate.games[3]);
	duplicate.games.back().game_path = legacy.path.string();
	Check(kyty::core::FindGameSettings(duplicate, legacy) == &duplicate.games[4],
	      "the absolute element wins over the legacy one");
	Check(kyty::core::SaveCustomSettings(duplicate, legacy, legacy_config).has_value() &&
	          duplicate.games.size() == 4 && duplicate.games[3].game_path == legacy.path.string(),
	      "saving removes the other element for the game");
}

void TestMigrateLegacyGamePaths() {
	auto                         settings = LoadFixture("qt6-game-entries.ini").settings;
	const std::vector<GameEntry> entries  = {
	    Entry("/home/user/Games/PS5/PPSA01234", "PPSA01234"),
	    Entry("/home/user/Games/PS5/Legacy/PPSA05678", "Legacy/PPSA05678"),
	    Entry("/other/Legacy/PPSA05678", "Legacy/PPSA05678")};
	Check(kyty::core::MigrateLegacyGamePaths(settings, entries), "a legacy path is migrated");
	Check(settings.games[3].game_path == "/home/user/Games/PS5/Legacy/PPSA05678",
	      "the first matching entry claims it");
	Check(!kyty::core::MigrateLegacyGamePaths(settings, entries), "nothing left to migrate");
	const auto reloaded = kyty::core::ParseSettings(kyty::core::SerializeSettings(settings));
	Check(reloaded.settings.games[3].game_path == "/home/user/Games/PS5/Legacy/PPSA05678",
	      "the absolute path is saved");

	// Not when another element already has the absolute path.
	auto taken               = LoadFixture("qt6-game-entries.ini").settings;
	taken.games[0].game_path = "/home/user/Games/PS5/Legacy/PPSA05678";
	Check(!kyty::core::MigrateLegacyGamePaths(taken, entries) &&
	          taken.games[3].game_path == "Legacy/PPSA05678",
	      "a taken path is not migrated");
}

// ---- EffectiveConfiguration

void TestResolveEffectiveConfiguration() {
	auto settings                      = LoadFixture("qt6-global-all-fields.ini").settings;
	settings.global.host_input_mapping = {"Cross=K"};
	const auto game                    = Entry("/games/PPSA01234", "PPSA01234");

	auto global = kyty::core::ResolveEffectiveConfiguration(settings, game, std::nullopt);
	Check(global.configuration == settings.global && global.controller == settings.controller &&
	          global.addons_dir == "/home/user/Games/PS5-Addons" && !global.cheat_plan,
	      "a game without custom settings uses the Global_Configuration");

	GameConfiguration custom;
	custom.vblank_frequency   = 30;
	custom.host_input_mapping = {"Cross=L"};
	Check(kyty::core::SaveCustomSettings(settings, game, custom).has_value(), "save custom");
	const fs::path plan         = "/data/_Patches/PPSA01234.json";
	auto           effective    = kyty::core::ResolveEffectiveConfiguration(settings, game, plan);
	auto           expected     = custom;
	expected.host_input_mapping = {"Cross=K"};
	Check(effective.configuration == expected,
	      "without input_mapping_custom the global mapping is used");
	Check(effective.controller == settings.controller && effective.cheat_plan == plan &&
	          effective.addons_dir == settings.addons_dir,
	      "global controller fields, add-ons folder, and the cheat plan");

	custom.input_mapping_custom = true;
	Check(kyty::core::SaveCustomSettings(settings, game, custom).has_value(), "save mapping");
	effective = kyty::core::ResolveEffectiveConfiguration(settings, game, std::nullopt);
	Check(effective.configuration == custom &&
	          effective.configuration.host_input_mapping == std::vector<std::string> {"Cross=L"},
	      "with input_mapping_custom the per-game mapping is used");

	settings.addons_dir = "";
	Check(
	    kyty::core::ResolveEffectiveConfiguration(settings, game, std::nullopt).addons_dir.empty(),
	    "an empty add-ons folder");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("SettingsStoreTests");
	TestPortable();
	TestUserStore();
	TestGlobalAllFields();
	TestGameEntries();
	TestSpecialValues();
	TestEmptyLists();
	TestLegacyGameDir();
	TestResaveKeepsKeys();
	TestUnknownKeysKept();
	TestInvalidValuesDefaulted();
	TestGameElements();
	TestSaveAndFaults();
	TestCustomSettingsLifecycle();
	TestMigrateLegacyGamePaths();
	TestResolveEffectiveConfiguration();
	std::puts("SettingsStoreTests: all tests passed");
	return 0;
}
