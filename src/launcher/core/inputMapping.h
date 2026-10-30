#pragma once

// The input mapping editor's model (task 16.4; Requirement 11.14; design.md, "Game_Configuration
// fields" and LFI-24; plan D24), ported from the Qt launcher's InputMappingDialog
// (src/launcher/src/inputMappingDialog.cpp). The model is what the editor shows and edits; the
// host_input_mapping entries are what the Emulator reads
// (src/graphics/presentation/window/hostInput.cpp, src/graphics/presentation/systemOverlay.cpp):
//   <control>=<key or mouse button>   a keyboard or mouse binding ("Cross=J", "R2=Mouse:Right")
//   Keyboard=Disabled                 custom keyboard bindings, none of them set
//   Pad:<control>=<pad source>        a gamepad binding ("Pad:Cross=South")
//   Pad=Disabled                      custom gamepad bindings, none of them set
//   MouseSensitivity=<0.1-5.0>        the mouse-to-right-stick speed
//   TouchParticleSkin=<skin>          the emulated touch pad's particles
//   TouchCursorImage=Off              hides the emulated touch pad's hand cursor and trail
// Without any keyboard (gamepad) binding entry the Emulator uses its default bindings, which the
// editor shows as its defaults. Entries the model does not understand (an unknown control, a
// gamepad binding of a stick direction or to an unknown source, text without "=") are kept in
// order, after the model's entries, as Requirement 11.14 asks the editor to write only the user's
// binding changes; the Qt editor dropped them.
//
// Nothing here reads or writes the Settings_Store: the editor edits a model, SaveMapping puts it
// into the LauncherSettings when the user saves, and the caller writes Kyty.ini (Requirement
// 11.14: no binding change reaches the Settings_Store before Save).

#include "launcher/core/expected.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/settingsStore.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// The editor's groups, in the order it shows them.
enum class InputSection { Face, Dpad, Shoulders, Sticks, System, LeftStick, RightStick, Shortcuts };

// "Face buttons", "D-pad", "Shoulders & triggers", "Stick clicks", "System", "Left stick",
// "Right stick", "In-game shortcuts (cycle)".
std::string_view InputSectionTitle(InputSection section);

// One PS5 control the Emulator maps (the Qt editor's table, in its order).
struct InputControl {
	std::string_view id;               // the Emulator's control name
	std::string_view label;            // the Gamepad tab's label
	std::string_view keyboard_label;   // the Keyboard and mouse tab's label
	std::string_view keyboard_default; // "" when the control has no default key
	std::string_view pad_default;      // "" when the control has no default pad source
	bool             pad_tab;          // listed on the Gamepad tab
	bool             pad_mappable; // the Emulator accepts "Pad:<id>=..." (not a stick direction)
	InputSection     section;
};

std::span<const InputControl> InputControls();

// The control named id (case-sensitive, as the Emulator), or nullptr.
const InputControl* FindInputControl(std::string_view id);

// A gamepad input the Emulator reads (SDL gamepad buttons and the two triggers).
struct PadSource {
	std::string_view id;    // "South"
	std::string_view label; // "A / Cross"
};

std::span<const PadSource> PadSources();

// The label of a pad source, or id itself when it is unknown (as Qt).
std::string_view PadSourceLabel(std::string_view id);

// The mouse buttons a keyboard binding can name.
inline constexpr std::array<std::string_view, 5> kMouseButtonNames = {
    "Mouse:Left", "Mouse:Right", "Mouse:Middle", "Mouse:X1", "Mouse:X2"};

enum class TouchParticleSkin { Classic, BlueNeon, Monochrome, Crimson, RedBlue };

struct TouchParticleSkinInfo {
	TouchParticleSkin skin;
	std::string_view  key;   // the TouchParticleSkin= value
	std::string_view  label; // "Classic", "Blue neon", "Monochrome", "Crimson", "Red/Blue"
};

std::span<const TouchParticleSkinInfo> TouchParticleSkins();

// Mouse sensitivity in tenths: 0.1-5.0 in steps of 0.1, default 1.0 (the Qt spin box).
inline constexpr int kMinMouseSensitivityTenths     = 1;
inline constexpr int kMaxMouseSensitivityTenths     = 50;
inline constexpr int kDefaultMouseSensitivityTenths = 10;

struct InputMappingModel {
	// False: the Emulator's default keyboard bindings, which keyboard then holds (no entry is
	// written). True after any keyboard edit: keyboard is written as it is.
	bool                               keyboard_custom = false;
	std::map<std::string, std::string> keyboard; // control id -> key name or mouse button name
	bool                               pad_custom = false;
	std::map<std::string, std::string> pad; // control id -> pad source id
	int                                mouse_sensitivity_tenths = kDefaultMouseSensitivityTenths;
	TouchParticleSkin                  particle_skin            = TouchParticleSkin::Classic;
	bool                               touch_cursor             = true;
	std::vector<std::string>           unknown; // entries kept as they are, in order

	friend bool operator==(const InputMappingModel&, const InputMappingModel&) = default;
};

// The model of host_input_mapping entries, read as the Emulator reads them:
// - "Keyboard=Disabled" and "Pad=Disabled" make that section custom;
// - "<control>=<value>" with a known control and a non-empty value binds the control and makes
//   the keyboard custom; "Pad:<control>=<source>" with a pad-mappable control and a known source
//   binds it and makes the gamepad custom. A binding replaces the control's earlier one and
//   removes every other control's binding to the same value (ignoring letter case), so the last
//   entry wins, as in the Emulator and the Qt editor;
// - the last "MouseSensitivity=" entry gives the sensitivity: its leading decimal number (no
//   number counts as 0, a non-finite one as 1.0) clamped to 0.1-5.0 and rounded to a tenth;
// - the first "TouchParticleSkin=" entry naming a known skin gives the skin (Classic otherwise);
//   the touch cursor is hidden when an entry is exactly "TouchCursorImage=Off". Other entries with
//   these two prefixes are dropped, as the Emulator ignores them;
// - every other entry is unknown and kept.
// A section that is not custom gets the default bindings.
InputMappingModel ParseMapping(std::span<const std::string> entries);

// The entries for model, in the Qt editor's order: the keyboard bindings in InputControls() order
// when the keyboard is custom ("Keyboard=Disabled" when none is set), the "Pad:" bindings likewise
// ("Pad=Disabled"), "MouseSensitivity=<n.n>" unless 1.0, "TouchParticleSkin=<key>" unless
// Classic, "TouchCursorImage=Off" when the cursor is hidden, then the unknown entries. Bindings of
// ids that are not controls, and empty values, are not written. ParseMapping(SerializeMapping(m))
// == m for every model that ParseMapping and the edits below produce (Property 37).
std::vector<std::string> SerializeMapping(const InputMappingModel& model);

enum class InputDevice { KeyboardMouse, Pad };

// True for one of kMouseButtonNames.
bool IsMouseButtonName(std::string_view name);

// Binds control to input and makes that section custom (the editor's capture). Refused (false,
// model unchanged) for an unknown control; on the keyboard for an input that is not a mouse button
// name or an IsEmulatorKeyName name, or a reserved key (IsReservedKeyName); on the gamepad for a
// control that is not pad_mappable or a source that is not in PadSources(). Every other control's
// binding to the same input (ignoring letter case) is removed.
bool Assign(InputMappingModel& model, InputDevice device, std::string_view control,
            std::string_view input);

// Removes the control's binding and makes that section custom (right-click). False, model
// unchanged, for an unknown control (on the gamepad: one that is not pad_mappable).
bool Unmap(InputMappingModel& model, InputDevice device, std::string_view control);

enum class InputTab { Gamepad, KeyboardMouse };

// The tab's defaults, as Qt's Restore Defaults: Gamepad resets the gamepad bindings; Keyboard and
// mouse resets the keyboard bindings and the mouse sensitivity. The section is no longer custom.
// The touch pad settings (shared by both tabs) and the unknown entries stay.
void RestoreDefaults(InputMappingModel& model, InputTab tab);

// Sets the mouse sensitivity, clamped to kMinMouseSensitivityTenths-kMaxMouseSensitivityTenths.
void SetMouseSensitivity(InputMappingModel& model, int tenths);

// Which mapping the editor edits (design.md, "Game_Configuration fields"): the global list (opened
// from the header bar) or one game's list (opened from its menu).
enum class MappingTarget { Global, Game };

// The entries the editor opens with: the global list; for a game, its own list while its custom
// settings have input_mapping_custom, else the global list (what the game runs with).
std::vector<std::string> MappingForTarget(const LauncherSettings& settings, MappingTarget target,
                                          const GameEntry& entry);

// The editor's Save, in memory: SerializeMapping(model) becomes settings.global.host_input_mapping
// (Global, after Validate) or the game's host_input_mapping with input_mapping_custom = true
// (Game: custom settings seeded from the Global_Configuration when the game has none, through
// EnableCustomSettings and SaveCustomSettings). A ValidationError (an unknown entry without "=",
// for example) leaves settings unchanged; the caller can list FindInvalidMappingEntries. entry is
// not used for Global. The caller then saves the Settings_Store (SaveSettingsStore).
Expected<void, ValidationError> SaveMapping(LauncherSettings& settings, MappingTarget target,
                                            const GameEntry& entry, const InputMappingModel& model);

// "Use global mapping" in the per-game editor: clears input_mapping_custom of the game's custom
// settings (its own list stays stored). False when the game has no custom settings or already uses
// the global mapping.
bool UseGlobalMapping(LauncherSettings& settings, const GameEntry& entry);

} // namespace kyty::core
