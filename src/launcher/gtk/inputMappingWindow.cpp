#include "launcher/gtk/inputMappingWindow.h"

#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/bindingCaptureDialog.h"
#include "launcher/gtk/dualsenseWidget.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/inputLabels.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/settingsWrite.h"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kyty::gtk {

// One binding row: a control on one tab.
struct InputBinding {
	const core::InputControl* control = nullptr;
	core::InputDevice         device  = core::InputDevice::KeyboardMouse;
	GtkWidget*                row     = nullptr;
	GtkWidget*                button  = nullptr;
};

struct InputMappingWindowState {
	LauncherContext*          context = nullptr;
	core::MappingTarget       target  = core::MappingTarget::Global;
	core::GameEntry           entry; // the edited game (MappingTarget::Game)
	core::InputMappingModel   model;
	std::vector<InputBinding> bindings;
	KytyDualsenseWidget*      diagram = nullptr;
	// The controls pressed on the connected controller, as the diagram and the Gamepad tab show
	// them.
	std::set<std::string> lit;
	// The binding whose capture dialog is open.
	std::optional<std::size_t>    capturing;
	ControllerService::ListenerId listener = 0;

	bool loading  = false; // widgets are being set from the model
	bool disposed = false;
	// Cancelled when the window goes away: no alert callback runs after it.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyInputMappingWindow {
	AdwWindow       parent_instance;
	AdwViewStack*   stack;
	GtkWidget*      game_info;
	GtkLabel*       game_title_label;
	GtkLabel*       game_note_label;
	GtkWidget*      game_settings_note_label;
	GtkLabel*       controller_status;
	GtkBox*         pad_columns;
	GtkBox*         diagram_box;
	GtkBox*         keyboard_columns;
	AdwSpinRow*     sensitivity_row;
	GtkDropDown*    skin_dropdown;
	GtkCheckButton* cursor_check;
	GtkButton*      defaults_button;
	GtkButton*      global_button;
	GtkButton*      save_button;

	kyty::gtk::InputMappingWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyInputMappingWindow, kyty_input_mapping_window, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::InputBinding;
using kyty::gtk::LauncherContext;
using kyty::gtk::SignalScope;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/input-mapping-window.xml";

// The index of a binding button's InputBinding (g_object_set_data on the button).
constexpr const char* kBindingIndexKey = "kyty-binding-index";

// The width of a binding button (the Qt launcher's).
constexpr int kBindingButtonWidth = 190;

constexpr core::InputSection kSections[] = {
    core::InputSection::Face,       core::InputSection::Dpad,      core::InputSection::Shoulders,
    core::InputSection::Sticks,     core::InputSection::System,    core::InputSection::LeftStick,
    core::InputSection::RightStick, core::InputSection::Shortcuts,
};

guint g_saved_signal = 0;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyInputMappingWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

bool HasOwnMapping(const core::LauncherSettings& settings, const core::GameEntry& entry) {
	const auto* game = core::FindGameSettings(settings, entry);
	return game != nullptr && game->configuration.input_mapping_custom;
}

// ---- Showing the model

// What a binding button shows: the key or mouse button name (the Emulator's names, as stored), the
// pad source's label, or "None".
std::string BindingText(const kyty::gtk::InputMappingWindowState& state,
                        const InputBinding&                       binding) {
	const bool  pad      = binding.device == core::InputDevice::Pad;
	const auto& bindings = pad ? state.model.pad : state.model.keyboard;
	const auto  found    = bindings.find(std::string(binding.control->id));
	if (found == bindings.end() || found->second.empty()) {
		// TRANSLATORS: a control without a binding.
		return C_("binding", "None");
	}
	return pad ? kyty::gtk::PadSourceText(found->second) : found->second;
}

void SetClass(GtkWidget* widget, const char* name, bool on) {
	if (on) {
		gtk_widget_add_css_class(widget, name);
	} else {
		gtk_widget_remove_css_class(widget, name);
	}
}

void UpdateBindings(KytyInputMappingWindow* self) {
	auto& state = *self->state;
	for (std::size_t i = 0; i < state.bindings.size(); i++) {
		const auto&       binding = state.bindings[i];
		const std::string text    = BindingText(state, binding);
		const std::string title   = kyty::gtk::InputControlTitle(*binding.control, binding.device);
		gtk_button_set_label(GTK_BUTTON(binding.button), text.c_str());
		gtk_widget_set_tooltip_text(
		    binding.button,
		    // TRANSLATORS: a binding's tooltip: "<binding name>: <input>. Click ...".
		    Format(_("%s: %s. Click to change, press Delete to unmap."), title.c_str(),
		           text.c_str())
		        .c_str());
		gtk_accessible_update_property(
		    GTK_ACCESSIBLE(binding.button), GTK_ACCESSIBLE_PROPERTY_LABEL,
		    // TRANSLATORS: a binding's accessible name: "<binding name>: <input>".
		    Format(C_("binding", "%s: %s"), title.c_str(), text.c_str()).c_str(), -1);
		const bool pad = binding.device == core::InputDevice::Pad;
		SetClass(binding.button, "binding-pressed",
		         pad && state.lit.count(std::string(binding.control->id)) != 0);
		SetClass(binding.button, "binding-selected", state.capturing == i);
	}
}

// The live controller state (the Qt launcher's UpdatePreview): a control lights up while the pad
// source bound to it is pressed (the touch pad click lights both halves); stick directions and
// touch pad fingers light up as the controller reports them.
void UpdatePreview(KytyInputMappingWindow* self) {
	auto&       state      = *self->state;
	const auto& controller = state.context->services.controller.State();
	state.lit.clear();
	for (const auto& [control, source]: state.model.pad) {
		if (controller.raw_pressed.count(source) != 0) {
			state.lit.insert(control);
		}
	}
	if (state.lit.count("TouchPad") != 0) {
		state.lit.insert("TouchPadRight");
	}
	for (const auto& name: controller.pressed) {
		const std::string_view view(name);
		if (view.starts_with("LeftStick") || view.starts_with("RightStick") ||
		    view.starts_with("TouchPad")) {
			state.lit.insert(name);
		}
	}
	kyty_dualsense_widget_set_pressed(state.diagram, state.lit);
	kyty_dualsense_widget_set_sticks(state.diagram, controller.left_x, controller.left_y,
	                                 controller.right_x, controller.right_y);
	kyty_dualsense_widget_set_selected(
	    state.diagram, state.capturing ? std::string(state.bindings[*state.capturing].control->id)
	                                   : std::string());

	std::string status;
	if (controller.paused) {
		status = _("Preview is paused while a game is running.");
	} else if (!controller.error.empty()) {
		// TRANSLATORS: %s is SDL's message.
		status = Format(_("Controller preview unavailable: %s"), controller.error.c_str());
	} else if (!controller.device_name.empty()) {
		// TRANSLATORS: %s is the name of the connected controller.
		status = Format(_("Connected: %s"), controller.device_name.c_str());
	} else {
		status = _("No controller connected");
	}
	gtk_label_set_text(self->controller_status, status.c_str());
	UpdateBindings(self);
}

// The model's sensitivity, skin, and cursor into their widgets, then every binding.
void LoadModel(KytyInputMappingWindow* self) {
	auto& state   = *self->state;
	state.loading = true;
	adw_spin_row_set_value(self->sensitivity_row,
	                       static_cast<double>(state.model.mouse_sensitivity_tenths) / 10.0);
	const auto skins = core::TouchParticleSkins();
	for (std::size_t i = 0; i < skins.size(); i++) {
		if (skins[i].skin == state.model.particle_skin) {
			gtk_drop_down_set_selected(self->skin_dropdown, static_cast<guint>(i));
		}
	}
	gtk_check_button_set_active(self->cursor_check, state.model.touch_cursor ? TRUE : FALSE);
	state.loading = false;
	UpdatePreview(self);
}

// ---- Editing

void OnControllerChanged(KytyInputMappingWindow* self) {
	if (Alive(self) && self->state->diagram != nullptr) {
		UpdatePreview(self);
	}
}

void OnCaptured(KytyInputMappingWindow* self, std::size_t index,
                const std::optional<std::string>& input) {
	if (!Alive(self)) {
		return;
	}
	auto& state = *self->state;
	state.capturing.reset();
	const InputBinding& binding = state.bindings[index];
	if (input && !core::Assign(state.model, binding.device, binding.control->id, *input)) {
		g_message("input mapping: %s=%s was refused", std::string(binding.control->id).c_str(),
		          input->c_str());
	}
	UpdatePreview(self);
	gtk_widget_grab_focus(binding.button);
}

void StartCapture(KytyInputMappingWindow* self, std::size_t index) {
	auto& state = *self->state;
	if (state.capturing || index >= state.bindings.size()) {
		return;
	}
	state.capturing             = index;
	const InputBinding& binding = state.bindings[index];
	UpdatePreview(self);
	KytyBindingCaptureDialog* dialog = kyty_binding_capture_dialog_new(
	    *state.context, GTK_WINDOW(self), binding.device,
	    kyty::gtk::InputControlTitle(*binding.control, binding.device),
	    [self, index](std::optional<std::string> input) { OnCaptured(self, index, input); });
	gtk_window_present(GTK_WINDOW(dialog));
}

std::optional<std::size_t> BindingIndex(GtkWidget* button) {
	if (button == nullptr) {
		return std::nullopt;
	}
	const gpointer data = g_object_get_data(G_OBJECT(button), kBindingIndexKey);
	if (data == nullptr) {
		return std::nullopt;
	}
	return GPOINTER_TO_SIZE(data) - 1;
}

void OnBindingClicked(GtkButton* button, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (const auto index = BindingIndex(GTK_WIDGET(button)); Alive(self) && index) {
		StartCapture(self, *index);
	}
}

// Delete on a binding unmaps it (LFI-24: replaces the Qt launcher's right-click). The shortcut is
// the row's (phase 7 D19), so it works whether the row or its button has the focus; the button
// keeps Enter and Space for the capture.
gboolean OnBindingDelete(GtkWidget* row, GVariant* /*arguments*/, gpointer data) {
	auto*      self  = KYTY_INPUT_MAPPING_WINDOW(data);
	const auto index = BindingIndex(row);
	if (!Alive(self) || !index) {
		return FALSE;
	}
	const InputBinding& binding = self->state->bindings[*index];
	core::Unmap(self->state->model, binding.device, binding.control->id);
	UpdatePreview(self);
	return TRUE;
}

// A click on the diagram: the capture of that control's gamepad binding (both touch pad halves are
// the touch pad click; stick directions have no gamepad binding).
void OnDiagramClicked(KytyDualsenseWidget* /*diagram*/, const char* control, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (!Alive(self) || control == nullptr) {
		return;
	}
	const std::string_view id = std::string_view(control) == "TouchPadRight" ? "TouchPad" : control;
	const auto&            bindings = self->state->bindings;
	for (std::size_t i = 0; i < bindings.size(); i++) {
		if (bindings[i].device == core::InputDevice::Pad && bindings[i].control->id == id) {
			StartCapture(self, i);
			return;
		}
	}
}

void OnSensitivityChanged(GObject* /*row*/, GParamSpec* /*pspec*/, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (Alive(self) && !self->state->loading) {
		core::SetMouseSensitivity(
		    self->state->model,
		    static_cast<int>(std::lround(adw_spin_row_get_value(self->sensitivity_row) * 10.0)));
	}
}

void OnSkinChanged(GObject* /*dropdown*/, GParamSpec* /*pspec*/, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (!Alive(self) || self->state->loading) {
		return;
	}
	const auto  skins    = core::TouchParticleSkins();
	const guint selected = gtk_drop_down_get_selected(self->skin_dropdown);
	if (selected < skins.size()) {
		self->state->model.particle_skin = skins[selected].skin;
	}
}

void OnCursorToggled(GtkCheckButton* button, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (Alive(self) && !self->state->loading) {
		self->state->model.touch_cursor = gtk_check_button_get_active(button) != FALSE;
	}
}

// Restore tab defaults: the shown tab's bindings (and on the Keyboard and mouse tab the mouse
// sensitivity), as the Qt launcher's Restore Defaults; nothing is written until Save.
void OnRestoreDefaults(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	const char* tab = adw_view_stack_get_visible_child_name(self->stack);
	core::RestoreDefaults(self->state->model, g_strcmp0(tab, "gamepad") == 0
	                                              ? core::InputTab::Gamepad
	                                              : core::InputTab::KeyboardMouse);
	LoadModel(self);
}

// ---- Saving

void Save(KytyInputMappingWindow* self);

void Write(KytyInputMappingWindow* self, core::LauncherSettings next) {
	kyty::gtk::SaveSettingsInteractively(GTK_WINDOW(self), *self->state->context, std::move(next),
	                                     self->state->cancellable.get(), [self]() {
		                                     g_signal_emit(self, g_saved_signal, 0);
		                                     gtk_window_close(GTK_WINDOW(self));
	                                     });
}

// HC-28: the serialized model has entries that are not Name=Value (unknown entries the model
// keeps); they are listed, numbered from 1, and removed from the model only when the user agrees.
void OfferDroppingInvalidEntries(KytyInputMappingWindow* self) {
	auto&       state   = *self->state;
	const auto  invalid = core::FindInvalidMappingEntries(core::SerializeMapping(state.model));
	std::string body =
	    state.target == core::MappingTarget::Game
	        ? _("The input mapping of this game has entries that are not of the form Name=Value:")
	        : _("The global input mapping has entries that are not of the form Name=Value:");
	for (const auto& entry: invalid) {
		body += "\n";
		// TRANSLATORS: an invalid input mapping entry: its number (from 1) and its text.
		body += Format(_("Entry %u: “%s”"), static_cast<unsigned>(entry.index + 1),
		               entry.entry.c_str());
	}
	body += "\n\n";
	body += _("The emulator ignores these entries. Remove them and save?");
	kyty::gtk::Confirm(
	    GTK_WINDOW(self), _("Invalid input mapping entries"), body, _("Remove and save"),
	    _("Cancel"), true,
	    [self](bool accepted) {
		    if (!accepted || !Alive(self)) {
			    return;
		    }
		    auto& model   = self->state->model;
		    model.unknown = core::DropInvalidMappingEntries(model.unknown);
		    Save(self);
	    },
	    state.cancellable.get());
}

// Save (11.14): the model into a copy of the settings (core::SaveMapping), then the
// Settings_Store; the window closes after the write.
void Save(KytyInputMappingWindow* self) {
	auto&                  state = *self->state;
	core::LauncherSettings next  = state.context->settings.loaded.settings;
	if (auto saved = core::SaveMapping(next, state.target, state.entry, state.model); !saved) {
		if (saved.error().field == "host_input_mapping") {
			OfferDroppingInvalidEntries(self);
			return;
		}
		g_message("input mapping: cannot save: %s: %s", saved.error().field.c_str(),
		          saved.error().message.c_str());
		kyty::gtk::ShowError(GTK_WINDOW(self), _("The input mapping could not be saved"),
		                     Format(_("The setting %s is not valid. Correct it in the settings "
		                              "editor, then save the input mapping again."),
		                            saved.error().field.c_str()),
		                     state.cancellable.get());
		return;
	}
	Write(self, std::move(next));
}

void OnSave(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (Alive(self)) {
		Save(self);
	}
}

// Use global mapping (game editor): the game runs with the global mapping again; its own list
// stays stored.
void OnUseGlobalMapping(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	core::LauncherSettings next = self->state->context->settings.loaded.settings;
	if (core::UseGlobalMapping(next, self->state->entry)) {
		Write(self, std::move(next));
	}
}

// ---- Construction

// The sections of device's tab in columns vertical boxes, each section in the column with the
// fewest rows so far; the Gamepad tab lists only the controls a gamepad can trigger.
void BuildTab(KytyInputMappingWindow* self, core::InputDevice device, GtkBox* parent,
              std::size_t columns) {
	auto&                    state = *self->state;
	const bool               pad   = device == core::InputDevice::Pad;
	std::vector<GtkWidget*>  boxes;
	std::vector<std::size_t> rows(columns, 0);
	for (std::size_t i = 0; i < columns; i++) {
		GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
		gtk_box_append(parent, box);
		boxes.push_back(box);
	}
	for (const auto section: kSections) {
		GtkWidget*  group = nullptr;
		std::size_t count = 0;
		for (const auto& control: core::InputControls()) {
			if (control.section != section || (pad && !control.pad_tab)) {
				continue;
			}
			if (group == nullptr) {
				group = adw_preferences_group_new();
				// The group's title is markup, so a translated "&" or "<" is escaped (p6-u8-2).
				const GCharPtr title(
				    g_markup_escape_text(kyty::gtk::InputSectionText(section).c_str(), -1));
				adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title.get());
			}
			GtkWidget* row = adw_action_row_new();
			adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
			adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
			                              kyty::gtk::InputControlText(control, device).c_str());
			GtkWidget* button = gtk_button_new();
			gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
			gtk_widget_set_size_request(button, kBindingButtonWidth, -1);
			gtk_widget_add_css_class(button, "binding");
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
			adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), button);
			adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
			// Stored plus one, so that no index is a null pointer.
			g_object_set_data(G_OBJECT(button), kBindingIndexKey,
			                  GSIZE_TO_POINTER(state.bindings.size() + 1));
			g_object_set_data(G_OBJECT(row), kBindingIndexKey,
			                  GSIZE_TO_POINTER(state.bindings.size() + 1));
			state.signals.push_back(
			    SignalScope::Connect(button, "clicked", G_CALLBACK(&OnBindingClicked), self));
			// Delete on the row or on its button (the row is the button's ancestor).
			GtkEventController* keys = gtk_shortcut_controller_new();
			gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys),
			                                  GTK_SHORTCUT_SCOPE_LOCAL);
			gtk_shortcut_controller_add_shortcut(
			    GTK_SHORTCUT_CONTROLLER(keys),
			    gtk_shortcut_new(gtk_alternative_trigger_new(
			                         gtk_keyval_trigger_new(GDK_KEY_Delete, GdkModifierType(0)),
			                         gtk_keyval_trigger_new(GDK_KEY_KP_Delete, GdkModifierType(0))),
			                     gtk_callback_action_new(&OnBindingDelete, self, nullptr)));
			gtk_widget_add_controller(row, keys);
			state.bindings.push_back({&control, device, row, button});
			count++;
		}
		if (group != nullptr) {
			std::size_t target = 0;
			for (std::size_t i = 1; i < columns; i++) {
				if (rows[i] < rows[target]) {
					target = i;
				}
			}
			gtk_box_append(GTK_BOX(boxes[target]), group);
			rows[target] += count;
		}
	}
}

KytyInputMappingWindow* NewWindow(LauncherContext& context, GtkWindow* parent,
                                  const core::GameEntry* entry) {
	auto* self  = KYTY_INPUT_MAPPING_WINDOW(g_object_new(KYTY_TYPE_INPUT_MAPPING_WINDOW, nullptr));
	auto& state = *self->state;
	state.context = &context;
	state.target  = entry != nullptr ? core::MappingTarget::Game : core::MappingTarget::Global;
	if (entry != nullptr) {
		state.entry = *entry;
	}
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	const core::LauncherSettings& settings = context.settings.loaded.settings;
	state.model = core::ParseMapping(core::MappingForTarget(settings, state.target, state.entry));

	BuildTab(self, core::InputDevice::Pad, self->pad_columns, 1);
	BuildTab(self, core::InputDevice::KeyboardMouse, self->keyboard_columns, 2);
	GtkWidget* diagram = kyty_dualsense_widget_new();
	gtk_box_append(self->diagram_box, diagram);
	state.diagram = KYTY_DUALSENSE_WIDGET(diagram);
	state.signals.push_back(
	    SignalScope::Connect(diagram, "control-clicked", G_CALLBACK(&OnDiagramClicked), self));

	auto skins = GObjectPtr<GtkStringList>::Adopt(gtk_string_list_new(nullptr));
	for (const auto& skin: core::TouchParticleSkins()) {
		gtk_string_list_append(skins.get(), kyty::gtk::TouchParticleSkinText(skin.skin).c_str());
	}
	gtk_drop_down_set_model(self->skin_dropdown, G_LIST_MODEL(skins.get()));

	if (entry != nullptr) {
		gtk_window_set_title(GTK_WINDOW(self), _("Game input mapping"));
		const std::string title_id = core::NormalizeTitleId(entry->title_id);
		const std::string title    = entry->title_name.empty() ? title_id : entry->title_name;
		gtk_label_set_text(self->game_title_label,
		                   title_id.empty() || title == title_id
		                       ? title.c_str()
		                       : Format(_("%s (%s)"), title.c_str(), title_id.c_str()).c_str());
		const bool own = HasOwnMapping(settings, *entry);
		gtk_label_set_text(self->game_note_label,
		                   own ? _("This game has its own input mapping.")
		                       : _("This game uses the global input mapping. Saving gives it its "
		                           "own mapping."));
		// Save creates the game's custom settings from the global ones when it has none
		// (core::SaveMapping, p6-u8-3), so the editor says so.
		gtk_widget_set_visible(self->game_settings_note_label,
		                       core::HasCustomSettings(settings, *entry) ? FALSE : TRUE);
		gtk_widget_set_visible(self->game_info, TRUE);
		gtk_widget_set_visible(GTK_WIDGET(self->global_button), TRUE);
		gtk_widget_set_sensitive(GTK_WIDGET(self->global_button), own ? TRUE : FALSE);
		gtk_widget_set_tooltip_text(
		    GTK_WIDGET(self->global_button),
		    own ? _("Run this game with the global input mapping again; its own mapping stays "
		            "stored")
		        : _("This game already uses the global input mapping"));
	} else {
		gtk_window_set_title(GTK_WINDOW(self), _("Input mapping"));
	}

	state.signals.push_back(
	    SignalScope::Connect(self->save_button, "clicked", G_CALLBACK(&OnSave), self));
	state.signals.push_back(SignalScope::Connect(self->defaults_button, "clicked",
	                                             G_CALLBACK(&OnRestoreDefaults), self));
	state.signals.push_back(SignalScope::Connect(self->global_button, "clicked",
	                                             G_CALLBACK(&OnUseGlobalMapping), self));
	state.signals.push_back(SignalScope::Connect(self->sensitivity_row, "notify::value",
	                                             G_CALLBACK(&OnSensitivityChanged), self));
	state.signals.push_back(SignalScope::Connect(self->skin_dropdown, "notify::selected",
	                                             G_CALLBACK(&OnSkinChanged), self));
	state.signals.push_back(
	    SignalScope::Connect(self->cursor_check, "toggled", G_CALLBACK(&OnCursorToggled), self));

	LoadModel(self);
	// The live state while the editor is open (16 ms polls); the service may call the listener
	// before AddListener returns.
	state.listener =
	    context.services.controller.AddListener([self]() { OnControllerChanged(self); });
	return self;
}

} // namespace

static void kyty_input_mapping_window_init(KytyInputMappingWindow* self) {
	self->state = new kyty::gtk::InputMappingWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_input_mapping_window_dispose(GObject* object) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		g_cancellable_cancel(state->cancellable.get());
		state->signals.clear();
		if (state->listener != 0 && state->context != nullptr) {
			state->context->services.controller.RemoveListener(state->listener);
			state->listener = 0;
		}
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_INPUT_MAPPING_WINDOW);
	G_OBJECT_CLASS(kyty_input_mapping_window_parent_class)->dispose(object);
}

static void kyty_input_mapping_window_finalize(GObject* object) {
	auto* self = KYTY_INPUT_MAPPING_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_input_mapping_window_parent_class)->finalize(object);
}

static void kyty_input_mapping_window_class_init(KytyInputMappingWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_input_mapping_window_dispose;
	object_class->finalize = kyty_input_mapping_window_finalize;
	g_saved_signal = g_signal_new("saved", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
	                              nullptr, nullptr, G_TYPE_NONE, 0);
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, stack);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, game_info);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, game_title_label);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, game_note_label);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow,
	                                     game_settings_note_label);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, controller_status);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, pad_columns);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, diagram_box);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, keyboard_columns);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, sensitivity_row);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, skin_dropdown);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, cursor_check);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, defaults_button);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, global_button);
	gtk_widget_class_bind_template_child(widget_class, KytyInputMappingWindow, save_button);
	// Escape is Cancel (closing writes nothing, 11.14).
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyInputMappingWindow* kyty_input_mapping_window_new_global(kyty::gtk::LauncherContext& context,
                                                             GtkWindow*                  parent) {
	return NewWindow(context, parent, nullptr);
}

KytyInputMappingWindow* kyty_input_mapping_window_new_for_game(kyty::gtk::LauncherContext&  context,
                                                               GtkWindow*                   parent,
                                                               const kyty::core::GameEntry& entry) {
	return NewWindow(context, parent, &entry);
}

const kyty::core::InputMappingModel&
kyty_input_mapping_window_get_model(KytyInputMappingWindow* window) {
	static const kyty::core::InputMappingModel kNone;
	g_return_val_if_fail(KYTY_IS_INPUT_MAPPING_WINDOW(window) && window->state != nullptr, kNone);
	return window->state->model;
}

void kyty_input_mapping_window_show_tab(KytyInputMappingWindow* window, const char* name) {
	g_return_if_fail(KYTY_IS_INPUT_MAPPING_WINDOW(window));
	adw_view_stack_set_visible_child_name(window->stack, name);
}
