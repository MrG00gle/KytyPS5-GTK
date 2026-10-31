#include "launcher/gtk/gamePickerDialog.h"

#include "launcher/core/text.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace {

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/game-picker-dialog.xml";

struct PickerState {
	std::vector<kyty::core::GameEntry>  games; // in row order
	kyty::gtk::GamePickedFn             picked;
	std::vector<kyty::gtk::SignalScope> signals;
};

} // namespace

struct _KytyGamePickerDialog {
	AdwWindow            parent_instance;
	AdwPreferencesGroup* group;
	GtkListBox*          list;
	GtkButton*           accept_button;
	PickerState*         state;
};

G_DEFINE_FINAL_TYPE(KytyGamePickerDialog, kyty_game_picker_dialog, ADW_TYPE_WINDOW)

namespace kyty::gtk {

std::vector<core::GameEntry> PickableGames(std::span<const core::GameEntry> entries) {
	std::vector<core::GameEntry> games;
	for (const auto& entry: entries) {
		if (!core::NormalizeTitleId(entry.title_id).empty()) {
			games.push_back(entry);
		}
	}
	std::stable_sort(
	    games.begin(), games.end(), [](const core::GameEntry& a, const core::GameEntry& b) {
		    const auto a_id = core::NormalizeTitleId(a.title_id);
		    const auto b_id = core::NormalizeTitleId(b.title_id);
		    if (a_id != b_id) {
			    return a_id < b_id;
		    }
		    if (const int order = core::CompareCaseInsensitive(a.title_name, b.title_name);
		        order != 0) {
			    return order < 0;
		    }
		    return a.path < b.path;
	    });
	return games;
}

} // namespace kyty::gtk

namespace {

void UpdateAcceptButton(KytyGamePickerDialog* self) {
	gtk_widget_set_sensitive(GTK_WIDGET(self->accept_button),
	                         gtk_list_box_get_selected_row(self->list) != nullptr ? TRUE : FALSE);
}

// Closes the dialog, then hands the selected game to the callback (once).
void ChooseSelected(KytyGamePickerDialog* self) {
	GtkListBoxRow* row = gtk_list_box_get_selected_row(self->list);
	if (row == nullptr || !self->state->picked) {
		return;
	}
	const int index = gtk_list_box_row_get_index(row);
	if (index < 0 || static_cast<std::size_t>(index) >= self->state->games.size()) {
		return;
	}
	const kyty::core::GameEntry entry  = self->state->games[static_cast<std::size_t>(index)];
	kyty::gtk::GamePickedFn     picked = std::exchange(self->state->picked, nullptr);
	gtk_window_close(GTK_WINDOW(self));
	picked(entry);
}

void OnAcceptClicked(GtkButton* /*button*/, gpointer data) {
	ChooseSelected(KYTY_GAME_PICKER_DIALOG(data));
}

void OnRowActivated(GtkListBox* list, GtkListBoxRow* row, gpointer data) {
	gtk_list_box_select_row(list, row);
	ChooseSelected(KYTY_GAME_PICKER_DIALOG(data));
}

void OnSelectionChanged(GtkListBox* /*list*/, gpointer data) {
	UpdateAcceptButton(KYTY_GAME_PICKER_DIALOG(data));
}

GtkWidget* GameRow(const kyty::core::GameEntry& entry) {
	const std::string title_id = kyty::core::NormalizeTitleId(entry.title_id);
	GtkWidget*        row      = adw_action_row_new();
	// Game data and paths are shown as they are, never as markup.
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), entry.title_name.empty()
	                                                            ? title_id.c_str()
	                                                            : entry.title_name.c_str());
	const std::string subtitle = title_id + "\n" + entry.path.string();
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	return row;
}

} // namespace

static void kyty_game_picker_dialog_init(KytyGamePickerDialog* self) {
	self->state = new PickerState;
	gtk_widget_init_template(GTK_WIDGET(self));
	GtkWidget* placeholder = gtk_label_new(_("No game in the library has a title ID."));
	gtk_label_set_wrap(GTK_LABEL(placeholder), TRUE);
	gtk_widget_add_css_class(placeholder, "dim-label");
	gtk_widget_set_margin_top(placeholder, 18);
	gtk_widget_set_margin_bottom(placeholder, 18);
	gtk_widget_set_margin_start(placeholder, 12);
	gtk_widget_set_margin_end(placeholder, 12);
	gtk_list_box_set_placeholder(self->list, placeholder);
	auto& signals = self->state->signals;
	signals.push_back(kyty::gtk::SignalScope::Connect(self->accept_button, "clicked",
	                                                  G_CALLBACK(OnAcceptClicked), self));
	signals.push_back(kyty::gtk::SignalScope::Connect(self->list, "row-activated",
	                                                  G_CALLBACK(OnRowActivated), self));
	signals.push_back(kyty::gtk::SignalScope::Connect(self->list, "selected-rows-changed",
	                                                  G_CALLBACK(OnSelectionChanged), self));
}

static void kyty_game_picker_dialog_dispose(GObject* object) {
	auto* self = KYTY_GAME_PICKER_DIALOG(object);
	if (self->state != nullptr) {
		self->state->signals.clear();
		self->state->picked = nullptr;
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_GAME_PICKER_DIALOG);
	G_OBJECT_CLASS(kyty_game_picker_dialog_parent_class)->dispose(object);
}

static void kyty_game_picker_dialog_finalize(GObject* object) {
	auto* self = KYTY_GAME_PICKER_DIALOG(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_game_picker_dialog_parent_class)->finalize(object);
}

static void kyty_game_picker_dialog_class_init(KytyGamePickerDialogClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_game_picker_dialog_dispose;
	object_class->finalize = kyty_game_picker_dialog_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyGamePickerDialog, group);
	gtk_widget_class_bind_template_child(widget_class, KytyGamePickerDialog, list);
	gtk_widget_class_bind_template_child(widget_class, KytyGamePickerDialog, accept_button);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyGamePickerDialog*
kyty_game_picker_dialog_new(GtkWindow* parent, std::span<const kyty::core::GameEntry> entries,
                            const std::string& title, const std::string& description,
                            const std::string& accept_label, kyty::gtk::GamePickedFn picked) {
	auto* self = KYTY_GAME_PICKER_DIALOG(g_object_new(KYTY_TYPE_GAME_PICKER_DIALOG, nullptr));
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	gtk_window_set_title(GTK_WINDOW(self), title.c_str());
	adw_preferences_group_set_description(self->group, description.c_str());
	gtk_button_set_label(self->accept_button, accept_label.c_str());
	self->state->games  = kyty::gtk::PickableGames(entries);
	self->state->picked = std::move(picked);
	for (const auto& entry: self->state->games) {
		gtk_list_box_append(self->list, GameRow(entry));
	}
	if (GtkListBoxRow* first = gtk_list_box_get_row_at_index(self->list, 0); first != nullptr) {
		gtk_list_box_select_row(self->list, first);
	}
	UpdateAcceptButton(self);
	return self;
}
