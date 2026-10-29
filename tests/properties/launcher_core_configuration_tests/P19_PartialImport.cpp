// Feature: linux-gtk-modernization, Property 19: Partial import changes only the fields present
//
// Validates Requirement 9.9. A base configuration and a source configuration, both valid with
// non-empty output paths (so that any mix of the two is valid); a Game_Settings_File whose member
// holds an arbitrary non-empty subset of the source's fields, each written in one of the accepted
// forms (the text form as a JSON string, a native JSON boolean or integer for those kinds, an enum
// as its key, its legacy integer index, or for a resolution its "1280x720" spelling, a string list
// as an array of strings). ApplyPatch sets every field in the file to the source's value and keeps
// every other field at the base's value; storing the result for the game (which had custom
// settings equal to the base, or none, with the base as the Global_Configuration) marks it custom
// with exactly that configuration and changes no other game.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <rapidcheck.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;

} // namespace

KYTY_PROPERTY(19, "Partial import changes only the fields present") {
	return rc::check(name, []() {
		auto       base   = ValidConfiguration(true);
		const auto source = ValidConfiguration(true);

		std::set<std::string>                            subset;
		std::vector<std::pair<std::string, std::string>> fields;
		const auto all  = kyty::core::GameConfigurationFields();
		const int  rate = Choose(1, 6);
		for (const auto& field: all) {
			if (OneIn(rate)) {
				subset.insert(std::string(field.name));
			}
		}
		if (subset.empty()) {
			subset.insert(std::string(all[Choose<size_t>(0, all.size())].name));
		}
		for (const auto& field: all) {
			if (subset.contains(std::string(field.name))) {
				fields.emplace_back(std::string(field.name), EncodeField(field, field.get(source)));
			}
		}
		const std::string title = "PPSA" + std::to_string(Choose(10000, 100000));
		const std::string file  = JsonObject({{title, JsonObject(fields)}});
		RC_LOG() << file << "\n";

		const auto parsed = kyty::core::ParseGameSettingsFile(file);
		if (!parsed) {
			RC_LOG() << parsed.error().message << "\n";
		}
		RC_ASSERT(parsed.has_value());
		const auto selected = kyty::core::SelectGameImport(*parsed, title);
		RC_ASSERT(selected.has_value());
		RC_ASSERT(!selected->needs_confirmation);
		RC_ASSERT(selected->patch.fields.size() == subset.size());

		const auto applied = kyty::core::ApplyPatch(base, selected->patch);
		for (const auto& field: all) {
			const bool present = subset.contains(std::string(field.name));
			RC_LOG() << field.name << (present ? " imported\n" : " kept\n");
			RC_ASSERT(field.get(applied) == field.get(present ? source : base));
		}

		// Stored for the game: it is marked custom with exactly that configuration.
		kyty::core::LauncherSettings settings = ValidSettings();
		kyty::core::GameEntry        entry;
		entry.path            = "/p19/" + title;
		entry.title_id        = title;
		const bool had_custom = OneIn(2);
		if (had_custom) {
			kyty::core::GameSettingsEntry stored;
			stored.game_path     = entry.path.string();
			stored.configuration = base;
			settings.games.push_back(stored);
		} else {
			base.input_mapping_custom = false;
			settings.global           = base;
		}
		const auto others = settings.games.size() - (had_custom ? 1 : 0);
		const auto start  = kyty::core::EnableCustomSettings(settings, entry);
		RC_ASSERT(start == base);
		const auto result = kyty::core::ApplyPatch(start, selected->patch);
		RC_ASSERT(kyty::core::SaveCustomSettings(settings, entry, result).has_value());
		RC_ASSERT(kyty::core::HasCustomSettings(settings, entry));
		RC_ASSERT(kyty::core::FindGameSettings(settings, entry)->configuration == result);
		RC_ASSERT(settings.games.size() == others + 1);

		RC_TAG(subset.size() == 1            ? "one field"
		       : subset.size() == all.size() ? "every field"
		                                     : "some fields");
		RC_TAG(had_custom ? "custom game" : "game with global settings");
	});
}
