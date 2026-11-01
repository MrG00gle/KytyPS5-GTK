#pragma once

// The DualSense diagram of the input mapping editor (task 21.2; plan D41; LFI-24), ported from the
// Qt launcher's DualSenseWidget (src/launcher/src/dualsenseWidget.cpp) and drawn with GtkSnapshot
// and GskPath: the controller's shell with every control the Emulator maps, by the diagram names
// of ControllerState::pressed ("Cross", "L1", "Up", "L3", "LeftStickUp", "TouchPad", ...).
// Pressed controls are filled, the selected one (whose binding is being captured) and the one
// under the pointer are outlined; the stick knobs follow the stick positions; stick directions and
// the touch pad halves are drawn only while active. The drawing keeps the Qt launcher's fixed
// colors in both color schemes (a picture of the controller). Clicking a control emits
// "control-clicked" with its name; the tooltip names the control under the pointer.

#include <gtk/gtk.h>
#include <set>
#include <string>

G_BEGIN_DECLS

#define KYTY_TYPE_DUALSENSE_WIDGET (kyty_dualsense_widget_get_type())
G_DECLARE_FINAL_TYPE(KytyDualsenseWidget, kyty_dualsense_widget, KYTY, DUALSENSE_WIDGET, GtkWidget)

G_END_DECLS

// A new diagram (minimum 330 x 230, natural 480 x 330 pixels; it scales to its allocation).
GtkWidget* kyty_dualsense_widget_new();

// The controls shown pressed.
void kyty_dualsense_widget_set_pressed(KytyDualsenseWidget*         widget,
                                       const std::set<std::string>& pressed);
const std::set<std::string>& kyty_dualsense_widget_get_pressed(KytyDualsenseWidget* widget);

// The control whose binding is being captured ("" for none).
void kyty_dualsense_widget_set_selected(KytyDualsenseWidget* widget, const std::string& control);

// The stick positions, each axis from -1 to 1 (ControllerState::left_x ... right_y).
void kyty_dualsense_widget_set_sticks(KytyDualsenseWidget* widget, float left_x, float left_y,
                                      float right_x, float right_y);

// The control at (x, y) in widget coordinates, or "" (what a click selects).
std::string kyty_dualsense_widget_hit_test(KytyDualsenseWidget* widget, double x, double y);
