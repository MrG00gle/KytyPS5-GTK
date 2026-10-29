// Feature: linux-gtk-modernization, Property 17: Custom settings lifecycle
//
// Validates Requirements 9.2, 9.3, and 9.13. For valid LauncherSettings (configurationGenerators.h)
// and a game that is either new or one of the stored games (found by its absolute path or by its
// legacy relative path):
// - enabling custom settings for a game without them gives a configuration equal to the
//   Global_Configuration (for a stored game, its own configuration) and changes nothing;
// - saving invalid values fails, names the field, and changes nothing; saving valid values stores
//   exactly those values, marks the game custom, keeps every other game, and survives a save and
//   load of Kyty.ini; the effective configuration is those values with the per-game mapping when
//   input_mapping_custom is set, else with the global one;
// - clearing removes the custom configuration: the game runs with the Global_Configuration, the
//   other games are unchanged, and the saved Kyty.ini has no element for it.

#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/iniFile.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <rapidcheck.h>
#include <string>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;
using kyty::core::GameEntry;
using kyty::core::GameSettingsEntry;
using kyty::core::LauncherSettings;

// The stored games other than the ones for entry, in order.
std::vector<GameSettingsEntry> Others(const LauncherSettings& settings, const GameEntry& entry) {
	std::vector<GameSettingsEntry> others;
	for (const auto& game: settings.games) {
		const bool absolute = game.game_path == entry.path.string();
		const bool legacy   = !game.game_path.empty() && game.game_path.front() != '/' &&
		                      game.game_path == entry.legacy_relative_path;
		if (!absolute && !legacy) {
			others.push_back(game);
		}
	}
	return others;
}

// True when the Kyty.ini text has an element for the game.
bool HasElement(const std::string& ini, const GameEntry& entry) {
	const auto file = kyty::core::IniFile::Parse(ini);
	for (const auto& [key, value]: file.Entries()) {
		if (key.starts_with("GameConfigurations/") && key.ends_with("/game_path")) {
			const auto path = file.GetString(key);
			if (path == entry.path.string() ||
			    (!entry.legacy_relative_path.empty() && path == entry.legacy_relative_path)) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

KYTY_PROPERTY(17, "Custom settings lifecycle") {
	return rc::check(name, []() {
		LauncherSettings settings = ValidSettings();
		const bool       stored   = !settings.games.empty() && OneIn(2);
		GameEntry        entry;
		if (stored) {
			entry = EntryFor(PickOf(settings.games));
		} else {
			entry.path       = "/p17/new/" + AnyText(4);
			entry.title_name = AnyText();
			if (OneIn(2)) {
				entry.legacy_relative_path = "p17/" + AnyText(3);
			}
		}
		RC_LOG() << "game " << entry.path.string() << " (" << entry.legacy_relative_path << ")\n";
		const auto others = Others(settings, entry);

		// Enable.
		const auto before = settings;
		const auto seeded = kyty::core::EnableCustomSettings(settings, entry);
		if (stored) {
			RC_ASSERT(kyty::core::HasCustomSettings(settings, entry));
			RC_ASSERT(seeded == kyty::core::FindGameSettings(settings, entry)->configuration);
		} else {
			RC_ASSERT(!kyty::core::HasCustomSettings(settings, entry));
			RC_ASSERT(seeded == settings.global);
		}
		RC_ASSERT(settings == before);

		// Save invalid values.
		auto invalid = ValidConfiguration();
		switch (Choose(0, 3)) {
			case 0: invalid.user_id = OneIn(2) ? 254 : 255; break;
			case 1: invalid.vblank_frequency = 361; break;
			default: invalid.user_name = ""; break;
		}
		const auto rejected = kyty::core::SaveCustomSettings(settings, entry, invalid);
		RC_ASSERT(!rejected.has_value());
		RC_ASSERT(!rejected.error().field.empty());
		RC_ASSERT(settings == before);

		// Save valid values.
		const auto values = ValidConfiguration();
		RC_ASSERT(kyty::core::SaveCustomSettings(settings, entry, values).has_value());
		RC_ASSERT(kyty::core::HasCustomSettings(settings, entry));
		RC_ASSERT(kyty::core::FindGameSettings(settings, entry)->configuration == values);
		RC_ASSERT(Others(settings, entry) == others);
		RC_ASSERT(settings.global == before.global);
		const auto effective = kyty::core::ResolveEffectiveConfiguration(settings, entry, {});
		auto       wanted    = values;
		if (!values.input_mapping_custom) {
			wanted.host_input_mapping = settings.global.host_input_mapping;
		}
		RC_ASSERT(effective.configuration == wanted);
		RC_ASSERT(effective.controller == settings.controller);

		const auto saved    = kyty::core::SerializeSettings(settings);
		const auto reloaded = kyty::core::ParseSettings(saved);
		RC_ASSERT(reloaded.defaulted_keys.empty());
		RC_ASSERT(HasElement(saved, entry));
		RC_ASSERT(kyty::core::FindGameSettings(reloaded.settings, entry) != nullptr);
		RC_ASSERT(kyty::core::FindGameSettings(reloaded.settings, entry)->configuration == values);

		// Clear.
		RC_ASSERT(kyty::core::ClearCustomSettings(settings, entry));
		RC_ASSERT(!kyty::core::HasCustomSettings(settings, entry));
		RC_ASSERT(Others(settings, entry) == others);
		RC_ASSERT(settings.games == others);
		RC_ASSERT(kyty::core::ResolveEffectiveConfiguration(settings, entry, {}).configuration ==
		          settings.global);
		const auto cleared = kyty::core::SerializeSettings(settings);
		RC_ASSERT(!HasElement(cleared, entry));
		const auto after = kyty::core::ParseSettings(cleared);
		RC_ASSERT(after.defaulted_keys.empty());
		RC_ASSERT(!kyty::core::HasCustomSettings(after.settings, entry));

		RC_TAG(stored ? "stored game" : "new game");
		RC_TAG(values.input_mapping_custom ? "per-game mapping" : "global mapping");
	});
}
