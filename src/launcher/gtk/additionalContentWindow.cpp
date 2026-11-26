#include "launcher/gtk/additionalContentWindow.h"

#include "launcher/core/additionalContentModel.h"
#include "launcher/core/gameContent.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/alerts.h"
#include "launcher/gtk/backgroundTask.h"
#include "launcher/gtk/focusSupport.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/imageDecoding.h"
#include "launcher/gtk/services/desktopLauncher.h"
#include "launcher/gtk/services/fileChooser.h"
#include "launcher/gtk/settingsWrite.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

// What the GTask thread lists.
struct AddOnLoad {
	std::string                         addons_dir;
	core::GameEntry                     entry;
	std::vector<std::string>            user_addons; // the game's, as stored
	std::uint64_t                       generation = 0;
	core::AdditionalContentListing      listing;
	std::vector<GObjectPtr<GdkTexture>> icons;     // per item; empty when it has no usable icon
	GObjectPtr<GdkTexture>              game_icon; // the game's icon0.png
};

struct AdditionalContentWindowState {
	LauncherContext* context = nullptr;
	core::GameEntry  entry;
	std::string      title_id;       // NormalizeTitleId(entry.title_id): the user add-ons' key
	std::string      folder;         // the add-ons folder of the latest listing
	std::uint64_t    generation = 0; // of the latest listing
	bool             loaded     = false;
	bool             disposed   = false;
	// Refresh moved the focus out of the list (the focus returns to Refresh after the listing).
	bool refocus_refresh = false;
	// False from dispose on: a pending listing's result is dropped.
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);
	// Cancelled when the window goes away: no Open folder or alert callback runs after it.
	GObjectPtr<GCancellable> cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyAdditionalContentWindow {
	AdwWindow       parent_instance;
	AdwWindowTitle* window_title;
	GtkStack*       content_stack;
	GtkSpinner*     spinner;
	GtkListBox*     items_list;
	GtkLabel*       status_label;
	GtkWidget*      refresh_button;
	GtkWidget*      open_button;
	GtkWidget*      add_group;
	GtkWidget*      add_button;
	GtkWidget*      label_row;
	GtkWidget*      folder_row;
	GtkWidget*      choose_button;

	kyty::gtk::AdditionalContentWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyAdditionalContentWindow, kyty_additional_content_window, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::AddOnLoad;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/additional-content-window.xml";

constexpr int kIconSize = 64; // the Qt launcher's

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyAdditionalContentWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

// The game's user add-ons in the settings as they are now.
std::vector<std::string> UserAddOns(const kyty::gtk::AdditionalContentWindowState& state) {
	if (state.title_id.empty()) {
		return {};
	}
	const auto& all   = state.context->settings.loaded.settings.user_addons;
	const auto  found = all.find(state.title_id);
	return found != all.end() ? found->second : std::vector<std::string> {};
}

// The status line (Requirement 11.3, HC-30): the launcher's translated text for each status (D46:
// not core::AdditionalContentMessage); the folder is shown as it is set.
std::string StatusText(const core::AdditionalContentListing& listing, const std::string& folder) {
	switch (listing.status) {
		case core::AdditionalContentStatus::Listed: {
			// The packages of the add-ons folder and the user add-ons.
			const auto count = static_cast<unsigned long>(listing.items.size());
			return Format(kyty::gtk::NGettext("%d add-on", "%d add-ons", count),
			              static_cast<int>(count));
		}
		case core::AdditionalContentStatus::FolderNotSet:
			return _("No add-ons folder is set. Open Global settings and set the Add-ons folder to "
			         "the location of your extracted add-on packages.");
		case core::AdditionalContentStatus::FolderMissing:
			return Format(_("The add-ons folder does not exist:\n%s\nCheck the path in Global "
			                "settings."),
			              folder.c_str());
		case core::AdditionalContentStatus::NoItems:
			return Format(_("No add-ons found for this game in:\n%s"), folder.c_str());
	}
	return "";
}

// The GTask thread's work: the listing and the decoded icons.
void Read(AddOnLoad& load) {
	load.listing =
	    core::LoadAdditionalContent(load.addons_dir, load.entry.title_id, load.user_addons);
	for (const auto& item: load.listing.items) {
		if (!item.user_entry.empty()) {
			load.icons.emplace_back(); // a user add-on shows the game's icon (upstream's)
			continue;
		}
		const auto icon = core::ReadEntryFile(item.directory, core::GameEntry::Kind::Directory,
		                                      "sce_sys/icon0.png", core::kMaxImageBytes);
		load.icons.push_back(GObjectPtr<GdkTexture>::Adopt(
		    kyty::gtk::DecodePng(icon.value_or(""), item.directory, "icon0.png")));
	}
	load.game_icon = GObjectPtr<GdkTexture>::Adopt(
	    kyty::gtk::DecodePng(core::ReadIcon0(load.entry.path, load.entry.kind).value_or(""),
	                         load.entry.path, "icon0.png"));
}

void OnRemove(GtkButton* button, gpointer data);

// The data key of a Remove button: the user add-on's entry.
constexpr const char* kUserEntryKey = "kyty-user-addon";

// The second line of a user add-on (upstream's texts).
std::string UserAddOnState(const core::AdditionalContentItem& item) {
	if (item.directory.empty()) {
		// TRANSLATORS: a user add-on without a data folder (a license only).
		return C_("add-on", "License only");
	}
	// TRANSLATORS: %s is the data folder of a user add-on.
	return Format(item.missing_directory ? _("Data folder not found: %s") : _("Data: %s"),
	              item.directory.c_str());
}

GtkWidget* NewItemRow(KytyAdditionalContentWindow* self, const core::AdditionalContentItem& item,
                      GdkTexture* icon) {
	GtkWidget* row = adw_action_row_new();
	// Add-on titles are game data, never markup.
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item.display_title.c_str());
	if (item.user_entry.empty()) {
		// TRANSLATORS: the state of a listed add-on (the Qt launcher's).
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), C_("add-on", "Installed"));
		gtk_widget_set_tooltip_text(row, item.content_id.c_str());
	} else {
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), UserAddOnState(item).c_str());
		GtkWidget* remove = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(remove, _("Remove this add-on from the game"));
		// TRANSLATORS: the Remove button of a user add-on; %s is its entitlement label.
		const std::string name = Format(_("Remove %s"), item.display_title.c_str());
		gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
		                               name.c_str(), -1);
		g_object_set_data_full(G_OBJECT(remove), kUserEntryKey, g_strdup(item.user_entry.c_str()),
		                       g_free);
		// The button goes with its row, before the window.
		g_signal_connect(remove, "clicked", G_CALLBACK(&OnRemove), self);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
	}
	GtkWidget* image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image), kIconSize);
	if (icon != nullptr) {
		gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(icon));
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(image), "applications-games-symbolic");
		gtk_widget_add_css_class(image, "dim-label");
	}
	gtk_widget_set_margin_top(image, 6);
	gtk_widget_set_margin_bottom(image, 6);
	gtk_accessible_update_state(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	adw_action_row_add_prefix(ADW_ACTION_ROW(row), image);
	return row;
}

void Apply(KytyAdditionalContentWindow* self, AddOnLoad& load) {
	auto& state = *self->state;
	if (load.generation != state.generation) {
		return; // a later Refresh is pending
	}
	state.loaded = true;
	state.folder = load.addons_dir;
	gtk_spinner_stop(self->spinner);
	// The status line next to the list is selectable (FEAT-012 (11)).
	kyty::gtk::MoveFocusOutOf(GTK_WINDOW(self), GTK_WIDGET(self->items_list), self->refresh_button);
	gtk_list_box_remove_all(self->items_list);
	const auto& items = load.listing.items;
	for (std::size_t i = 0; i < items.size(); i++) {
		// HC-30: an add-on without a usable icon shows the game's.
		GdkTexture* icon =
		    i < load.icons.size() && load.icons[i] ? load.icons[i].get() : load.game_icon.get();
		gtk_list_box_append(self->items_list, NewItemRow(self, items[i], icon));
	}
	gtk_stack_set_visible_child_name(self->content_stack, items.empty() ? "empty" : "items");
	gtk_label_set_text(self->status_label, StatusText(load.listing, load.addons_dir).c_str());
	// The Qt launcher enabled Open folder only while the folder exists.
	gtk_widget_set_sensitive(self->open_button, load.listing.folder_exists ? TRUE : FALSE);
	gtk_widget_set_sensitive(self->refresh_button, TRUE);
	// The focus left a row for Refresh, which was off while it listed: it goes back to Refresh.
	if (std::exchange(state.refocus_refresh, false) &&
	    gtk_root_get_focus(GTK_ROOT(self)) == nullptr) {
		gtk_widget_grab_focus(self->refresh_button);
	}
	g_debug("additional content of %s in \"%s\": %zu item(s)", state.entry.path.c_str(),
	        load.addons_dir.c_str(), items.size());
}

void List(KytyAdditionalContentWindow* self) {
	auto& state  = *self->state;
	state.loaded = false;
	state.generation++;
	// Refresh (its mnemonic) with a focused row: the rows are hidden now and removed later.
	GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(self));
	state.refocus_refresh =
	    focus != nullptr && gtk_widget_is_ancestor(focus, GTK_WIDGET(self->items_list)) != FALSE;
	kyty::gtk::MoveFocusOutOf(GTK_WINDOW(self), GTK_WIDGET(self->items_list), self->refresh_button);
	gtk_widget_set_sensitive(self->refresh_button, FALSE);
	gtk_widget_set_sensitive(self->open_button, FALSE);
	gtk_stack_set_visible_child_name(self->content_stack, "loading");
	gtk_spinner_start(self->spinner);
	auto load         = std::make_shared<AddOnLoad>();
	load->addons_dir  = state.context->settings.loaded.settings.addons_dir;
	load->entry       = state.entry;
	load->user_addons = UserAddOns(state);
	load->generation  = state.generation;
	// An unowned GTask, a recorded exception (p6-u9-6): the alive flag drops a late result.
	kyty::gtk::RunInBackground([load]() { Read(*load); },
	                           [self, alive = state.alive, load]() {
		                           if (*alive) {
			                           Apply(self, *load);
		                           }
	                           });
}

void OnRefresh(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_ADDITIONAL_CONTENT_WINDOW(data);
	if (Alive(self)) {
		List(self);
	}
}

void OnOpenFolder(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_ADDITIONAL_CONTENT_WINDOW(data);
	if (!Alive(self) || self->state->folder.empty()) {
		return;
	}
	auto& state = *self->state;
	state.context->services.desktop_launcher.OpenFolder(
	    GTK_WINDOW(self), state.folder,
	    [self](const std::string& error) {
		    if (Alive(self) && !error.empty()) {
			    g_message("additional content: cannot open %s: %s", self->state->folder.c_str(),
			              error.c_str());
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The add-ons folder could not be opened"),
			                         error, self->state->cancellable.get());
		    }
	    },
	    state.cancellable.get());
}

// ---- User add-ons (upstream KytyPS5's launcher, commit 66e21a62)

// Saves entries as the game's user add-ons; saved runs after the Settings_Store was written.
void SaveUserAddOns(KytyAdditionalContentWindow* self, std::vector<std::string> entries,
                    std::function<void()> saved) {
	auto&                  state = *self->state;
	core::LauncherSettings next  = state.context->settings.loaded.settings;
	if (entries.empty()) {
		next.user_addons.erase(state.title_id);
	} else {
		next.user_addons[state.title_id] = std::move(entries);
	}
	kyty::gtk::SaveSettingsInteractively(GTK_WINDOW(self), *state.context, std::move(next),
	                                     state.cancellable.get(),
	                                     [alive = state.alive, saved = std::move(saved)]() {
		                                     if (*alive) {
			                                     saved();
		                                     }
	                                     });
}

void MarkRow(GtkWidget* row, bool error) {
	if (error) {
		gtk_widget_add_css_class(row, "error");
	} else {
		gtk_widget_remove_css_class(row, "error");
	}
}

void OnAdd(KytyAdditionalContentWindow* self) {
	if (!Alive(self) || self->state->title_id.empty()) {
		return;
	}
	auto entry = core::MakeUserAddOn(gtk_editable_get_text(GTK_EDITABLE(self->label_row)),
	                                 gtk_editable_get_text(GTK_EDITABLE(self->folder_row)));
	MarkRow(self->label_row, !entry && entry.error() == core::UserAddOnError::InvalidLabel);
	MarkRow(self->folder_row, !entry && entry.error() == core::UserAddOnError::MissingFolder);
	if (!entry) {
		// Upstream's text.
		gtk_label_set_text(self->status_label,
		                   _("Enter a 16-character entitlement label (A-Z, 0-9) and an existing "
		                     "data folder, or no folder for a license only."));
		return;
	}
	auto entries = UserAddOns(*self->state);
	g_debug("additional content of %s: add %s", self->state->entry.path.c_str(), entry->c_str());
	core::AddUserAddOn(entries, std::move(*entry));
	SaveUserAddOns(self, std::move(entries), [self]() {
		gtk_editable_set_text(GTK_EDITABLE(self->label_row), "");
		gtk_editable_set_text(GTK_EDITABLE(self->folder_row), "");
		List(self);
	});
}

void OnAddClicked(GtkButton* /*button*/, gpointer data) {
	OnAdd(KYTY_ADDITIONAL_CONTENT_WINDOW(data));
}

void OnRowActivated(AdwEntryRow* /*row*/, gpointer data) {
	OnAdd(KYTY_ADDITIONAL_CONTENT_WINDOW(data));
}

void OnRemove(GtkButton* button, gpointer data) {
	auto*       self = KYTY_ADDITIONAL_CONTENT_WINDOW(data);
	const auto* entry =
	    static_cast<const char*>(g_object_get_data(G_OBJECT(button), kUserEntryKey));
	if (!Alive(self) || entry == nullptr) {
		return;
	}
	auto entries = UserAddOns(*self->state);
	if (!core::RemoveUserAddOn(entries, entry)) {
		return;
	}
	g_debug("additional content of %s: remove %s", self->state->entry.path.c_str(), entry);
	SaveUserAddOns(self, std::move(entries), [self]() { List(self); });
}

void OnChooseFolder(GtkButton* /*button*/, gpointer data) {
	auto* self = KYTY_ADDITIONAL_CONTENT_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	auto& state = *self->state;
	state.context->services.file_chooser.SelectFolders(
	    GTK_WINDOW(self), _("Select add-on data folder"),
	    [self](std::vector<std::filesystem::path> folders, const std::string& error) {
		    if (!Alive(self)) {
			    return;
		    }
		    if (!error.empty()) {
			    kyty::gtk::ShowError(GTK_WINDOW(self), _("The folder could not be chosen"), error,
			                         self->state->cancellable.get());
			    return;
		    }
		    if (!folders.empty()) {
			    gtk_editable_set_text(GTK_EDITABLE(self->folder_row), folders.front().c_str());
		    }
	    },
	    state.cancellable.get());
}

} // namespace

static void kyty_additional_content_window_init(KytyAdditionalContentWindow* self) {
	self->state = new kyty::gtk::AdditionalContentWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_additional_content_window_dispose(GObject* object) {
	auto* self = KYTY_ADDITIONAL_CONTENT_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		*state->alive   = false;
		g_cancellable_cancel(state->cancellable.get());
		state->signals.clear();
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_ADDITIONAL_CONTENT_WINDOW);
	G_OBJECT_CLASS(kyty_additional_content_window_parent_class)->dispose(object);
}

static void kyty_additional_content_window_finalize(GObject* object) {
	auto* self = KYTY_ADDITIONAL_CONTENT_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_additional_content_window_parent_class)->finalize(object);
}

static void kyty_additional_content_window_class_init(KytyAdditionalContentWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_additional_content_window_dispose;
	object_class->finalize = kyty_additional_content_window_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, window_title);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, content_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, spinner);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, items_list);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, status_label);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, refresh_button);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, open_button);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, add_group);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, add_button);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, label_row);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, folder_row);
	gtk_widget_class_bind_template_child(widget_class, KytyAdditionalContentWindow, choose_button);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyAdditionalContentWindow*
kyty_additional_content_window_new(kyty::gtk::LauncherContext& context, GtkWindow* parent,
                                   const kyty::core::GameEntry& entry) {
	auto* self =
	    KYTY_ADDITIONAL_CONTENT_WINDOW(g_object_new(KYTY_TYPE_ADDITIONAL_CONTENT_WINDOW, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	state.entry   = entry;
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	const std::string title_id = core::NormalizeTitleId(entry.title_id);
	const std::string name     = entry.title_name.empty() ? title_id : entry.title_name;
	std::string       game     = name;
	if (!title_id.empty() && title_id != name) {
		// TRANSLATORS: the game of the additional content window: "<game name> · <title ID>".
		game = Format(C_("additional content", "%s · %s"), name.c_str(), title_id.c_str());
	}
	adw_window_title_set_subtitle(self->window_title, game.c_str());
	const std::string title =
	    // TRANSLATORS: the additional content window's title; %s is the game's name.
	    Format(_("Additional content — %s"), name.c_str());
	gtk_window_set_title(GTK_WINDOW(self), title.c_str());
	state.signals.push_back(
	    SignalScope::Connect(self->refresh_button, "clicked", G_CALLBACK(&OnRefresh), self));
	state.signals.push_back(
	    SignalScope::Connect(self->open_button, "clicked", G_CALLBACK(&OnOpenFolder), self));
	state.signals.push_back(
	    SignalScope::Connect(self->add_button, "clicked", G_CALLBACK(&OnAddClicked), self));
	state.signals.push_back(
	    SignalScope::Connect(self->choose_button, "clicked", G_CALLBACK(&OnChooseFolder), self));
	for (GtkWidget* row: {self->label_row, self->folder_row}) {
		state.signals.push_back(
		    SignalScope::Connect(row, "entry-activated", G_CALLBACK(&OnRowActivated), self));
	}
	// User add-ons are kept by title ID (upstream's launcher offers none without one).
	state.title_id = title_id;
	gtk_widget_set_sensitive(self->add_group, title_id.empty() ? FALSE : TRUE);
	List(self);
	return self;
}

bool kyty_additional_content_window_is_loaded(KytyAdditionalContentWindow* window) {
	g_return_val_if_fail(KYTY_IS_ADDITIONAL_CONTENT_WINDOW(window), false);
	return window->state != nullptr && window->state->loaded;
}
