#include "launcher/gtk/trophyDetailsDialog.h"

#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/trophyPresentation.h"

#include <cstdarg>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

struct TrophyDetailsDialogState {
	core::TrophyRow             shown;
	core::TrophyRow             real;
	GObjectPtr<GdkTexture>      icon;
	std::function<void(int id)> revealed;
	std::vector<SignalScope>    signals;
	bool                        disposed = false;
};

} // namespace kyty::gtk

struct _KytyTrophyDetailsDialog {
	AdwWindow  parent_instance;
	GtkImage*  icon;
	GtkLabel*  name_label;
	GtkWidget* hidden_label;
	GtkWidget* reveal_button;
	GtkLabel*  grade_value;
	GtkLabel*  status_value;
	GtkLabel*  date_value;
	GtkLabel*  details_value;
	GtkWidget* reward_key;
	GtkLabel*  reward_value;
	GtkWidget* close_button;

	kyty::gtk::TrophyDetailsDialogState* state;
};

G_DEFINE_FINAL_TYPE(KytyTrophyDetailsDialog, kyty_trophy_details_dialog, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/trophy-details-dialog.xml";

// A value the trophy does not have (the Qt launcher's "-").
constexpr const char* kNoValue = "\u2014";

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	kyty::gtk::GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

// The trophy's name; "Trophy <id>" when the package has none (the Qt launcher's text, translated).
std::string TrophyName(const core::TrophyRow& row) {
	if (row.masked) {
		return _("Hidden trophy");
	}
	// TRANSLATORS: the name of a trophy that has none in its package; %d is its ID.
	return row.name.empty() ? Format(_("Trophy %d"), row.id) : row.name;
}

void Show(KytyTrophyDetailsDialog* self, const core::TrophyRow& row) {
	const auto& state = *self->state;
	if (row.masked) {
		gtk_image_set_from_paintable(self->icon, GDK_PAINTABLE(kyty::gtk::HiddenTrophyTexture()));
	} else if (state.icon) {
		gtk_image_set_from_paintable(self->icon, GDK_PAINTABLE(state.icon.get()));
	} else {
		gtk_image_set_from_icon_name(self->icon, "kyty-trophy-symbolic");
	}
	// Locked icons are grayed (style.css); the hidden trophy image keeps its colors.
	if (!row.masked && !row.unlocked) {
		gtk_widget_add_css_class(GTK_WIDGET(self->icon), "trophy-locked");
	} else {
		gtk_widget_remove_css_class(GTK_WIDGET(self->icon), "trophy-locked");
	}
	const std::string name = TrophyName(row);
	gtk_label_set_text(self->name_label, name.c_str());
	gtk_widget_set_visible(self->hidden_label, row.hidden && !row.masked ? TRUE : FALSE);
	gtk_widget_set_visible(self->reveal_button, row.masked ? TRUE : FALSE);
	gtk_label_set_text(self->grade_value,
	                   row.masked ? _("Hidden trophy") : kyty::gtk::TrophyGradeLabel(row.grade));
	gtk_label_set_text(self->status_value, row.unlocked ? _("Earned") : _("Not earned"));
	const std::string date = row.unlocked && row.earned_unix_ms
	                             ? kyty::gtk::TrophyEarnedText(*row.earned_unix_ms)
	                             : std::string();
	gtk_label_set_text(self->date_value, date.empty() ? kNoValue : date.c_str());
	gtk_label_set_text(self->details_value,
	                   row.masked || row.description.empty() ? kNoValue : row.description.c_str());
	// The reward row only when the package defines a reward text (the Qt launcher's rule).
	const bool reward = state.real.has_reward && !state.real.reward.empty();
	gtk_widget_set_visible(self->reward_key, reward ? TRUE : FALSE);
	gtk_widget_set_visible(GTK_WIDGET(self->reward_value), reward ? TRUE : FALSE);
	gtk_label_set_text(self->reward_value, row.masked ? kNoValue : row.reward.c_str());
}

void OnReveal(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_TROPHY_DETAILS_DIALOG(data);
	if (self->state == nullptr || self->state->disposed || !self->state->shown.masked) {
		return;
	}
	auto& state = *self->state;
	state.shown = state.real;
	Show(self, state.shown);
	// The button goes away; the keyboard focus moves on to Close.
	gtk_widget_grab_focus(self->close_button);
	if (auto revealed = std::exchange(state.revealed, nullptr); revealed) {
		revealed(state.shown.id);
	}
}

} // namespace

static void kyty_trophy_details_dialog_init(KytyTrophyDetailsDialog* self) {
	self->state = new kyty::gtk::TrophyDetailsDialogState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_trophy_details_dialog_dispose(GObject* object) {
	auto* self = KYTY_TROPHY_DETAILS_DIALOG(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		state->signals.clear();
		state->revealed = nullptr;
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_TROPHY_DETAILS_DIALOG);
	G_OBJECT_CLASS(kyty_trophy_details_dialog_parent_class)->dispose(object);
}

static void kyty_trophy_details_dialog_finalize(GObject* object) {
	auto* self = KYTY_TROPHY_DETAILS_DIALOG(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_trophy_details_dialog_parent_class)->finalize(object);
}

static void kyty_trophy_details_dialog_class_init(KytyTrophyDetailsDialogClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_trophy_details_dialog_dispose;
	object_class->finalize = kyty_trophy_details_dialog_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, icon);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, name_label);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, hidden_label);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, reveal_button);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, grade_value);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, status_value);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, date_value);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, details_value);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, reward_key);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, reward_value);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyDetailsDialog, close_button);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyTrophyDetailsDialog* kyty_trophy_details_dialog_new(GtkWindow*                   parent,
                                                        const kyty::core::TrophyRow& shown,
                                                        const kyty::core::TrophyRow& real,
                                                        GdkTexture*                  icon,
                                                        std::function<void(int id)>  revealed) {
	auto* self = KYTY_TROPHY_DETAILS_DIALOG(g_object_new(KYTY_TYPE_TROPHY_DETAILS_DIALOG, nullptr));
	auto& state    = *self->state;
	state.shown    = shown;
	state.real     = shown.masked ? real : shown;
	state.icon     = kyty::gtk::GObjectPtr<GdkTexture>::Ref(icon);
	state.revealed = std::move(revealed);
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	state.signals.push_back(kyty::gtk::SignalScope::Connect(self->reveal_button, "clicked",
	                                                        G_CALLBACK(&OnReveal), self));
	Show(self, state.shown);
	return self;
}
