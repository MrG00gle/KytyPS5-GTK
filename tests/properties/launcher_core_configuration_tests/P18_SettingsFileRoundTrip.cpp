// Feature: linux-gtk-modernization, Property 18: Game settings export and import round trip
//
// Validates Requirements 9.8 and 9.11. For a valid configuration (configurationGenerators.h) and a
// title ID that is not empty after trimming (PPSA codes and other text, mixed letter case,
// whitespace around): ParseGameSettingsFile(ExportGameSettings(id, config)) gives exactly one
// member, keyed by the trimmed upper-case ID, with every one of the 35 fields at its exported
// value, and applying it to any base configuration (the default or another valid one) gives
// config. Now and then the file goes through WriteGameSettingsFile and ReadGameSettingsFile.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameSettingsFile.h"
#include "support/configurationGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <rapidcheck.h>
#include <string>

namespace {

using namespace KytyTests::ConfigGen;

} // namespace

KYTY_PROPERTY(18, "Game settings export and import round trip") {
	return rc::check(name, []() {
		const auto config = ValidConfiguration();
		const auto title  = TitleId();
		const auto key    = kyty::core::NormalizeTitleId(title);
		RC_LOG() << "title '" << title << "' -> '" << key << "'\n";
		RC_ASSERT(!key.empty());

		const auto bytes = kyty::core::ExportGameSettings(title, config);
		RC_ASSERT(bytes.has_value());
		RC_LOG() << *bytes;

		const bool through_file = OneIn(4);
		auto       parsed       = kyty::core::ParseGameSettingsFile(*bytes);
		if (through_file) {
			const KytyTests::TempDir dir("kyty_p18");
			const auto               path = dir.Path() / "settings.json";
			RC_ASSERT(kyty::core::WriteGameSettingsFile(path, title, config).has_value());
			parsed = kyty::core::ReadGameSettingsFile(path);
		}
		if (!parsed) {
			RC_LOG() << parsed.error().message << "\n";
		}
		RC_ASSERT(parsed.has_value());
		RC_ASSERT(parsed->by_title.size() == 1u);
		RC_ASSERT(parsed->by_title.contains(key));

		const auto& patch = parsed->by_title.at(key);
		RC_ASSERT(patch.fields.size() == kyty::core::GameConfigurationFields().size());
		for (const auto& field: kyty::core::GameConfigurationFields()) {
			const auto it = patch.fields.find(std::string(field.name));
			RC_ASSERT(it != patch.fields.end());
			RC_ASSERT(it->second == field.get(config));
		}

		const auto base = OneIn(2) ? kyty::core::GameConfiguration {} : ValidConfiguration();
		RC_ASSERT(kyty::core::ApplyPatch(base, patch) == config);

		const auto selected = kyty::core::SelectGameImport(*parsed, title);
		RC_ASSERT(selected.has_value());
		RC_ASSERT(!selected->needs_confirmation);

		RC_TAG(through_file ? "through a file" : "in memory");
	});
}
