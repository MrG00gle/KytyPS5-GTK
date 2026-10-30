// Example tests of the input mapping editor's model (src/launcher/core/inputMapping.h, task 16.4;
// Requirement 11.14): the control, pad source, and skin tables against the Emulator's
// (src/graphics/presentation/window/hostInput.cpp), parsing host_input_mapping entries as the
// Emulator reads them (defaults, Disabled entries, conflicts, the last mouse sensitivity, the first
// known skin, unknown entries kept), the serialized order, assign, unmap, restore defaults, and
// saving to the global or a per-game target without touching Kyty.ini before the caller saves it
// through SaveSettingsStore.

#include "launcher/core/inputMapping.h"
#include "launcher/core/keyNames.h"
#include "launcher/core/settingsStore.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

using kyty::core::InputDevice;
using kyty::core::InputMappingModel;
using kyty::core::InputTab;
using kyty::core::MappingTarget;
using kyty::core::ParseMapping;
using kyty::core::SerializeMapping;
using kyty::core::TouchParticleSkin;
using Entries = std::vector<std::string>;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "InputMappingTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string Join(const Entries& entries) {
	std::string text;
	for (const auto& entry: entries) {
		text += (text.empty() ? "[" : ", [") + entry + "]";
	}
	return text;
}

void CheckRoundTrip(const InputMappingModel& model, const std::string& what) {
	Check(ParseMapping(SerializeMapping(model)) == model,
	      what + ": ParseMapping(SerializeMapping(model)) == model; " +
	          Join(SerializeMapping(model)));
}

void TestTables() {
	// The Emulator's CONTROL_INFO names and PAD_SOURCES (hostInput.cpp).
	const std::set<std::string_view> emulator_controls = {"L3",
	                                                      "R3",
	                                                      "Options",
	                                                      "Up",
	                                                      "Right",
	                                                      "Down",
	                                                      "Left",
	                                                      "L2",
	                                                      "R2",
	                                                      "L1",
	                                                      "R1",
	                                                      "Triangle",
	                                                      "Circle",
	                                                      "Cross",
	                                                      "Square",
	                                                      "TouchPad",
	                                                      "TouchPadRight",
	                                                      "LeftStickLeft",
	                                                      "LeftStickRight",
	                                                      "LeftStickUp",
	                                                      "LeftStickDown",
	                                                      "RightStickLeft",
	                                                      "RightStickRight",
	                                                      "RightStickUp",
	                                                      "RightStickDown",
	                                                      "SpeakerVolume",
	                                                      "VibrationIntensity",
	                                                      "TriggerEffectIntensity",
	                                                      "GyroToggle",
	                                                      "TouchPadMode"};
	std::set<std::string_view>       ids;
	for (const auto& control: kyty::core::InputControls()) {
		Check(ids.insert(control.id).second, "unique id " + std::string(control.id));
		Check(kyty::core::FindInputControl(control.id) == &control, "found by id");
		const bool stick_direction =
		    control.id.starts_with("LeftStick") || control.id.starts_with("RightStick");
		Check(control.pad_mappable == !stick_direction,
		      "only stick directions are not pad-mappable: " + std::string(control.id));
		if (!control.keyboard_default.empty()) {
			Check(kyty::core::IsEmulatorKeyName(control.keyboard_default) &&
			          !kyty::core::IsReservedKeyName(control.keyboard_default),
			      "the default key of " + std::string(control.id) + " is a usable key name");
		}
		if (!control.pad_default.empty()) {
			Check(control.pad_tab && control.pad_mappable &&
			          kyty::core::PadSourceLabel(control.pad_default) != control.pad_default,
			      "the default pad source of " + std::string(control.id) + " is known");
		}
		Check(!kyty::core::InputSectionTitle(control.section).empty(), "a section title");
	}
	Check(ids == emulator_controls, "the controls are the Emulator's");
	for (const auto& id: {"LeftStickUp", "LeftStickDown", "LeftStickLeft", "LeftStickRight",
	                      "RightStickUp", "RightStickDown", "RightStickLeft", "RightStickRight"}) {
		Check(!kyty::core::FindInputControl(id)->pad_mappable,
		      std::string(id) + " is not pad-mappable");
	}
	Check(kyty::core::FindInputControl("TouchPad")->keyboard_label == "Touch pad left" &&
	          kyty::core::FindInputControl("TouchPad")->label == "Touch pad click",
	      "the touch pad's two labels");
	Check(kyty::core::FindInputControl("cross") == nullptr &&
	          kyty::core::FindInputControl("") == nullptr,
	      "ids are case-sensitive");
	Check(kyty::core::InputSectionTitle(kyty::core::InputSection::Shoulders) ==
	              "Shoulders & triggers" &&
	          kyty::core::InputSectionTitle(kyty::core::InputSection::Shortcuts) ==
	              "In-game shortcuts (cycle)",
	      "section titles");

	std::set<std::string_view> sources;
	for (const auto& source: kyty::core::PadSources()) {
		sources.insert(source.id);
		Check(kyty::core::PadSourceLabel(source.id) == source.label, "pad source label");
	}
	Check(sources.size() == 23 && sources.contains("South") && sources.contains("RightTrigger") &&
	          sources.contains("Misc1"),
	      "the Emulator's 23 pad sources");
	Check(kyty::core::PadSourceLabel("Nothing") == "Nothing", "an unknown source is its id");
	Check(kyty::core::TouchParticleSkins().size() == 5 &&
	          kyty::core::TouchParticleSkins()[4].key == "RedBlue" &&
	          kyty::core::TouchParticleSkins()[4].label == "Red/Blue",
	      "the five skins");
	for (const auto& name: kyty::core::kMouseButtonNames) {
		Check(kyty::core::IsMouseButtonName(name) && !kyty::core::IsEmulatorKeyName(name),
		      std::string(name) + " is a mouse button");
	}
	Check(!kyty::core::IsMouseButtonName("Mouse:left"), "mouse names are case-sensitive");
}

void TestDefaults() {
	const auto model = ParseMapping({});
	Check(!model.keyboard_custom && !model.pad_custom && model.unknown.empty(), "not custom");
	Check(model.keyboard.size() == 29 && model.keyboard.at("Cross") == "J" &&
	          model.keyboard.at("L3") == "Left Shift" &&
	          model.keyboard.at("TouchPadMode") == "F6" && !model.keyboard.contains("GyroToggle"),
	      "the keyboard defaults");
	Check(model.pad.size() == 17 && model.pad.at("Cross") == "South" &&
	          model.pad.at("GyroToggle") == "Back" && !model.pad.contains("TouchPadMode"),
	      "the gamepad defaults");
	Check(model.mouse_sensitivity_tenths == 10 &&
	          model.particle_skin == TouchParticleSkin::Classic && model.touch_cursor,
	      "the touch pad and mouse defaults");
	Check(SerializeMapping(model).empty(), "the defaults write no entry");
}

void TestParse() {
	auto model = ParseMapping(Entries {"Cross=K", "Circle=k", "R2=Mouse:Right", "L2=Mouse:Right"});
	Check(model.keyboard_custom && model.keyboard.size() == 2 &&
	          model.keyboard.at("Circle") == "k" && model.keyboard.at("L2") == "Mouse:Right",
	      "a later binding to the same input wins, ignoring letter case");
	Check(SerializeMapping(model) == Entries {"Circle=k", "L2=Mouse:Right"},
	      "written in control order: " + Join(SerializeMapping(model)));
	Check(model.pad.size() == 17 && !model.pad_custom, "the gamepad keeps its defaults");

	model = ParseMapping(Entries {"Cross=J", "Cross=L"});
	Check(model.keyboard.size() == 1 && model.keyboard.at("Cross") == "L", "the last binding wins");

	// SDL_strcasecmp stops at a NUL byte: a value with one must match byte for byte.
	const std::string with_nul("Cross=a\0b", 9);
	model = ParseMapping(Entries {"Circle=A", with_nul});
	Check(model.keyboard.size() == 2 && model.keyboard.at("Circle") == "A" &&
	          model.keyboard.at("Cross") == std::string("a\0b", 3),
	      "a value with a NUL byte does not take another control's key");
	CheckRoundTrip(model, "a value with a NUL byte");

	model = ParseMapping(Entries {"Keyboard=Disabled"});
	Check(model.keyboard_custom && model.keyboard.empty(), "Keyboard=Disabled");
	Check(SerializeMapping(model) == Entries {"Keyboard=Disabled"}, "written back");
	model = ParseMapping(Entries {"Keyboard=Disabled", "Cross=J"});
	Check(SerializeMapping(model) == Entries {"Cross=J"}, "Disabled plus a binding");

	model = ParseMapping(Entries {"Pad:Cross=South", "Pad:Circle=South", "Pad:TouchPadRight=Misc1",
	                              "Pad:SpeakerVolume=LeftPaddle1"});
	Check(model.pad_custom && model.pad.size() == 3 && model.pad.at("Circle") == "South",
	      "gamepad bindings and conflicts");
	Check(SerializeMapping(model) == Entries {"Pad:Circle=South", "Pad:TouchPadRight=Misc1",
	                                          "Pad:SpeakerVolume=LeftPaddle1"},
	      "gamepad entries: " + Join(SerializeMapping(model)));
	Check(model.keyboard.size() == 29 && !model.keyboard_custom, "the keyboard keeps its defaults");
	model = ParseMapping(Entries {"Pad=Disabled"});
	Check(model.pad_custom && model.pad.empty() &&
	          SerializeMapping(model) == Entries {"Pad=Disabled"},
	      "Pad=Disabled");

	const Entries unknown = {"Pad:LeftStickUp=North",
	                         "Pad:Cross=Nothing",
	                         "Pad:Unknown=South",
	                         "Pad:Cross",
	                         "Foo=Bar",
	                         "Keyboard=disabled",
	                         "Cross=",
	                         "=J",
	                         "NoEquals",
	                         "",
	                         "pad:Cross=South",
	                         "MouseSensitivity"};
	model                 = ParseMapping(unknown);
	Check(model.unknown == unknown && !model.keyboard_custom && !model.pad_custom,
	      "unknown entries are kept in order and change nothing");
	Check(SerializeMapping(model) == unknown, "and written back");

	struct Sensitivity {
		std::string text;
		int         tenths;
	};
	for (const auto& [text, tenths]: std::vector<Sensitivity> {{"2.5", 25},
	                                                           {"0.04", 1},
	                                                           {"9", 50},
	                                                           {" 1.26x", 13},
	                                                           {"nan", 10},
	                                                           {"inf", 10},
	                                                           {"", 1},
	                                                           {"abc", 1},
	                                                           {"+3", 30},
	                                                           {"-2", 1},
	                                                           {"1e999", 10},
	                                                           {"0.1", 1},
	                                                           {"5", 50},
	                                                           {"1.04", 10},
	                                                           {"\t4.96", 50},
	                                                           {"2.25", 23}}) {
		model = ParseMapping(Entries {"MouseSensitivity=" + text});
		Check(model.mouse_sensitivity_tenths == tenths,
		      "MouseSensitivity=" + text + " is " + std::to_string(model.mouse_sensitivity_tenths));
		Check(model.unknown.empty(), "a sensitivity entry is not unknown");
	}
	model = ParseMapping(Entries {"MouseSensitivity=2", "MouseSensitivity=3"});
	Check(model.mouse_sensitivity_tenths == 30, "the last sensitivity wins (the Emulator's rule)");

	model = ParseMapping(Entries {"TouchParticleSkin=Sparkles", "TouchParticleSkin=Crimson",
	                              "TouchParticleSkin=BlueNeon", "TouchCursorImage=On"});
	Check(model.particle_skin == TouchParticleSkin::Crimson && model.touch_cursor &&
	          model.unknown.empty(),
	      "the first known skin wins; other skin and cursor values are dropped");
	model = ParseMapping(Entries {"TouchCursorImage=Off", "TouchCursorImage=On"});
	Check(!model.touch_cursor, "TouchCursorImage=Off hides the cursor");

	model = ParseMapping(Entries {"NoEquals", "TouchCursorImage=Off", "Pad:Cross=East", "Cross=J",
	                              "TouchParticleSkin=RedBlue", "MouseSensitivity=0.5", "Foo=Bar"});
	Check(SerializeMapping(model) == Entries {"Cross=J", "Pad:Cross=East", "MouseSensitivity=0.5",
	                                          "TouchParticleSkin=RedBlue", "TouchCursorImage=Off",
	                                          "NoEquals", "Foo=Bar"},
	      "the serialized order: " + Join(SerializeMapping(model)));

	// Every pair of pool entries round-trips.
	const auto& pool = ContentGen::MappingEntryPool();
	for (const auto& first: pool) {
		for (const auto& second: pool) {
			CheckRoundTrip(ParseMapping(Entries {first, second}),
			               "[" + first + "] [" + second + "]");
		}
	}
	CheckRoundTrip(ParseMapping(pool), "the whole pool");

	// Bindings of non-controls and empty values are not written.
	InputMappingModel odd;
	odd.keyboard_custom = true;
	odd.keyboard        = {{"Cross", ""}, {"NotAControl", "J"}};
	Check(SerializeMapping(odd) == Entries {"Keyboard=Disabled"}, "nothing to write");
	odd.mouse_sensitivity_tenths = 99;
	Check(SerializeMapping(odd) == Entries {"Keyboard=Disabled", "MouseSensitivity=5.0"},
	      "the sensitivity is clamped when written");
}

void TestEdits() {
	auto model = ParseMapping({});
	Check(kyty::core::Assign(model, InputDevice::KeyboardMouse, "Circle", "j"), "assign j");
	Check(model.keyboard_custom && model.keyboard.at("Circle") == "j" &&
	          !model.keyboard.contains("Cross"),
	      "the key moves from Cross to Circle");
	Check(kyty::core::Assign(model, InputDevice::KeyboardMouse, "R2", "Mouse:Left"), "mouse");
	for (const std::string_view input:
	     {"F1", "Escape", "f11", "NotAKey", "", " ", "\t", "Mouse:left"}) {
		const auto before = model;
		Check(!kyty::core::Assign(model, InputDevice::KeyboardMouse, "Cross", input) &&
		          model == before,
		      "refused key '" + std::string(input) + "'");
	}
	Check(!kyty::core::Assign(model, InputDevice::KeyboardMouse, "Nothing", "J"),
	      "unknown control");
	Check(kyty::core::Assign(model, InputDevice::KeyboardMouse, "Square", "Keypad ,"),
	      "a key name with a comma");
	CheckRoundTrip(model, "keyboard edits");

	Check(kyty::core::Assign(model, InputDevice::Pad, "Cross", "East") && model.pad_custom &&
	          model.pad.at("Cross") == "East" && !model.pad.contains("Circle"),
	      "a pad source moves");
	Check(kyty::core::Assign(model, InputDevice::Pad, "TouchPadRight", "Misc1"),
	      "a control outside the Gamepad tab");
	Check(!kyty::core::Assign(model, InputDevice::Pad, "LeftStickUp", "North"),
	      "a stick direction");
	Check(!kyty::core::Assign(model, InputDevice::Pad, "Cross", "Nothing"), "an unknown source");
	Check(!kyty::core::Assign(model, InputDevice::Pad, "Cross", "south"), "sources are exact");
	CheckRoundTrip(model, "pad edits");

	Check(kyty::core::Unmap(model, InputDevice::KeyboardMouse, "Circle") &&
	          !model.keyboard.contains("Circle"),
	      "unmap a key");
	Check(kyty::core::Unmap(model, InputDevice::Pad, "Cross") && !model.pad.contains("Cross"),
	      "unmap a pad source");
	Check(!kyty::core::Unmap(model, InputDevice::Pad, "LeftStickUp") &&
	          !kyty::core::Unmap(model, InputDevice::KeyboardMouse, "Nothing"),
	      "unmap refused");
	auto fresh = ParseMapping({});
	Check(kyty::core::Unmap(fresh, InputDevice::KeyboardMouse, "GyroToggle") &&
	          fresh.keyboard_custom && fresh.keyboard.size() == 29,
	      "unmapping an unbound control makes the section custom (as Qt)");
	CheckRoundTrip(fresh, "unmap of an unbound control");

	model.particle_skin = TouchParticleSkin::Monochrome;
	model.touch_cursor  = false;
	kyty::core::SetMouseSensitivity(model, 0);
	Check(model.mouse_sensitivity_tenths == 1, "clamped to 0.1");
	kyty::core::SetMouseSensitivity(model, 51);
	Check(model.mouse_sensitivity_tenths == 50, "clamped to 5.0");
	kyty::core::SetMouseSensitivity(model, 17);
	CheckRoundTrip(model, "all settings");
	model.unknown = {"Foo=Bar"};

	kyty::core::RestoreDefaults(model, InputTab::Gamepad);
	Check(!model.pad_custom && model.pad == ParseMapping({}).pad && model.keyboard_custom &&
	          model.mouse_sensitivity_tenths == 17 &&
	          model.particle_skin == TouchParticleSkin::Monochrome && !model.touch_cursor,
	      "the Gamepad tab's defaults only");
	kyty::core::RestoreDefaults(model, InputTab::KeyboardMouse);
	Check(!model.keyboard_custom && model.keyboard == ParseMapping({}).keyboard &&
	          model.mouse_sensitivity_tenths == 10 &&
	          model.particle_skin == TouchParticleSkin::Monochrome && !model.touch_cursor &&
	          model.unknown == Entries {"Foo=Bar"},
	      "the keyboard tab's defaults and sensitivity; touch settings and unknown entries stay");
	Check(SerializeMapping(model) ==
	          Entries {"TouchParticleSkin=Monochrome", "TouchCursorImage=Off", "Foo=Bar"},
	      "after both restores: " + Join(SerializeMapping(model)));

	// The MappingEdit helper runs every kind.
	auto edited = ParseMapping({});
	using Kind  = ContentGen::MappingEdit::Kind;
	for (const auto& edit: std::vector<ContentGen::MappingEdit> {
	         {Kind::Assign, InputDevice::KeyboardMouse, "Cross", "Space"},
	         {Kind::Unmap, InputDevice::Pad, "Circle"},
	         {.kind = Kind::Restore, .tab = InputTab::Gamepad},
	         {.kind = Kind::Sensitivity, .tenths = 33},
	         {.kind = Kind::Skin, .particle_skin = TouchParticleSkin::BlueNeon},
	         {.kind = Kind::Cursor, .touch_cursor = false},
	     }) {
		Check(ContentGen::ApplyMappingEdit(edited, edit), "an edit applies");
	}
	// The keyboard is custom: all 29 bindings are written, then the three settings.
	const auto entries = SerializeMapping(edited);
	Check(entries.size() == 32 && entries[0] == "Cross=Space" &&
	          entries[29] == "MouseSensitivity=3.3" &&
	          entries[30] == "TouchParticleSkin=BlueNeon" && entries[31] == "TouchCursorImage=Off",
	      "edits: " + Join(entries));
	CheckRoundTrip(edited, "the MappingEdit sequence");
}

kyty::core::GameEntry Game(const std::string& path) {
	kyty::core::GameEntry entry;
	entry.path       = path;
	entry.title_name = "Test game";
	entry.title_id   = "PPSA01234";
	return entry;
}

void TestTargets() {
	kyty::core::LauncherSettings settings;
	settings.global.host_input_mapping = {"Cross=K"};
	settings.global.user_name          = "Global user";
	const auto game                    = Game("/games/a");
	const auto other                   = Game("/games/b");

	Check(kyty::core::MappingForTarget(settings, MappingTarget::Global, game) ==
	          Entries {"Cross=K"},
	      "the global list");
	Check(kyty::core::MappingForTarget(settings, MappingTarget::Game, game) == Entries {"Cross=K"},
	      "a game without custom settings edits the global list");
	Check(!kyty::core::UseGlobalMapping(settings, game), "nothing to clear");

	// Save for the game: custom settings seeded from the Global_Configuration.
	auto model = ParseMapping(kyty::core::MappingForTarget(settings, MappingTarget::Game, game));
	Check(kyty::core::Assign(model, InputDevice::KeyboardMouse, "Circle", "J"), "edit");
	auto before = settings;
	Check(kyty::core::SaveMapping(settings, MappingTarget::Game, game, model).has_value(),
	      "saved for the game");
	const auto* custom = kyty::core::FindGameSettings(settings, game);
	Check(custom != nullptr && custom->configuration.input_mapping_custom &&
	          custom->configuration.host_input_mapping == Entries {"Cross=K", "Circle=J"} &&
	          custom->configuration.user_name == "Global user",
	      "seeded from the Global_Configuration, with input_mapping_custom");
	Check(settings.global == before.global, "the global list is unchanged");
	Check(kyty::core::MappingForTarget(settings, MappingTarget::Game, game) ==
	          Entries {"Cross=K", "Circle=J"},
	      "the game's own list now");
	Check(kyty::core::MappingForTarget(settings, MappingTarget::Game, other) == Entries {"Cross=K"},
	      "another game still uses the global list");

	// Use global mapping: the flag is cleared, the list stays.
	Check(kyty::core::UseGlobalMapping(settings, game), "cleared");
	custom = kyty::core::FindGameSettings(settings, game);
	Check(!custom->configuration.input_mapping_custom &&
	          custom->configuration.host_input_mapping == Entries {"Cross=K", "Circle=J"},
	      "the stored list stays");
	Check(kyty::core::MappingForTarget(settings, MappingTarget::Game, game) == Entries {"Cross=K"},
	      "the game runs with the global list again");
	Check(!kyty::core::UseGlobalMapping(settings, game), "already global");

	// Saving again for a game with custom settings keeps its other fields.
	auto configuration      = custom->configuration;
	configuration.user_name = "Game user";
	Check(kyty::core::SaveCustomSettings(settings, game, configuration).has_value(), "custom");
	Check(kyty::core::SaveMapping(settings, MappingTarget::Game, game,
	                              ParseMapping(Entries {"Pad=Disabled"}))
	          .has_value(),
	      "saved again");
	custom = kyty::core::FindGameSettings(settings, game);
	Check(custom->configuration.user_name == "Game user" &&
	          custom->configuration.input_mapping_custom &&
	          custom->configuration.host_input_mapping == Entries {"Pad=Disabled"},
	      "the other custom fields stay");

	// Global save.
	Check(kyty::core::SaveMapping(settings, MappingTarget::Global, game,
	                              ParseMapping(Entries {"Keyboard=Disabled"}))
	          .has_value(),
	      "saved globally");
	Check(settings.global.host_input_mapping == Entries {"Keyboard=Disabled"}, "the global list");

	// An unknown entry without '=' cannot be saved; nothing changes.
	before        = settings;
	auto invalid  = ParseMapping(Entries {"NoEquals", "Cross=J"});
	auto rejected = kyty::core::SaveMapping(settings, MappingTarget::Global, game, invalid);
	Check(!rejected && rejected.error().field == "host_input_mapping" && settings == before,
	      "a global list with an invalid entry is refused");
	rejected = kyty::core::SaveMapping(settings, MappingTarget::Game, other, invalid);
	Check(!rejected && rejected.error().field == "host_input_mapping" && settings == before,
	      "a game list with an invalid entry is refused");
}

// Kyty.ini is opened with OpenSettingsStore and saved with SaveSettingsStore, as the GTK launcher
// does.
void TestStoreUntouchedUntilSave() {
	const KytyTests::TempDir temp("kyty_input_mapping");
	const auto               ini     = temp.Path() / "Kyty.ini";
	auto                     initial = kyty::core::OpenSettingsStore(ini);
	Check(!initial.load_error, "a new Settings_Store");
	auto settings                      = initial.loaded.settings;
	settings.global.host_input_mapping = {"Cross=K", "Foo=Bar"};
	Check(kyty::core::SaveSettingsStore(initial, settings).has_value(), "the initial store");
	auto store = kyty::core::OpenSettingsStore(ini);
	Check(!store.load_error && store.loaded.settings == settings, "the editor opens the store");
	settings         = store.loaded.settings;
	const auto bytes = kyty::core::ReadFileLimited(ini, kyty::core::kMaxSettingsFileBytes);

	const auto game  = Game("/games/a");
	auto       model = ParseMapping(settings.global.host_input_mapping);
	Check(kyty::core::Assign(model, InputDevice::KeyboardMouse, "Square", "\""), "edit");
	Check(kyty::core::Assign(model, InputDevice::Pad, "Cross", "North"), "edit");
	kyty::core::SetMouseSensitivity(model, 25);
	Check(kyty::core::SaveMapping(settings, MappingTarget::Game, game, model).has_value(), "save");
	const auto unchanged = kyty::core::ReadFileLimited(ini, kyty::core::kMaxSettingsFileBytes);
	Check(bytes && unchanged && *unchanged == *bytes,
	      "no edit and no SaveMapping touches Kyty.ini");

	Check(kyty::core::SaveSettingsStore(store, settings).has_value(),
	      "the caller saves through SaveSettingsStore");
	const auto  reopened = kyty::core::OpenSettingsStore(ini);
	const auto& loaded   = reopened.loaded.settings;
	Check(!reopened.load_error, "loads");
	Check(kyty::core::MappingForTarget(loaded, MappingTarget::Game, game) ==
	          SerializeMapping(model),
	      "the stored mapping is the serialized model: " +
	          Join(kyty::core::MappingForTarget(loaded, MappingTarget::Game, game)));
	Check(ParseMapping(kyty::core::MappingForTarget(loaded, MappingTarget::Game, game)) == model,
	      "and parses back to the model");
	Check(loaded.global.host_input_mapping == Entries {"Cross=K", "Foo=Bar"},
	      "the global list is unchanged");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("InputMappingTests");
	TestTables();
	TestDefaults();
	TestParse();
	TestEdits();
	TestTargets();
	TestStoreUntouchedUntilSave();
	std::printf("InputMappingTests: all tests passed\n");
	return 0;
}
