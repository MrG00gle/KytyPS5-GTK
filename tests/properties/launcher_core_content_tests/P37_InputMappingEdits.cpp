// Feature: linux-gtk-modernization, Property 37: Input mapping edits
//
// Validates Requirement 11.14. Each case stores an initial host_input_mapping (entries of every
// kind from KytyTests::ContentGen::MappingEntryPool, random keyboard, mouse, and gamepad bindings)
// as the global list or as one game's list (the game has no custom settings, custom settings
// without input_mapping_custom, or custom settings with it) in a real Kyty.ini, opens the editor's
// model on MappingForTarget, and applies 0-15 editor operations: assign a key, mouse button, or pad
// source (some refused: unknown controls, reserved or unknown keys, stick directions, unknown
// sources), unmap, restore a tab's defaults, and change the mouse sensitivity, particle skin, or
// touch cursor. Each operation must do what it says (a binding is set and is the only one to its
// input; a refused one changes nothing); ParseMapping(SerializeMapping(model)) must equal the
// edited model; neither the edits nor SaveMapping may change the Kyty.ini bytes; and after the
// caller saves, the stored mapping of the target (global, or the game with input_mapping_custom)
// must equal SerializeMapping(model) while the other lists stay as they were. A model with an
// unknown entry that Validate refuses (no "=") must not be saved and leave the settings unchanged.
// Kyty.ini is opened with OpenSettingsStore and saved with SaveSettingsStore, as the GTK launcher
// does (its first save creates the missing Kyty directory).

#include "launcher/core/atomicFile.h"
#include "launcher/core/inputMapping.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/text.h"
#include "support/contentGenerators.h"
#include "support/property.h"
#include "support/tempDir.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;
using kyty::core::InputDevice;
using kyty::core::InputMappingModel;
using kyty::core::MappingTarget;

template <typename T>
T Choose(T min, T max) {
	return *rc::gen::resize(100, rc::gen::inRange<T>(min, max));
}

bool OneIn(int n) {
	return Choose(0, n) == 0;
}

template <typename T>
const T& Pick(const std::vector<T>& values) {
	return values[Choose<std::size_t>(0, values.size())];
}

std::string Control() {
	if (OneIn(15)) {
		return Pick(std::vector<std::string> {"Nothing", "cross", ""});
	}
	const auto controls = kyty::core::InputControls();
	return std::string(controls[Choose<std::size_t>(0, controls.size())].id);
}

std::string Key() {
	if (OneIn(8)) {
		return Pick(
		    std::vector<std::string> {"F1", "Escape", "F11", "NotAKey", "", " ", "Mouse:Up"});
	}
	return Pick(ContentGen::KeyNamePool());
}

std::string Source() {
	if (OneIn(10)) {
		return Pick(std::vector<std::string> {"Nothing", "south", ""});
	}
	const auto sources = kyty::core::PadSources();
	return std::string(sources[Choose<std::size_t>(0, sources.size())].id);
}

std::vector<std::string> InitialMapping() {
	std::vector<std::string> entries;
	const int                count = Choose(0, 8);
	for (int i = 0; i < count; i++) {
		switch (Choose(0, 3)) {
			case 0: entries.push_back(Pick(ContentGen::MappingEntryPool())); break;
			case 1: entries.push_back(Control() + "=" + Pick(ContentGen::KeyNamePool())); break;
			default: entries.push_back("Pad:" + Control() + "=" + Source()); break;
		}
	}
	return entries;
}

ContentGen::MappingEdit Edit() {
	using Kind = ContentGen::MappingEdit::Kind;
	ContentGen::MappingEdit edit;
	const auto              device = OneIn(2) ? InputDevice::Pad : InputDevice::KeyboardMouse;
	switch (Choose(0, 9)) {
		case 0:
		case 1:
		case 2:
			edit = {Kind::Assign, device, Control(), device == InputDevice::Pad ? Source() : Key()};
			break;
		case 3: edit = {Kind::Unmap, device, Control()}; break;
		case 4:
			edit.kind = Kind::Restore;
			edit.tab =
			    OneIn(2) ? kyty::core::InputTab::Gamepad : kyty::core::InputTab::KeyboardMouse;
			break;
		case 5:
			edit.kind   = Kind::Sensitivity;
			edit.tenths = Choose(-5, 60);
			break;
		case 6:
		case 7: {
			edit.kind          = Kind::Skin;
			const auto skins   = kyty::core::TouchParticleSkins();
			edit.particle_skin = skins[Choose<std::size_t>(0, skins.size())].skin;
			break;
		}
		default:
			edit.kind         = Kind::Cursor;
			edit.touch_cursor = OneIn(2);
			break;
	}
	return edit;
}

// The checks of one operation on the model after it.
void CheckEdit(const ContentGen::MappingEdit& edit, bool applied, const InputMappingModel& before,
               const InputMappingModel& after) {
	using Kind = ContentGen::MappingEdit::Kind;
	if (!applied) {
		RC_ASSERT(after == before);
		return;
	}
	const auto& bindings = edit.device == InputDevice::Pad ? after.pad : after.keyboard;
	switch (edit.kind) {
		case Kind::Assign: {
			const auto found = bindings.find(edit.control);
			RC_ASSERT(found != bindings.end() && found->second == edit.input);
			for (const auto& [control, input]: bindings) {
				if (control != edit.control) {
					RC_ASSERT(kyty::core::CompareCaseInsensitive(input, edit.input) != 0);
				}
			}
			const bool custom =
			    edit.device == InputDevice::Pad ? after.pad_custom : after.keyboard_custom;
			RC_ASSERT(custom);
			break;
		}
		case Kind::Unmap: RC_ASSERT(!bindings.contains(edit.control)); break;
		case Kind::Restore: {
			const auto defaults = kyty::core::ParseMapping({});
			if (edit.tab == kyty::core::InputTab::Gamepad) {
				RC_ASSERT(after.pad == defaults.pad && !after.pad_custom);
			} else {
				RC_ASSERT(after.keyboard == defaults.keyboard && !after.keyboard_custom &&
				          after.mouse_sensitivity_tenths == 10);
			}
			RC_ASSERT(after.unknown == before.unknown &&
			          after.touch_cursor == before.touch_cursor &&
			          after.particle_skin == before.particle_skin);
			break;
		}
		case Kind::Sensitivity:
			RC_ASSERT(after.mouse_sensitivity_tenths == std::clamp(edit.tenths, 1, 50));
			break;
		case Kind::Skin: RC_ASSERT(after.particle_skin == edit.particle_skin); break;
		case Kind::Cursor: RC_ASSERT(after.touch_cursor == edit.touch_cursor); break;
	}
}

} // namespace

KYTY_PROPERTY(37, "Input mapping edits") {
	return rc::check(name, []() {
		const KytyTests::TempDir temp("kyty_p37");
		const auto               ini = temp.Path() / "Kyty" / "Kyty.ini";

		kyty::core::GameEntry game;
		game.path         = temp.Path() / "games" / "Game";
		game.title_name   = "Game";
		game.title_id     = "PPSA01234";
		const auto target = OneIn(2) ? MappingTarget::Global : MappingTarget::Game;

		kyty::core::LauncherSettings settings;
		settings.global.host_input_mapping = InitialMapping();
		const int game_state               = Choose(0, 3); // none, custom, custom mapping
		if (game_state != 0) {
			auto configuration                 = settings.global;
			configuration.user_name            = "Game user";
			configuration.input_mapping_custom = game_state == 2;
			configuration.host_input_mapping   = InitialMapping();
			kyty::core::GameSettingsEntry element;
			element.name          = game.title_name;
			element.basedir       = game.path.string();
			element.game_path     = game.path.string();
			element.configuration = configuration;
			settings.games.push_back(std::move(element));
		}
		// The first save creates the missing Kyty directory, as in a fresh XDG_CONFIG_HOME.
		auto initial = kyty::core::OpenSettingsStore(ini);
		RC_ASSERT(!initial.load_error.has_value());
		RC_ASSERT(kyty::core::SaveSettingsStore(initial, settings).has_value());
		// The editor opens the Settings_Store as the GTK launcher does.
		auto store = kyty::core::OpenSettingsStore(ini);
		RC_ASSERT(!store.load_error.has_value());
		settings         = store.loaded.settings;
		const auto bytes = kyty::core::ReadFileLimited(ini, kyty::core::kMaxSettingsFileBytes);
		RC_ASSERT(bytes.has_value());
		const auto original = settings;

		auto model = kyty::core::ParseMapping(kyty::core::MappingForTarget(settings, target, game));
		const int edits = Choose(0, 16);
		for (int i = 0; i < edits; i++) {
			const auto edit    = Edit();
			const auto before  = model;
			const bool applied = ContentGen::ApplyMappingEdit(model, edit);
			RC_LOG() << "edit " << static_cast<int>(edit.kind) << " " << edit.control << " '"
			         << edit.input << "': " << applied << "\n";
			CheckEdit(edit, applied, before, model);
		}
		const auto serialized = kyty::core::SerializeMapping(model);
		RC_ASSERT(kyty::core::ParseMapping(serialized) == model);

		const bool valid     = kyty::core::FindInvalidMappingEntries(serialized).empty();
		const auto saved     = kyty::core::SaveMapping(settings, target, game, model);
		const auto unchanged = kyty::core::ReadFileLimited(ini, kyty::core::kMaxSettingsFileBytes);
		// Nothing reaches Kyty.ini before Save. (RC_ASSERT does not short-circuit &&, so the value
		// is checked first.)
		RC_ASSERT(unchanged.has_value());
		if (unchanged.has_value()) {
			RC_ASSERT(*unchanged == *bytes);
		}
		if (!valid) {
			RC_ASSERT(!saved.has_value());
			RC_ASSERT(saved.error().field == "host_input_mapping");
			RC_ASSERT(settings == original);
			RC_TAG("refused: an entry without '='");
			return;
		}
		RC_ASSERT(saved.has_value());

		// The caller saves the Settings_Store through SaveSettingsStore, as the GTK launcher does.
		RC_ASSERT(kyty::core::SaveSettingsStore(store, settings).has_value());
		const auto reopened = kyty::core::OpenSettingsStore(ini);
		RC_ASSERT(!reopened.load_error.has_value());
		const auto& stored = reopened.loaded.settings;
		RC_ASSERT(kyty::core::MappingForTarget(stored, target, game) == serialized);
		if (target == MappingTarget::Game) {
			const auto* custom = kyty::core::FindGameSettings(stored, game);
			// RC_ASSERT does not short-circuit: check the pointer before using it.
			RC_ASSERT(custom != nullptr);
			if (custom != nullptr) {
				RC_ASSERT(custom->configuration.input_mapping_custom);
				if (game_state != 0) {
					RC_ASSERT(custom->configuration.user_name == "Game user");
				}
			}
			RC_ASSERT(stored.global.host_input_mapping == original.global.host_input_mapping);
		} else {
			RC_ASSERT(stored.global.host_input_mapping == serialized);
			RC_ASSERT(stored.games == original.games);
		}
		RC_TAG(target == MappingTarget::Global ? "global" : "game");
		RC_TAG(edits == 0 ? "no edit" : "edits");
	});
}
