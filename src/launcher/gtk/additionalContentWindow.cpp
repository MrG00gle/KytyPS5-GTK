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

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

// What the GTask thread lists.
struct AddOnLoad {
	std::string                         addons_dir;
	core::GameEntry                     entry;
	std::uint64_t                       generation = 0;
	core::AdditionalContentListing      listing;
	std::vector<GObjectPtr<GdkTexture>> icons;     // per item; empty when it has no usable icon
	GObjectPtr<GdkTexture>              game_icon; // the game's icon0.png
};

struct AdditionalContentWindowState {
	LauncherContext* context = nullptr;
	core::GameEntry  entry;
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

// The status line (Requirement 11.3, HC-30): the launcher's translated text for each status (D46:
// not core::AdditionalContentMessage); the folder is shown as it is set.
std::string StatusText(const core::AdditionalContentListing& listing, const std::string& folder) {
	switch (listing.status) {
		case core::AdditionalContentStatus::Listed: {
			const auto count = static_cast<unsigned long>(listing.items.size());
			return Format(kyty::gtk::NGettext("%d installed add-on", "%d installed add-ons", count),
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
	load.listing = core::LoadAdditionalContent(load.addons_dir, load.entry.title_id);
	for (const auto& item: load.listing.items) {
		const auto icon = core::ReadEntryFile(item.directory, core::GameEntry::Kind::Directory,
		                                      "sce_sys/icon0.png", core::kMaxImageBytes);
		load.icons.push_back(GObjectPtr<GdkTexture>::Adopt(
		    kyty::gtk::DecodePng(icon.value_or(""), item.directory, "icon0.png")));
	}
	load.game_icon = GObjectPtr<GdkTexture>::Adopt(
	    kyty::gtk::DecodePng(core::ReadIcon0(load.entry.path, load.entry.kind).value_or(""),
	                         load.entry.path, "icon0.png"));
}

GtkWidget* NewItemRow(const core::AdditionalContentItem& item, GdkTexture* icon) {
	GtkWidget* row = adw_action_row_new();
	// Add-on titles are game data, never markup.
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item.display_title.c_str());
	// TRANSLATORS: the state of a listed add-on (the Qt launcher's).
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), C_("add-on", "Installed"));
	gtk_widget_set_tooltip_text(row, item.content_id.c_str());
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
		gtk_list_box_append(self->items_list, NewItemRow(items[i], icon));
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
	auto load        = std::make_shared<AddOnLoad>();
	load->addons_dir = state.context->settings.loaded.settings.addons_dir;
	load->entry      = state.entry;
	load->generation = state.generation;
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
	List(self);
	return self;
}

bool kyty_additional_content_window_is_loaded(KytyAdditionalContentWindow* window) {
	g_return_val_if_fail(KYTY_IS_ADDITIONAL_CONTENT_WINDOW(window), false);
	return window->state != nullptr && window->state->loaded;
}
