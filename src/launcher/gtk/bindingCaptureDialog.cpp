#include "launcher/gtk/bindingCaptureDialog.h"

#include "launcher/gtk/bindingCapture.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/services/controllerService.h"

#include <cstdarg>
#include <utility>

namespace kyty::gtk {

struct BindingCaptureState {
	LauncherContext*              context = nullptr;
	core::InputDevice             device  = core::InputDevice::KeyboardMouse;
	BindingCapturedFn             done;
	PadCapture                    pad;
	ControllerService::ListenerId listener = 0;
	bool                          ready    = false; // the pad capture knows what was held
	bool                          finished = false; // done ran or will not run
};

} // namespace kyty::gtk

struct _KytyBindingCaptureDialog {
	AdwWindow  parent_instance;
	GtkWidget* capture_area;
	GtkImage*  device_image;
	GtkLabel*  prompt_label;
	GtkLabel*  refusal_label;

	kyty::gtk::BindingCaptureState* state;
};

G_DEFINE_FINAL_TYPE(KytyBindingCaptureDialog, kyty_binding_capture_dialog, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::BindingCaptureState;
using kyty::gtk::KeyCapture;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/binding-capture-dialog.xml";

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const kyty::gtk::GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

void RemoveListener(BindingCaptureState& state) {
	if (state.listener != 0 && state.context != nullptr) {
		state.context->services.controller.RemoveListener(state.listener);
	}
	state.listener = 0;
}

// Ends the capture: done runs with input (nullopt: cancelled); with destroy the dialog closes.
void Finish(KytyBindingCaptureDialog* self, std::optional<std::string> input, bool destroy) {
	auto& state = *self->state;
	if (state.finished) {
		return;
	}
	state.finished = true;
	RemoveListener(state);
	auto done = std::move(state.done);
	if (done) {
		done(std::move(input));
	}
	if (destroy) {
		gtk_window_destroy(GTK_WINDOW(self));
	}
}

void ShowRefusal(KytyBindingCaptureDialog* self, const char* text) {
	gtk_label_set_text(self->refusal_label, text);
	gtk_widget_set_visible(GTK_WIDGET(self->refusal_label), TRUE);
}

void UpdatePadText(KytyBindingCaptureDialog* self) {
	const auto&        controller = self->state->context->services.controller.State();
	const std::string& sdl_error  = controller.error; // core text: SDL's message (library text)
	std::string        text;
	if (controller.paused) {
		text = _("Controller input is paused while a game is running.\nPress Esc to cancel.");
	} else if (!sdl_error.empty()) {
		text = Format(
		    // TRANSLATORS: %s is SDL's message.
		    _("The controller cannot be read: %s\nPress Esc to cancel."), sdl_error.c_str());
	} else if (controller.device_name.empty()) {
		text = _("No controller connected.\nConnect one, or press Esc to cancel.");
	} else {
		// TRANSLATORS: %s is the name of the connected controller.
		text = Format(_("Press a button or trigger on %s.\nEsc cancels."),
		              controller.device_name.c_str());
	}
	gtk_label_set_text(self->prompt_label, text.c_str());
}

void OnControllerChanged(KytyBindingCaptureDialog* self) {
	auto& state = *self->state;
	if (!state.ready || state.finished) {
		return;
	}
	if (auto source = state.pad.Update(state.context->services.controller.State().raw_pressed)) {
		Finish(self, std::move(source), true);
		return;
	}
	UpdatePadText(self);
}

gboolean OnKeyPressed(GtkEventControllerKey* /*controller*/, guint keyval, guint /*keycode*/,
                      GdkModifierType state, gpointer data) {
	return kyty_binding_capture_dialog_handle_key(KYTY_BINDING_CAPTURE_DIALOG(data), keyval, state);
}

void OnButtonPressed(GtkGestureClick* gesture, int /*presses*/, double /*x*/, double /*y*/,
                     gpointer         data) {
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	kyty_binding_capture_dialog_handle_button(
	    KYTY_BINDING_CAPTURE_DIALOG(data),
	    gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)));
}

// The header bar's close button (and closing from outside): a cancel.
gboolean OnCloseRequest(GtkWindow* window, gpointer /*data*/) {
	Finish(KYTY_BINDING_CAPTURE_DIALOG(window), std::nullopt, false);
	return FALSE;
}

} // namespace

static void kyty_binding_capture_dialog_init(KytyBindingCaptureDialog* self) {
	self->state = new BindingCaptureState;
	gtk_widget_init_template(GTK_WIDGET(self));

	// Keys go to the dialog before any of its widgets (the close button) sees them.
	GtkEventController* keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(&OnKeyPressed), self);
	gtk_widget_add_controller(GTK_WIDGET(self), keys);

	// Every mouse button in the capture area (below the header bar).
	GtkGesture* click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	g_signal_connect(click, "pressed", G_CALLBACK(&OnButtonPressed), self);
	gtk_widget_add_controller(self->capture_area, GTK_EVENT_CONTROLLER(click));

	g_signal_connect(self, "close-request", G_CALLBACK(&OnCloseRequest), nullptr);
}

static void kyty_binding_capture_dialog_dispose(GObject* object) {
	auto* self = KYTY_BINDING_CAPTURE_DIALOG(object);
	if (auto* state = self->state; state != nullptr) {
		// Destroyed without a result (with its parent, for example): done never runs.
		state->finished = true;
		state->done     = nullptr;
		RemoveListener(*state);
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_BINDING_CAPTURE_DIALOG);
	G_OBJECT_CLASS(kyty_binding_capture_dialog_parent_class)->dispose(object);
}

static void kyty_binding_capture_dialog_finalize(GObject* object) {
	auto* self = KYTY_BINDING_CAPTURE_DIALOG(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_binding_capture_dialog_parent_class)->finalize(object);
}

static void kyty_binding_capture_dialog_class_init(KytyBindingCaptureDialogClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_binding_capture_dialog_dispose;
	object_class->finalize = kyty_binding_capture_dialog_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyBindingCaptureDialog, capture_area);
	gtk_widget_class_bind_template_child(widget_class, KytyBindingCaptureDialog, device_image);
	gtk_widget_class_bind_template_child(widget_class, KytyBindingCaptureDialog, prompt_label);
	gtk_widget_class_bind_template_child(widget_class, KytyBindingCaptureDialog, refusal_label);
}

KytyBindingCaptureDialog* kyty_binding_capture_dialog_new(kyty::gtk::LauncherContext& context,
                                                          GtkWindow*                  parent,
                                                          kyty::core::InputDevice     device,
                                                          const std::string&          binding_title,
                                                          kyty::gtk::BindingCapturedFn done) {
	auto* self =
	    KYTY_BINDING_CAPTURE_DIALOG(g_object_new(KYTY_TYPE_BINDING_CAPTURE_DIALOG, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	state.device  = device;
	state.done    = std::move(done);
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	// TRANSLATORS: the capture dialog's title; %s names the binding ("Face buttons / Cross").
	gtk_window_set_title(GTK_WINDOW(self),
	                     Format(_("Set binding — %s"), binding_title.c_str()).c_str());
	if (device == core::InputDevice::Pad) {
		gtk_image_set_from_icon_name(self->device_image, "input-gaming-symbolic");
		// The service may call the listener before AddListener returns; the inputs held now are
		// read only afterwards.
		state.listener =
		    context.services.controller.AddListener([self]() { OnControllerChanged(self); });
		state.pad   = kyty::gtk::PadCapture(context.services.controller.State().raw_pressed);
		state.ready = true;
		UpdatePadText(self);
	} else {
		gtk_image_set_from_icon_name(self->device_image, "input-keyboard-symbolic");
		gtk_label_set_text(
		    self->prompt_label,
		    _("Press a key or mouse button.\nF1, F2, F7, F8, and F11 are reserved; Esc cancels."));
	}
	return self;
}

gboolean kyty_binding_capture_dialog_handle_key(KytyBindingCaptureDialog* dialog, guint keyval,
                                                GdkModifierType state) {
	g_return_val_if_fail(KYTY_IS_BINDING_CAPTURE_DIALOG(dialog), FALSE);
	if (dialog->state->finished) {
		return TRUE;
	}
	auto result = kyty::gtk::ClassifyCapturedKey(keyval, state);
	if (result.outcome == KeyCapture::Cancelled) {
		Finish(dialog, std::nullopt, true);
		return TRUE;
	}
	if (dialog->state->device == core::InputDevice::Pad) {
		return FALSE; // only Escape: the controller gives the binding
	}
	switch (result.outcome) {
		case KeyCapture::Bound: Finish(dialog, std::move(result.name), true); break;
		case KeyCapture::Reserved:
			ShowRefusal(dialog, _("That key is reserved by the emulator."));
			break;
		case KeyCapture::Modified:
			ShowRefusal(dialog, _("Press the key without Ctrl, Alt, or Super."));
			break;
		case KeyCapture::Unsupported:
		case KeyCapture::Cancelled: ShowRefusal(dialog, _("That key is not supported.")); break;
	}
	return TRUE;
}

void kyty_binding_capture_dialog_handle_button(KytyBindingCaptureDialog* dialog, guint button) {
	g_return_if_fail(KYTY_IS_BINDING_CAPTURE_DIALOG(dialog));
	if (dialog->state->finished || dialog->state->device != core::InputDevice::KeyboardMouse) {
		return;
	}
	if (auto binding = kyty::gtk::MouseButtonBinding(button)) {
		Finish(dialog, std::move(binding), true);
	}
}
