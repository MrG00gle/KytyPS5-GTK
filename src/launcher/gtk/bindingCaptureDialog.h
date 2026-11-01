#pragma once

// The binding capture dialog of the input mapping editor (task 21.2; plan D41; HC-28; LFI-24),
// modal over the editor, from resources/ui/binding-capture-dialog.xml. It waits for the input that
// one control's binding becomes, by the rules of bindingCapture.h:
// - keyboard and mouse: a key press (ClassifyCapturedKey; an unsupported or reserved key, or a
//   character key with Ctrl, Alt, or Super, shows why it is refused, and the dialog keeps waiting),
//   or a mouse button press in the capture area (MouseButtonBinding);
// - gamepad: the first pad source pressed on the connected controller (ControllerService) that was
//   not already held when the dialog opened (PadCapture). The dialog names the controller, or says
//   that none is connected or that controller input is paused while a game runs. It listens to the
//   service only while it is open.
// Escape and the header bar's close button cancel. done runs once: with the input (then the dialog
// closes), or with nullopt after a cancel; never after the dialog was destroyed otherwise (for
// example with its parent).

#include "launcher/core/inputMapping.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>
#include <functional>
#include <optional>
#include <string>

G_BEGIN_DECLS

#define KYTY_TYPE_BINDING_CAPTURE_DIALOG (kyty_binding_capture_dialog_get_type())
G_DECLARE_FINAL_TYPE(KytyBindingCaptureDialog, kyty_binding_capture_dialog, KYTY,
                     BINDING_CAPTURE_DIALOG, AdwWindow)

G_END_DECLS

namespace kyty::gtk {

using BindingCapturedFn = std::function<void(std::optional<std::string> input)>;

} // namespace kyty::gtk

// A capture for device, titled "Set binding — <binding_title>", transient for and modal over parent
// (may be nullptr). Not presented yet; GTK owns the window. context must outlive it.
KytyBindingCaptureDialog* kyty_binding_capture_dialog_new(kyty::gtk::LauncherContext& context,
                                                          GtkWindow*                  parent,
                                                          kyty::core::InputDevice     device,
                                                          const std::string&          binding_title,
                                                          kyty::gtk::BindingCapturedFn done);

// The dialog's handlers of a key press (its key controller) and of a press of mouse button button
// in the capture area (its click gesture), for the harness and the UI tests, which cannot inject
// input events. handle_key returns TRUE when the dialog used the key.
gboolean kyty_binding_capture_dialog_handle_key(KytyBindingCaptureDialog* dialog, guint keyval,
                                                GdkModifierType state);
void     kyty_binding_capture_dialog_handle_button(KytyBindingCaptureDialog* dialog, guint button);
