// Feature: linux-gtk-modernization, Property 15: Settings_Store round trip
//
// Validates Requirements 7.17 and 9.7. Valid LauncherSettings from configurationGenerators.h
// (Game_Folders, add-ons folder, view, grid icon width, update check, raw theme and geometry or
// none, main and settings window sizes, a valid Global_Configuration and controller, 0-4 games
// with custom configurations and input-mapping overrides, unknown keys at the top and in the
// elements; text from a Unicode alphabet with the characters Kyty.ini quotes and escapes).
// SaveSettings followed by LoadSettings (through a file, and through SerializeSettings and
// ParseSettings) gives settings equal field by field with no defaulted key, and a second save
// writes the same bytes.
//
// Left out of the generator, as in Property 14, because QSettings cannot round-trip it either:
// U+FEFF (Qt's decoder drops it as the first character of a value).

#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <rapidcheck.h>
#include <string>

namespace {

using namespace KytyTests::ConfigGen;
using kyty::core::LauncherSettings;

void AssertEqual(const LauncherSettings& loaded, const LauncherSettings& stored) {
	RC_ASSERT(loaded.game_folders == stored.game_folders);
	RC_ASSERT(loaded.addons_dir == stored.addons_dir);
	RC_ASSERT(loaded.grid_view == stored.grid_view);
	RC_ASSERT(loaded.grid_icon_width == stored.grid_icon_width);
	RC_ASSERT(loaded.check_updates_on_startup == stored.check_updates_on_startup);
	RC_ASSERT(loaded.theme == stored.theme);
	RC_ASSERT(loaded.geometry == stored.geometry);
	RC_ASSERT(loaded.window == stored.window);
	RC_ASSERT(loaded.settings_window == stored.settings_window);
	RC_ASSERT(loaded.global == stored.global);
	RC_ASSERT(loaded.controller == stored.controller);
	RC_ASSERT(loaded.games.size() == stored.games.size());
	for (size_t i = 0; i < stored.games.size(); i++) {
		RC_LOG() << "game " << i << "\n";
		RC_ASSERT(loaded.games[i].name == stored.games[i].name);
		RC_ASSERT(loaded.games[i].basedir == stored.games[i].basedir);
		RC_ASSERT(loaded.games[i].game_path == stored.games[i].game_path);
		RC_ASSERT(loaded.games[i].configuration == stored.games[i].configuration);
		RC_ASSERT(loaded.games[i].unknown == stored.games[i].unknown);
	}
	RC_ASSERT(loaded.unknown == stored.unknown);
	RC_ASSERT(loaded == stored);
}

} // namespace

KYTY_PROPERTY(15, "Settings_Store round trip") {
	return rc::check(name, []() {
		const LauncherSettings stored = ValidSettings();
		const std::string      bytes  = kyty::core::SerializeSettings(stored);
		RC_LOG() << bytes << "\n";

		const auto parsed = kyty::core::ParseSettings(bytes);
		RC_ASSERT(parsed.defaulted_keys.empty());
		AssertEqual(parsed.settings, stored);
		RC_ASSERT(kyty::core::SerializeSettings(parsed.settings) == bytes);

		const KytyTests::TempDir dir("kyty_p15");
		const auto               ini = dir.Path() / "Kyty.ini";
		RC_ASSERT(kyty::core::SaveSettings(ini, stored).has_value());
		const auto loaded = kyty::core::LoadSettings(ini);
		RC_ASSERT(loaded.has_value());
		RC_ASSERT(loaded->defaulted_keys.empty());
		AssertEqual(loaded->settings, stored);

		RC_TAG(stored.games.empty() ? "no game" : "games");
		RC_TAG(stored.unknown.Entries().empty() ? "no unknown key" : "unknown keys");
	});
}
