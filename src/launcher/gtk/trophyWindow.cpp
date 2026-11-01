#include "launcher/gtk/trophyWindow.h"

#include "launcher/core/gameContent.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/titleId.h"
#include "launcher/core/trophyModel.h"
#include "launcher/gtk/backgroundTask.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/imageDecoding.h"
#include "launcher/gtk/trophyDetailsDialog.h"
#include "launcher/gtk/trophyPresentation.h"

#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

// One readable package and what its page shows.
struct TrophyPage {
	core::LoadedTrophyPackage             loaded;
	std::set<int>                         revealed; // hidden trophies the user revealed
	std::map<int, GObjectPtr<GdkTexture>> icons;    // by trophy ID
	std::vector<core::TrophyRow>          rows;     // as shown, in package order
	GtkListBox*                           list = nullptr;
	std::vector<SignalScope>              row_signals;
};

// What the GTask thread reads.
struct TrophyLoad {
	core::GameEntry       entry;
	std::filesystem::path data_dir;
	int                   user_id          = 0;
	int                   console_language = 0;
	core::GameTrophies    trophies;
	// The file names of the packages that cannot be read, in file name order.
	std::vector<std::string>                           unreadable;
	std::vector<std::map<int, GObjectPtr<GdkTexture>>> icons; // per readable package
	GObjectPtr<GdkTexture>                             art;   // the game's icon0.png
};

struct TrophyWindowState {
	LauncherContext*                         context = nullptr;
	core::GameEntry                          entry;
	std::vector<std::unique_ptr<TrophyPage>> pages;
	GObjectPtr<GdkTexture>                   art;
	bool                                     loaded   = false;
	bool                                     disposed = false;
	// False from dispose on: the background read's result is dropped.
	std::shared_ptr<bool>    alive = std::make_shared<bool>(true);
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyTrophyWindow {
	AdwWindow       parent_instance;
	AdwWindowTitle* window_title;
	GtkStack*       content_stack;
	GtkSpinner*     spinner;
	AdwStatusPage*  error_page;
	GtkWidget*      errors_box;
	GtkLabel*       errors_label;
	GtkWidget*      switcher;
	AdwViewStack*   packages;

	kyty::gtk::TrophyWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyTrophyWindow, kyty_trophy_window, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;
using kyty::gtk::TrophyLoad;
using kyty::gtk::TrophyPage;

constexpr const char* kTemplateResource = "/io/github/MrG00gle/KytyPS5_GTK/ui/trophy-window.xml";

// The row index of a Reveal button, plus one (g_object_set_data on the button).
constexpr const char* kRowIndexKey = "kyty-trophy-row-index";

// The icon sizes of the Qt launcher's viewer (76 px rows, 56 px game art), a little smaller.
constexpr int kRowIconSize   = 64;
constexpr int kGameArtSize   = 64;
constexpr int kGradeIconSize = 24;

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyTrophyWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

std::string GameName(const core::GameEntry& entry) {
	return entry.title_name.empty() ? core::NormalizeTitleId(entry.title_id) : entry.title_name;
}

// The trophy's name as a row shows it (D46: the placeholder and the "Trophy <id>" fallback are
// the launcher's translated texts).
std::string RowName(const core::TrophyRow& row) {
	if (row.masked) {
		return _("Hidden trophy");
	}
	// TRANSLATORS: the name of a trophy that has none in its package; %d is its ID.
	return row.name.empty() ? Format(_("Trophy %d"), row.id) : row.name;
}

// "Could not read trophy package <file>." for each file, one per line (Requirement 11.1).
std::string UnreadableText(const std::vector<std::string>& files) {
	std::string text;
	for (const auto& file: files) {
		text += text.empty() ? "" : "\n";
		text += Format(_("Could not read trophy package %s."), file.c_str());
	}
	return text;
}

// The GTask thread's work: the packages and unlock data, the unreadable packages' names, and the
// decoded icons.
void Read(TrophyLoad& load) {
	load.trophies =
	    core::LoadGameTrophies(load.entry, load.data_dir, load.user_id, load.console_language);
	std::set<std::string> readable;
	for (const auto& loaded: load.trophies.packages) {
		readable.insert(loaded.package.file_name);
	}
	for (const auto& file: core::FindTrophyPackageFiles(load.entry)) {
		if (readable.count(file.file_name) == 0) {
			load.unreadable.push_back(file.file_name);
		}
	}
	for (const auto& loaded: load.trophies.packages) {
		std::map<int, GObjectPtr<GdkTexture>> icons;
		for (const auto& trophy: loaded.package.trophies) {
			if (!trophy.icon_png.empty()) {
				icons.emplace(trophy.id, GObjectPtr<GdkTexture>::Adopt(kyty::gtk::DecodePng(
				                             trophy.icon_png, load.entry.path, "trophy icon")));
			}
		}
		load.icons.push_back(std::move(icons));
	}
	load.art = GObjectPtr<GdkTexture>::Adopt(
	    kyty::gtk::DecodePng(core::ReadIcon0(load.entry.path, load.entry.kind).value_or(""),
	                         load.entry.path, "icon0.png"));
}

// ---- Rows

void FillRows(KytyTrophyWindow* self, TrophyPage& page);

TrophyPage* PageOf(KytyTrophyWindow* self, GtkWidget* widget) {
	GtkWidget* list = gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX);
	for (const auto& page: self->state->pages) {
		if (GTK_WIDGET(page->list) == list) {
			return page.get();
		}
	}
	return nullptr;
}

// Reveals the hidden trophy of row index: the page shows its real name and description.
void Reveal(KytyTrophyWindow* self, TrophyPage& page, std::size_t index) {
	if (index >= page.rows.size() || !page.rows[index].masked) {
		return;
	}
	page.revealed.insert(page.rows[index].id);
	FillRows(self, page);
	if (GtkListBoxRow* row = gtk_list_box_get_row_at_index(page.list, static_cast<int>(index));
	    row != nullptr) {
		gtk_widget_grab_focus(GTK_WIDGET(row));
	}
}

void OnRevealClicked(GtkButton* button, gpointer data) {
	auto* self = KYTY_TROPHY_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	TrophyPage*    page  = PageOf(self, GTK_WIDGET(button));
	const gpointer index = g_object_get_data(G_OBJECT(button), kRowIndexKey);
	if (page != nullptr && index != nullptr) {
		Reveal(self, *page, GPOINTER_TO_SIZE(index) - 1);
	}
}

void OnRowActivated(GtkListBox* list, GtkListBoxRow* row, gpointer data) {
	auto* self = KYTY_TROPHY_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	TrophyPage* page  = PageOf(self, GTK_WIDGET(row));
	const int   index = gtk_list_box_row_get_index(row);
	if (page == nullptr || page->list != list || index < 0 ||
	    static_cast<std::size_t>(index) >= page->rows.size()) {
		return;
	}
	const core::TrophyRow& shown = page->rows[static_cast<std::size_t>(index)];
	core::TrophyRow        real  = shown;
	if (shown.masked) {
		auto revealed = page->revealed;
		revealed.insert(shown.id);
		real = core::BuildTrophyViewer(page->loaded.package, page->loaded.unlocks, revealed)
		           .trophies[static_cast<std::size_t>(index)];
	}
	const auto  found = page->icons.find(shown.id);
	GdkTexture* icon  = found != page->icons.end() ? found->second.get() : nullptr;
	// The dialog is destroyed with the window (destroy-with-parent), so its callback never runs
	// after the window's dispose; alive guards it anyway.
	auto* dialog =
	    kyty_trophy_details_dialog_new(GTK_WINDOW(self), shown, real, icon,
	                                   [self, alive = self->state->alive, page,
	                                    row_index = static_cast<std::size_t>(index)](int /*id*/) {
		                                   if (*alive) {
			                                   Reveal(self, *page, row_index);
		                                   }
	                                   });
	gtk_window_present(GTK_WINDOW(dialog));
}

GtkWidget* NewLabel(const std::string& text, const char* css_class, bool wrap) {
	GtkWidget* label = gtk_label_new(text.c_str());
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	if (wrap) {
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	}
	if (css_class != nullptr) {
		gtk_widget_add_css_class(label, css_class);
	}
	return label;
}

GtkWidget* NewTrophyRow(KytyTrophyWindow* self, TrophyPage& page, std::size_t index) {
	const core::TrophyRow& trophy = page.rows[index];
	GtkWidget*             row    = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 10);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

	GtkWidget* icon = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(icon), kRowIconSize);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_widget_add_css_class(icon, "trophy-icon");
	const auto found = page.icons.find(trophy.id);
	if (trophy.masked) {
		gtk_image_set_from_paintable(GTK_IMAGE(icon),
		                             GDK_PAINTABLE(kyty::gtk::HiddenTrophyTexture()));
	} else if (found != page.icons.end() && found->second) {
		gtk_image_set_from_paintable(GTK_IMAGE(icon), GDK_PAINTABLE(found->second.get()));
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(icon), "kyty-trophy-symbolic");
	}
	// Locked icons are grayed (style.css); the hidden trophy image keeps its colors.
	if (!trophy.masked && !trophy.unlocked) {
		gtk_widget_add_css_class(icon, "trophy-locked");
	}
	gtk_box_append(GTK_BOX(box), icon);

	GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	const std::string name = RowName(trophy);
	gtk_box_append(GTK_BOX(text), NewLabel(name, "heading", true));
	if (!trophy.masked) {
		GtkWidget* grade = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_append(GTK_BOX(grade), kyty::gtk::NewTrophyGradeIcon(trophy.grade, 16));
		gtk_box_append(GTK_BOX(grade),
		               NewLabel(kyty::gtk::TrophyGradeLabel(trophy.grade), "dim-label", false));
		gtk_box_append(GTK_BOX(text), grade);
		if (!trophy.description.empty()) {
			gtk_box_append(GTK_BOX(text), NewLabel(trophy.description, nullptr, true));
		}
	}
	gtk_box_append(GTK_BOX(box), text);

	GtkWidget* side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_valign(side, GTK_ALIGN_CENTER);
	if (trophy.unlocked) {
		// The local earned time, or "Earned" when the unlock data has no time (the Qt launcher's).
		const std::string date =
		    trophy.earned_unix_ms ? kyty::gtk::TrophyEarnedText(*trophy.earned_unix_ms) : "";
		GtkWidget* label = NewLabel(date.empty() ? _("Earned") : date, "dim-label", false);
		gtk_label_set_xalign(GTK_LABEL(label), 1);
		gtk_widget_add_css_class(label, "trophy-earned");
		gtk_box_append(GTK_BOX(side), label);
	}
	if (trophy.masked) {
		GtkWidget* reveal = gtk_button_new_with_mnemonic(_("_Reveal"));
		gtk_widget_set_valign(reveal, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(reveal,
		                            _("Show the name and description of this hidden trophy"));
		g_object_set_data(G_OBJECT(reveal), kRowIndexKey, GSIZE_TO_POINTER(index + 1));
		page.row_signals.push_back(
		    SignalScope::Connect(reveal, "clicked", G_CALLBACK(&OnRevealClicked), self));
		gtk_box_append(GTK_BOX(side), reveal);
	}
	gtk_box_append(GTK_BOX(box), side);

	std::string accessible;
	if (trophy.masked) {
		// TRANSLATORS: a hidden trophy's row for screen readers; %s is "Hidden trophy".
		accessible = Format(C_("trophy row", "%s, not earned"), name.c_str());
	} else if (trophy.unlocked) {
		// TRANSLATORS: a trophy row for screen readers: "<name>, <grade>, earned".
		accessible = Format(C_("trophy row", "%s, %s, earned"), name.c_str(),
		                    kyty::gtk::TrophyGradeLabel(trophy.grade));
	} else {
		// TRANSLATORS: a trophy row for screen readers: "<name>, <grade>, not earned".
		accessible = Format(C_("trophy row", "%s, %s, not earned"), name.c_str(),
		                    kyty::gtk::TrophyGradeLabel(trophy.grade));
	}
	gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               accessible.c_str(), -1);
	return row;
}

// The rows of the package as BuildTrophyViewer gives them with the page's revealed trophies.
void FillRows(KytyTrophyWindow* self, TrophyPage& page) {
	page.rows =
	    core::BuildTrophyViewer(page.loaded.package, page.loaded.unlocks, page.revealed).trophies;
	page.row_signals.clear();
	gtk_list_box_remove_all(page.list);
	for (std::size_t i = 0; i < page.rows.size(); i++) {
		gtk_list_box_append(page.list, NewTrophyRow(self, page, i));
	}
}

// ---- Pages

GtkWidget* NewStat(const char* name, const std::string& value) {
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), NewLabel(name, "dim-label", false));
	gtk_box_append(GTK_BOX(box), NewLabel(value, "title-3", false));
	return box;
}

void AddPage(KytyTrophyWindow* self, std::size_t index) {
	auto&                    state = *self->state;
	TrophyPage&              page  = *state.pages[index];
	const core::TrophyViewer viewer =
	    core::BuildTrophyViewer(page.loaded.package, page.loaded.unlocks, page.revealed);
	const std::string title = viewer.title.empty() ? viewer.file_name : viewer.title;

	GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(content, 18);
	gtk_widget_set_margin_start(content, 18);
	gtk_widget_set_margin_end(content, 18);

	GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	GtkWidget* art    = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(art), kGameArtSize);
	if (state.art) {
		gtk_image_set_from_paintable(GTK_IMAGE(art), GDK_PAINTABLE(state.art.get()));
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(art), "applications-games-symbolic");
		gtk_widget_add_css_class(art, "dim-label");
	}
	gtk_accessible_update_state(GTK_ACCESSIBLE(art), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	gtk_box_append(GTK_BOX(header), art);
	GtkWidget* names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(names, TRUE);
	gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(names), NewLabel(title, "title-2", true));
	gtk_box_append(GTK_BOX(names),
	               // TRANSLATORS: the number of trophies of a trophy package.
	               NewLabel(Format(_("All trophies: %u"), viewer.total), "dim-label", false));
	gtk_box_append(GTK_BOX(header), names);
	gtk_box_append(GTK_BOX(header),
	               NewStat(_("Progress"), kyty::gtk::TrophyPercentText(viewer.percent)));
	// TRANSLATORS: the earned trophies of all trophies: "<earned>/<total>".
	const std::string earned = Format(C_("trophy count", "%u/%u"), viewer.earned, viewer.total);
	gtk_box_append(GTK_BOX(header), NewStat(_("Earned"), earned));
	// The earned count of each grade the package has (the Qt launcher's header).
	for (const auto grade: kyty::gtk::kTrophyGrades) {
		const std::size_t g = core::TrophyGradeIndex(grade);
		if (viewer.total_by_grade[g] > 0) {
			gtk_box_append(GTK_BOX(header), kyty::gtk::NewTrophyGradeCount(
			                                    grade, viewer.earned_by_grade[g], kGradeIconSize));
		}
	}
	gtk_box_append(GTK_BOX(content), header);

	GtkWidget* progress = gtk_progress_bar_new();
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), viewer.percent / 100.0);
	const std::string progress_text = kyty::gtk::TrophyPercentText(viewer.percent);
	gtk_widget_set_tooltip_text(progress, progress_text.c_str());
	gtk_accessible_update_property(GTK_ACCESSIBLE(progress), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               _("Progress"), -1);
	gtk_box_append(GTK_BOX(content), progress);

	GtkWidget* scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scrolled, TRUE);
	GtkWidget* list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	gtk_widget_set_margin_bottom(list, 18);
	gtk_widget_set_valign(list, GTK_ALIGN_START);
	gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               title.c_str(), -1);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
	gtk_box_append(GTK_BOX(content), scrolled);
	page.list = GTK_LIST_BOX(list);
	state.signals.push_back(
	    SignalScope::Connect(list, "row-activated", G_CALLBACK(&OnRowActivated), self));
	FillRows(self, page);

	adw_view_stack_add_titled_with_icon(self->packages, content,
	                                    ("package-" + std::to_string(index)).c_str(), title.c_str(),
	                                    "kyty-trophy-symbolic");
}

void Apply(KytyTrophyWindow* self, TrophyLoad& load) {
	auto& state  = *self->state;
	state.loaded = true;
	state.art    = std::move(load.art);
	gtk_spinner_stop(self->spinner);
	const auto& trophies = load.trophies;
	g_debug("trophies of %s: %zu package file(s), %zu readable, %zu unreadable",
	        state.entry.path.c_str(), trophies.package_files, trophies.packages.size(),
	        load.unreadable.size());
	if (trophies.packages.empty() && load.unreadable.empty()) {
		gtk_stack_set_visible_child_name(self->content_stack, "empty");
		return;
	}
	const std::string errors = UnreadableText(load.unreadable);
	if (trophies.packages.empty()) {
		// The status page's description is markup.
		const GCharPtr markup(g_markup_escape_text(errors.c_str(), -1));
		adw_status_page_set_description(self->error_page, markup.get());
		gtk_stack_set_visible_child_name(self->content_stack, "error");
		return;
	}
	gtk_label_set_text(self->errors_label, errors.c_str());
	gtk_widget_set_visible(self->errors_box, errors.empty() ? FALSE : TRUE);
	for (std::size_t i = 0; i < trophies.packages.size(); i++) {
		auto page    = std::make_unique<TrophyPage>();
		page->loaded = trophies.packages[i];
		if (i < load.icons.size()) {
			page->icons = std::move(load.icons[i]);
		}
		state.pages.push_back(std::move(page));
		AddPage(self, i);
	}
	gtk_widget_set_visible(self->switcher, state.pages.size() > 1 ? TRUE : FALSE);
	gtk_stack_set_visible_child_name(self->content_stack, "trophies");
}

} // namespace

static void kyty_trophy_window_init(KytyTrophyWindow* self) {
	self->state = new kyty::gtk::TrophyWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_trophy_window_dispose(GObject* object) {
	auto* self = KYTY_TROPHY_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		*state->alive   = false;
		state->signals.clear();
		for (const auto& page: state->pages) {
			page->row_signals.clear();
		}
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_TROPHY_WINDOW);
	G_OBJECT_CLASS(kyty_trophy_window_parent_class)->dispose(object);
}

static void kyty_trophy_window_finalize(GObject* object) {
	auto* self = KYTY_TROPHY_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_trophy_window_parent_class)->finalize(object);
}

static void kyty_trophy_window_class_init(KytyTrophyWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_trophy_window_dispose;
	object_class->finalize = kyty_trophy_window_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, window_title);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, content_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, spinner);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, error_page);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, errors_box);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, errors_label);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, switcher);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyWindow, packages);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyTrophyWindow* kyty_trophy_window_new(kyty::gtk::LauncherContext& context, GtkWindow* parent,
                                         const kyty::core::GameEntry& entry) {
	auto* self    = KYTY_TROPHY_WINDOW(g_object_new(KYTY_TYPE_TROPHY_WINDOW, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	state.entry   = entry;
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	const std::string name = GameName(entry);
	adw_window_title_set_subtitle(self->window_title, name.c_str());
	// TRANSLATORS: the trophy window's title; %s is the game's name.
	gtk_window_set_title(GTK_WINDOW(self), Format(_("Trophies — %s"), name.c_str()).c_str());
	gtk_stack_set_visible_child_name(self->content_stack, "loading");
	gtk_spinner_start(self->spinner);

	// HC-29: the user ID and the console language of the game's effective configuration.
	const auto effective =
	    core::ResolveEffectiveConfiguration(context.settings.loaded.settings, entry, std::nullopt);
	auto load              = std::make_shared<TrophyLoad>();
	load->entry            = entry;
	load->data_dir         = context.paths.data_dir;
	load->user_id          = effective.configuration.user_id;
	load->console_language = effective.configuration.console_language;
	kyty::gtk::RunInBackground([load]() { Read(*load); },
	                           [self, alive = state.alive, load]() {
		                           if (*alive) {
			                           Apply(self, *load);
		                           }
	                           });
	return self;
}

bool kyty_trophy_window_is_loaded(KytyTrophyWindow* window) {
	g_return_val_if_fail(KYTY_IS_TROPHY_WINDOW(window), false);
	return window->state != nullptr && window->state->loaded;
}
