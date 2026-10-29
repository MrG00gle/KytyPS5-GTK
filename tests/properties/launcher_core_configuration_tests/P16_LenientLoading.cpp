// Feature: linux-gtk-modernization, Property 16: Lenient loading of Qt-written stores
//
// Validates Requirements 7.11 and 9.6. Valid LauncherSettings (configurationGenerators.h) are
// serialized to Kyty.ini; then every schema key is kept, deleted, or replaced by a value that does
// not read as a valid value of its field (rates of 0, 1/20, or 1/5 per key and case): unparsable
// text of the field's kind, a list or opaque value where a string or list is expected, the
// Requirement 9.4-invalid values (user name, user ID, vblank, trophy duration, an empty output
// path while its output is on), invalid grid widths, window sizes, and controller colors, and
// gpu_index, console_language, and elf rule breakers. GameConfigurations/size and each element's
// game_path are left alone: they identify the elements. LoadSettings must give each deleted or
// corrupted field its default and every other field its stored value (unknown keys included), and
// report exactly corrupted ∪ (deleted − optional) in defaulted_keys (plan D20), where the optional
// keys are LauncherWindow/*, GameConfigurations/N/input_mapping_custom, and the preserved
// MainDialog/theme and MainDialog/geometry. In a third of the cases the store is a legacy one
// (review-u6 finding 4): game_dirs is dropped, the Game_Folders are in game_dir (kept or
// corrupted), and Launcher/addons_dir, grid_view, and grid_icon_width are each deleted with a
// probability of at least 1/2; in a legacy store they are optional too, so a missing one gets its
// default without a report.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/iniFile.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <algorithm>
#include <rapidcheck.h>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;
using kyty::core::FieldKind;
using kyty::core::GameConfiguration;
using kyty::core::IniFile;
using kyty::core::IniValue;
using kyty::core::LauncherSettings;

enum class Action { Keep, Delete, Corrupt };

struct Corruption {
	IniFile&               file;
	std::set<std::string>& defaulted;
	int                    rate; // 1 in rate keys is deleted or corrupted
	int                    deleted   = 0;
	int                    corrupted = 0;

	Action Pick() const {
		if (!OneIn(rate)) {
			return Action::Keep;
		}
		return OneIn(2) ? Action::Delete : Action::Corrupt;
	}

	// Applies the action to key; true when the field must get its default.
	bool Apply(Action action, const std::string& key, bool optional, const IniValue& invalid) {
		if (action == Action::Keep) {
			return false;
		}
		if (action == Action::Delete) {
			file.Remove(key);
			deleted++;
			if (!optional) {
				defaulted.insert(key);
			}
		} else {
			RC_ASSERT(file.Set(key, invalid));
			corrupted++;
			defaulted.insert(key);
		}
		return true;
	}

	// A key outside the field tables.
	template <typename T>
	void Key(const std::string& key, bool optional, const IniValue& invalid, T& target,
	         const T& fallback) {
		if (Apply(Pick(), key, optional, invalid)) {
			target = fallback;
		}
	}

	template <typename Target>
	void Field(const kyty::core::BasicFieldDescriptor<Target>& field, const std::string& key,
	           bool optional, Target& expected) {
		if (Apply(Pick(), key, optional, InvalidStoredValue(field.name, field.kind))) {
			RC_ASSERT(field.set(expected, field.get(Target {})));
		}
	}

	// The fields of a stored configuration under prefix.
	void Configuration(const std::string& prefix, bool per_game, const GameConfiguration& stored,
	                   GameConfiguration& expected) {
		std::vector<std::pair<const kyty::core::FieldDescriptor*, Action>> actions;
		std::set<std::string>                                              touched;
		for (const auto& field: kyty::core::GameConfigurationFields()) {
			if (!per_game && field.name == kyty::core::kInputMappingCustomField) {
				continue;
			}
			actions.emplace_back(&field, Pick());
			if (actions.back().second != Action::Keep) {
				touched.insert(std::string(field.name));
			}
		}
		const auto output_of = [](std::string_view path) -> std::string {
			if (path == "shader_log_folder") {
				return "shader_log_direction";
			}
			if (path == "command_buffer_dump_folder") {
				return "command_buffer_dump_enabled";
			}
			if (path == "printf_output_file") {
				return "printf_direction";
			}
			return {};
		};
		for (const auto& [field, action]: actions) {
			const auto key      = prefix + std::string(field->name);
			const bool optional = field->name == kyty::core::kInputMappingCustomField;
			auto       invalid  = InvalidStoredValue(field->name, field->kind);
			// An empty output path is invalid only while its output is on: only when the output
			// field keeps its stored value and that value is on.
			const auto output = output_of(field->name);
			if (!output.empty() && !touched.contains(output) && OneIn(2)) {
				auto probe = stored;
				RC_ASSERT(field->set(probe, kyty::core::FieldValue(std::string())));
				if (kyty::core::IsMissingOutputPath(field->name, probe)) {
					invalid = IniValue::FromString("");
				}
			}
			if (Apply(action, key, optional, invalid)) {
				RC_ASSERT(field->set(expected, field->get(GameConfiguration {})));
			}
		}
	}
};

} // namespace

KYTY_PROPERTY(16, "Lenient loading of Qt-written stores") {
	return rc::check(name, []() {
		const LauncherSettings        stored = ValidSettings();
		IniFile                       file = IniFile::Parse(kyty::core::SerializeSettings(stored));
		LauncherSettings              expected = stored;
		std::set<std::string>         defaulted;
		static const std::vector<int> rates = {1000000, 20, 5};
		Corruption                    c {file, defaulted, PickOf(rates)};
		const LauncherSettings        defaults;
		const auto                    text = [](const std::vector<std::string>& texts) {
			return IniValue::FromString(PickOf(texts));
		};
		const auto bool_text = [&] { return text({"maybe", "TRUE", "1", "", "yes"}); };

		// A legacy store (review-u6 finding 4): the Game_Folders are in game_dir, and each Launcher
		// key that came later is deleted half of the time on top of the rate; those keys are
		// optional there. game_dir makes the store legacy, so it is kept or corrupted, never
		// deleted.
		const bool               legacy = OneIn(3);
		std::vector<std::string> legacy_deleted;
		if (legacy) {
			RC_ASSERT(MakeLegacyStore(file, stored.game_folders));
			if (c.Pick() != Action::Keep && c.Apply(Action::Corrupt, "Launcher/game_dir", false,
			                                        IniValue::FromByteArray("x"))) {
				expected.game_folders = defaults.game_folders;
			}
		} else {
			c.Key("Launcher/game_dirs", false, IniValue::FromByteArray("x"), expected.game_folders,
			      defaults.game_folders);
		}
		const auto launcher_key = [&](const std::string& key, const IniValue& invalid, auto& target,
		                              const auto& fallback) {
			auto action = c.Pick();
			if (legacy && action == Action::Keep && OneIn(2)) {
				action = Action::Delete;
			}
			if (legacy && action == Action::Delete) {
				legacy_deleted.push_back(key);
			}
			if (c.Apply(action, key, legacy, invalid)) {
				target = fallback;
			}
		};
		launcher_key("Launcher/addons_dir",
		             OneIn(2) ? IniValue::FromStringList({"a", "b"}) : IniValue::FromByteArray("x"),
		             expected.addons_dir, defaults.addons_dir);
		launcher_key("Launcher/grid_view", bool_text(), expected.grid_view, defaults.grid_view);
		launcher_key("Launcher/grid_icon_width", text({"100", "330", "225", "0", "abc", "-224"}),
		             expected.grid_icon_width, defaults.grid_icon_width);
		c.Key("MainDialog/check_updates_on_startup", false, bool_text(),
		      expected.check_updates_on_startup, defaults.check_updates_on_startup);
		// Preserved raw values: only deleted (any text is kept as it is).
		if (stored.theme && OneIn(c.rate)) {
			file.Remove("MainDialog/theme");
			expected.theme.reset();
			c.deleted++;
		}
		if (stored.geometry && OneIn(c.rate)) {
			file.Remove("MainDialog/geometry");
			expected.geometry.reset();
			c.deleted++;
		}
		const auto size_text = [&] { return text({"0", "-5", "abc", "", "2147483648"}); };
		c.Key("LauncherWindow/width", true, size_text(), expected.window.width,
		      defaults.window.width);
		c.Key("LauncherWindow/height", true, size_text(), expected.window.height,
		      defaults.window.height);
		c.Key("LauncherWindow/maximized", true, bool_text(), expected.window.maximized,
		      defaults.window.maximized);

		c.Configuration("GlobalConfiguration/", false, stored.global, expected.global);
		for (const auto& field: kyty::core::GlobalOnlyFields()) {
			c.Field(field, "GlobalConfiguration/" + std::string(field.name), false,
			        expected.controller);
		}

		for (size_t i = 0; i < stored.games.size(); i++) {
			const auto prefix = "GameConfigurations/" + std::to_string(i + 1) + "/";
			auto&      game   = expected.games[i];
			const auto invalid_text =
			    OneIn(2) ? IniValue::FromStringList({"a", "b"}) : IniValue::FromByteArray("x");
			c.Key(prefix + "name", false, invalid_text, game.name, std::string());
			c.Key(prefix + "basedir", false, invalid_text, game.basedir, std::string());
			bool custom = true;
			c.Key(prefix + "custom_settings", false, bool_text(), custom, true);
			c.Configuration(prefix, true, stored.games[i].configuration, game.configuration);
		}

		const auto report = kyty::core::ParseSettings(file.Serialize());
		RC_LOG() << "deleted " << c.deleted << ", corrupted " << c.corrupted << "\n";
		for (const auto& key: report.defaulted_keys) {
			RC_LOG() << "reported " << key << "\n";
		}
		for (const auto& key: defaulted) {
			RC_LOG() << "expected " << key << "\n";
		}
		RC_ASSERT(std::set<std::string>(report.defaulted_keys.begin(),
		                                report.defaulted_keys.end()) == defaulted);
		RC_ASSERT(report.defaulted_keys.size() == defaulted.size());
		// The legacy optional-key rule: a missing key gets its default without a report.
		if (legacy) {
			for (const auto& key: LegacyOptionalKeys()) {
				if (file.Find(key) == nullptr) {
					RC_LOG() << "legacy optional key missing: " << key << "\n";
					RC_ASSERT(std::count(report.defaulted_keys.begin(), report.defaulted_keys.end(),
					                     key) == 0);
				}
			}
			RC_ASSERT(report.settings.game_folders == expected.game_folders);
			RC_ASSERT(report.settings.addons_dir == expected.addons_dir);
			RC_ASSERT(report.settings.grid_view == expected.grid_view);
			RC_ASSERT(report.settings.grid_icon_width == expected.grid_icon_width);
		}
		RC_ASSERT(report.settings.game_folders == expected.game_folders);
		RC_ASSERT(report.settings.global == expected.global);
		RC_ASSERT(report.settings.controller == expected.controller);
		RC_ASSERT(report.settings.games == expected.games);
		RC_ASSERT(report.settings.unknown == expected.unknown);
		RC_ASSERT(report.settings == expected);

		RC_TAG(c.deleted + c.corrupted == 0 ? "unchanged"
		       : c.corrupted == 0           ? "deleted only"
		       : c.deleted == 0             ? "corrupted only"
		                                    : "deleted and corrupted");
		RC_TAG(stored.games.empty() ? "no game" : "games");
		RC_TAG(!legacy                  ? "game_dirs store"
		       : legacy_deleted.empty() ? "legacy store, optional keys kept"
		                                : "legacy store, optional keys deleted");
	});
}
