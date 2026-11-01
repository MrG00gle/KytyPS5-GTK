#pragma once

// The input mapping editor's texts for what core/inputMapping.h names (task 21.2; plan D46): the
// sections, the controls, the pad sources, and the touch pad particle skins have fixed English
// labels in Launcher_Core, so the editor shows its own translated texts for them, in the
// kyty-launcher domain. Ids the tables do not know are shown as the core has them.

#include "launcher/core/inputMapping.h"

#include <string>
#include <string_view>

namespace kyty::gtk {

// "Face buttons", "D-pad", "Shoulders and triggers", "Stick clicks", "System", "Left stick",
// "Right stick", "In-game shortcuts (cycle)".
std::string InputSectionText(core::InputSection section);

// The control's label on the Gamepad tab (device Pad) or the Keyboard and mouse tab
// (KeyboardMouse); they differ only for the touch pad click ("Touch pad left" on the keyboard).
std::string InputControlText(const core::InputControl& control, core::InputDevice device);

// "<section> / <label>": how the editor names a binding (tooltips, the capture dialog's title,
// accessible labels), as the Qt launcher's accessible names did.
std::string InputControlTitle(const core::InputControl& control, core::InputDevice device);

// The label of a pad source ("A / Cross" for South), or id itself when it is unknown.
std::string PadSourceText(std::string_view id);

// "Classic", "Blue neon", "Monochrome", "Crimson", "Red/Blue".
std::string TouchParticleSkinText(core::TouchParticleSkin skin);

} // namespace kyty::gtk
