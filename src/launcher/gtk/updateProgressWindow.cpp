#include "launcher/gtk/updateProgressWindow.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <cstdarg>
#include <string>

struct _KytyUpdateProgressWindow {
	AdwWindow       parent_instance;
	GtkLabel*       status_label;
	GtkStack*       progress_stack;
	GtkProgressBar* progress_bar;
	GtkSpinner*     spinner;
	GtkWidget*      cancel_button;
	gboolean        restarting;
};

G_DEFINE_FINAL_TYPE(KytyUpdateProgressWindow, kyty_update_progress_window, ADW_TYPE_WINDOW)

namespace {

using kyty::gtk::GCharPtr;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/update-progress-window.xml";

guint g_cancel_requested_signal = 0;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

std::string SizeText(uint64_t bytes) {
	const GCharPtr text(g_format_size(bytes));
	return std::string(text.View());
}

void RequestCancel(KytyUpdateProgressWindow* self) {
	if (self->restarting == FALSE) {
		g_signal_emit(self, g_cancel_requested_signal, 0);
	}
}

void ShowBusy(KytyUpdateProgressWindow* self, const char* text) {
	gtk_label_set_text(self->status_label, text);
	gtk_stack_set_visible_child_name(self->progress_stack, "busy");
	gtk_spinner_start(self->spinner);
}

} // namespace

// The template's Cancel button.
static void kyty_update_progress_window_on_cancel(GtkButton* /*button*/,
                                                  KytyUpdateProgressWindow* self) {
	RequestCancel(self);
}

// Escape (window.close) and the header bar's close button: Cancel while it is offered; the owner
// destroys the window.
static gboolean kyty_update_progress_window_close_request(GtkWindow* window) {
	RequestCancel(KYTY_UPDATE_PROGRESS_WINDOW(window));
	return TRUE;
}

static void kyty_update_progress_window_init(KytyUpdateProgressWindow* self) {
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_update_progress_window_dispose(GObject* object) {
	gtk_widget_dispose_template(GTK_WIDGET(object), KYTY_TYPE_UPDATE_PROGRESS_WINDOW);
	G_OBJECT_CLASS(kyty_update_progress_window_parent_class)->dispose(object);
}

static void kyty_update_progress_window_class_init(KytyUpdateProgressWindowClass* klass) {
	auto* object_class          = G_OBJECT_CLASS(klass);
	auto* widget_class          = GTK_WIDGET_CLASS(klass);
	auto* window_class          = GTK_WINDOW_CLASS(klass);
	object_class->dispose       = kyty_update_progress_window_dispose;
	window_class->close_request = kyty_update_progress_window_close_request;
	g_cancel_requested_signal =
	    g_signal_new("cancel-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
	                 nullptr, nullptr, G_TYPE_NONE, 0);
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyUpdateProgressWindow, status_label);
	gtk_widget_class_bind_template_child(widget_class, KytyUpdateProgressWindow, progress_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyUpdateProgressWindow, progress_bar);
	gtk_widget_class_bind_template_child(widget_class, KytyUpdateProgressWindow, spinner);
	gtk_widget_class_bind_template_child(widget_class, KytyUpdateProgressWindow, cancel_button);
	gtk_widget_class_bind_template_callback(widget_class, kyty_update_progress_window_on_cancel);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyUpdateProgressWindow* kyty_update_progress_window_new(GtkWindow* parent, std::string_view tag) {
	auto* self =
	    KYTY_UPDATE_PROGRESS_WINDOW(g_object_new(KYTY_TYPE_UPDATE_PROGRESS_WINDOW, nullptr));
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	const std::string release(tag);
	// TRANSLATORS: %s is the tag of the release being downloaded.
	gtk_label_set_text(self->status_label, Format(_("Downloading %s…"), release.c_str()).c_str());
	kyty_update_progress_window_set_downloaded(self, 0, 0);
	return self;
}

void kyty_update_progress_window_set_downloaded(KytyUpdateProgressWindow* self, uint64_t received,
                                                uint64_t total) {
	g_return_if_fail(KYTY_IS_UPDATE_PROGRESS_WINDOW(self));
	const double fraction =
	    total > 0 ? std::min(1.0, static_cast<double>(received) / static_cast<double>(total)) : 0.0;
	gtk_progress_bar_set_fraction(self->progress_bar, fraction);
	// TRANSLATORS: the bytes downloaded so far, then the size of the package ("1.2 MB of 50.0 MB").
	const std::string text =
	    Format(_("%s of %s"), SizeText(received).c_str(), SizeText(total).c_str());
	gtk_progress_bar_set_text(self->progress_bar, text.c_str());
	gtk_stack_set_visible_child_name(self->progress_stack, "download");
}

void kyty_update_progress_window_set_preparing(KytyUpdateProgressWindow* self) {
	g_return_if_fail(KYTY_IS_UPDATE_PROGRESS_WINDOW(self));
	ShowBusy(self, _("Preparing the update…"));
}

void kyty_update_progress_window_set_restarting(KytyUpdateProgressWindow* self) {
	g_return_if_fail(KYTY_IS_UPDATE_PROGRESS_WINDOW(self));
	self->restarting = TRUE;
	ShowBusy(self, _("Restarting to install the update…"));
	gtk_widget_set_sensitive(self->cancel_button, FALSE);
}
