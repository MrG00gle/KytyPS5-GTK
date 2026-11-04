#pragma once

// The input mapping editor (task 21.2; plan D41; HC-28; Requirement 11.14; LFI-24), modal for its
// parent, from resources/ui/input-mapping-window.xml, ported from the Qt launcher's
// InputMappingDialog (src/launcher/src/inputMappingDialog.cpp). It edits a
// core::InputMappingModel: ParseMapping(MappingForTarget(settings, target, entry)), the global
// mapping (from the header bar) or one game's (from its menu; a game without its own mapping
// starts with the global one).
// - Gamepad tab: the controls the gamepad can trigger in sections, each with the pad source bound
//   to it; the DualSense diagram (dualsenseWidget.h), which lights the controls pressed on the
//   connected controller and opens the capture for a clicked control; the connected-controller
//   status. The editor listens to ControllerService while it is open.
// - Keyboard and mouse tab: every control with its key or mouse button, and the mouse
//   sensitivity (0.1-5.0).
// - Below both: the emulated touch pad's particle skin and hand cursor.
// A click on a binding, or Enter while it has the focus, opens the binding capture dialog
// (bindingCaptureDialog.h; core::Assign); Delete while the binding's row or its button has the
// focus unmaps it (core::Unmap; a shortcut of the row, phase 7 D19).
// Restore tab defaults resets the shown tab (core::RestoreDefaults). Save puts the model into the
// settings (core::SaveMapping) and writes the Settings_Store (settingsWrite.h): an entry that is
// not Name=Value refuses the save, and the editor lists the invalid entries of
// SerializeMapping(model), numbered from 1, and offers to remove them. For a game, "Use global
// mapping" (shown only then; enabled while the game has its own mapping) makes the game use the
// global mapping again (core::UseGlobalMapping) and writes the Settings_Store; while the game has
// no custom settings, the header says "Saving gives this game its own settings." (a save seeds
// them from the Global_Configuration, phase 7 D29). Section titles are markup-escaped. Cancel,
// Escape, and closing write nothing (11.14): no edit reaches the Settings_Store before Save.
//
// Signal "saved" (no arguments): emitted after the editor wrote the Settings_Store (Save, Use
// global mapping), before it closes.

#include "launcher/core/gameMetadata.h"
#include "launcher/core/inputMapping.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_INPUT_MAPPING_WINDOW (kyty_input_mapping_window_get_type())
G_DECLARE_FINAL_TYPE(KytyInputMappingWindow, kyty_input_mapping_window, KYTY, INPUT_MAPPING_WINDOW,
                     AdwWindow)

G_END_DECLS

// The editor of the global mapping, transient for and modal over parent (may be nullptr). Not
// presented yet; GTK owns the window. context must outlive it.
KytyInputMappingWindow* kyty_input_mapping_window_new_global(kyty::gtk::LauncherContext& context,
                                                             GtkWindow*                  parent);

// The editor of entry's mapping (entry is copied).
KytyInputMappingWindow* kyty_input_mapping_window_new_for_game(kyty::gtk::LauncherContext&  context,
                                                               GtkWindow*                   parent,
                                                               const kyty::core::GameEntry& entry);

// The model as edited so far (for the harness and the UI tests).
const kyty::core::InputMappingModel&
kyty_input_mapping_window_get_model(KytyInputMappingWindow* window);

// Shows the tab named name: "gamepad" or "keyboard".
void kyty_input_mapping_window_show_tab(KytyInputMappingWindow* window, const char* name);
