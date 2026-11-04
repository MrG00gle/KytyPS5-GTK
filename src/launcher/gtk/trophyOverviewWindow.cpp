#include "launcher/gtk/trophyOverviewWindow.h"

#include "launcher/core/cancelToken.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/trophyModel.h"
#include "launcher/gtk/backgroundTask.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/trophyPresentation.h"
#include "launcher/gtk/trophyWindow.h"

#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kyty::gtk {

// One game of the library and the configuration its trophy data is read with.
struct OverviewGame {
	core::GameEntry entry;
	int             user_id          = 0;
	int             console_language = 0;
};

// What the GTask thread reads. The window cancels it when it goes away; the read stops before the
// next game (p6-u9-1).
struct OverviewLoad {
	std::filesystem::path     data_dir;
	std::vector<OverviewGame> games;
	core::TrophyOverview      overview;
	core::CancelToken         cancel;
	std::size_t               games_read = 0;
};

struct TrophyOverviewWindowState {
	LauncherContext*                    context = nullptr;
	std::vector<core::GameEntry>        entries; // the library's games, in scan order
	std::vector<GObjectPtr<GdkTexture>> icons;   // their icons as the library had them
	core::TrophyOverview                overview;
	std::shared_ptr<OverviewLoad>       load; // the running read, until its result arrives
	// The rows' columns line up whether or not a game has a Platinum trophy (p6-u9-5): the
	// progress block and each grade's count have a size group over every row.
	GObjectPtr<GtkSizeGroup>              progress_group;
	std::vector<GObjectPtr<GtkSizeGroup>> grade_groups;
	bool                                  loaded   = false;
	bool                                  disposed = false;
	// False from dispose on: the background read's result is dropped.
	std::shared_ptr<bool>    alive = std::make_shared<bool>(true);
	std::vector<SignalScope> signals;
};

} // namespace kyty::gtk

struct _KytyTrophyOverviewWindow {
	AdwWindow   parent_instance;
	GtkStack*   content_stack;
	GtkSpinner* spinner;
	GtkBox*     totals_box;
	GtkLabel*   total_label;
	GtkListBox* games_list;

	kyty::gtk::TrophyOverviewWindowState* state;
};

G_DEFINE_FINAL_TYPE(KytyTrophyOverviewWindow, kyty_trophy_overview_window, ADW_TYPE_WINDOW)

namespace {

namespace core = kyty::core;
using kyty::gtk::GCharPtr;
using kyty::gtk::GObjectPtr;
using kyty::gtk::OverviewLoad;
using kyty::gtk::SignalScope;

constexpr const char* kTemplateResource =
    "/io/github/MrG00gle/KytyPS5_GTK/ui/trophy-overview-window.xml";

constexpr int kArtSize            = 64;
constexpr int kRowGradeIconSize   = 24;
constexpr int kTotalGradeIconSize = 32;
constexpr int kProgressWidth      = 130; // the Qt launcher's

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);
std::string Format(const char* format, ...) {
	va_list arguments;
	va_start(arguments, format);
	const GCharPtr text(g_strdup_vprintf(format, arguments));
	va_end(arguments);
	return std::string(text.View());
}

bool Alive(KytyTrophyOverviewWindow* self) {
	return self->state != nullptr && !self->state->disposed;
}

// The GTask thread's work: every game's packages and unlock data, then the overview. Only what
// BuildTrophyOverview counts is kept per game (the grades and the unlocked IDs: no icon, name,
// description, or reward text), and a cancelled read stops before the next game (p6-u9-1).
void Read(OverviewLoad& load) {
	std::vector<core::TrophyOverviewEntry> entries;
	entries.reserve(load.games.size());
	for (const auto& game: load.games) {
		if (load.cancel.IsCancelled()) {
			return;
		}
		auto trophies =
		    core::LoadGameTrophies(game.entry, load.data_dir, game.user_id, game.console_language);
		for (auto& package: trophies.packages) {
			for (auto& trophy: package.package.trophies) {
				// swap, not clear(): the memory itself goes
				std::string().swap(trophy.name);
				std::string().swap(trophy.description);
				std::string().swap(trophy.reward);
				std::string().swap(trophy.icon_png);
			}
			package.unlocks.timestamps.clear();
		}
		entries.push_back(
		    {game.entry.title_name, game.entry.title_id, std::move(trophies.packages)});
		load.games_read++;
	}
	load.overview = core::BuildTrophyOverview(entries);
}

GtkWidget* NewLabel(const std::string& text, const char* css_class, float xalign) {
	GtkWidget* label = gtk_label_new(text.c_str());
	gtk_label_set_xalign(GTK_LABEL(label), xalign);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	if (css_class != nullptr) {
		gtk_widget_add_css_class(label, css_class);
	}
	return label;
}

GtkWidget* NewGameRow(KytyTrophyOverviewWindow* self, const core::TrophyOverviewRow& game) {
	auto&      state = *self->state;
	GtkWidget* row   = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

	GtkWidget* art = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(art), kArtSize);
	GdkTexture* icon =
	    game.entry_index < state.icons.size() ? state.icons[game.entry_index].get() : nullptr;
	if (icon != nullptr) {
		gtk_image_set_from_paintable(GTK_IMAGE(art), GDK_PAINTABLE(icon));
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(art), "applications-games-symbolic");
		gtk_widget_add_css_class(art, "dim-label");
	}
	gtk_accessible_update_state(GTK_ACCESSIBLE(art), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	gtk_box_append(GTK_BOX(box), art);

	GtkWidget* names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(names, TRUE);
	gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(names), NewLabel(game.name, "heading", 0));
	gtk_box_append(GTK_BOX(names), NewLabel(game.title_id, "dim-label", 0));
	gtk_box_append(GTK_BOX(box), names);

	const std::string percent = kyty::gtk::TrophyPercentText(game.percent);
	// TRANSLATORS: the earned trophies of all trophies of a game: "Earned <earned>/<total>".
	const std::string earned =
	    Format(C_("trophy overview column", "Earned %u/%u"), game.earned, game.total);
	GtkWidget*        progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(progress_box, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(progress_box), NewLabel(percent, "title-2", 1));
	gtk_box_append(GTK_BOX(progress_box), NewLabel(earned, "dim-label", 1));
	GtkWidget* progress = gtk_progress_bar_new();
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), game.percent / 100.0);
	gtk_widget_set_size_request(progress, kProgressWidth, -1);
	gtk_accessible_update_state(GTK_ACCESSIBLE(progress), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	gtk_box_append(GTK_BOX(progress_box), progress);
	gtk_box_append(GTK_BOX(box), progress_box);
	gtk_size_group_add_widget(state.progress_group.get(), progress_box);

	// Platinum only when the game has a platinum trophy (the Qt launcher's rule); without one, an
	// empty box keeps its column, so the columns of every row line up (p6-u9-5).
	for (const auto grade: kyty::gtk::kTrophyGrades) {
		const std::size_t g = core::TrophyGradeIndex(grade);
		GtkWidget*        count =
		    grade == core::TrophyGrade::Platinum && game.total_by_grade[g] == 0
		        ? gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)
		        : kyty::gtk::NewTrophyGradeCount(grade, game.earned_by_grade[g], kRowGradeIconSize);
		gtk_widget_set_valign(count, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), count);
		gtk_size_group_add_widget(state.grade_groups[g].get(), count);
	}

	const std::string accessible =
	    // TRANSLATORS: a game row of the trophy overview for screen readers: "<game>, <percent>,
	    // Earned <earned>/<total>".
	    Format(C_("trophy overview row", "%s, %s, %s"), game.name.c_str(), percent.c_str(),
	           earned.c_str());
	gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
	                               accessible.c_str(), -1);
	gtk_widget_set_tooltip_text(row, _("Open the trophies of this game"));
	return row;
}

void OnRowActivated(GtkListBox* /*list*/, GtkListBoxRow* row, gpointer data) {
	auto* self = KYTY_TROPHY_OVERVIEW_WINDOW(data);
	if (!Alive(self)) {
		return;
	}
	auto&     state = *self->state;
	const int index = gtk_list_box_row_get_index(row);
	if (index < 0 || static_cast<std::size_t>(index) >= state.overview.rows.size()) {
		return;
	}
	const std::size_t entry = state.overview.rows[static_cast<std::size_t>(index)].entry_index;
	if (entry < state.entries.size()) {
		gtk_window_present(GTK_WINDOW(
		    kyty_trophy_window_new(*state.context, GTK_WINDOW(self), state.entries[entry])));
	}
}

void Apply(KytyTrophyOverviewWindow* self, OverviewLoad& load) {
	auto& state    = *self->state;
	state.loaded   = true;
	state.overview = std::move(load.overview);
	state.load.reset();
	gtk_spinner_stop(self->spinner);
	g_debug("trophy overview: %zu game(s), %zu with trophy data", state.entries.size(),
	        state.overview.rows.size());
	if (state.overview.rows.empty()) {
		gtk_stack_set_visible_child_name(self->content_stack, "empty");
		return;
	}
	// TRANSLATORS: the earned trophies of every game: "Total <number>".
	gtk_label_set_text(self->total_label, Format(_("Total %u"), state.overview.earned).c_str());
	for (const auto grade: kyty::gtk::kTrophyGrades) {
		gtk_box_append(self->totals_box,
		               kyty::gtk::NewTrophyGradeCount(
		                   grade, state.overview.earned_by_grade[core::TrophyGradeIndex(grade)],
		                   kTotalGradeIconSize));
	}
	for (const auto& game: state.overview.rows) {
		gtk_list_box_append(self->games_list, NewGameRow(self, game));
	}
	gtk_stack_set_visible_child_name(self->content_stack, "games");
}

} // namespace

static void kyty_trophy_overview_window_init(KytyTrophyOverviewWindow* self) {
	self->state = new kyty::gtk::TrophyOverviewWindowState;
	gtk_widget_init_template(GTK_WIDGET(self));
}

static void kyty_trophy_overview_window_dispose(GObject* object) {
	auto* self = KYTY_TROPHY_OVERVIEW_WINDOW(object);
	if (auto* state = self->state; state != nullptr && !state->disposed) {
		state->disposed = true;
		*state->alive   = false;
		// A read that still runs stops before its next game.
		if (state->load) {
			state->load->cancel.Cancel();
			state->load.reset();
		}
		state->signals.clear();
	}
	gtk_widget_dispose_template(GTK_WIDGET(self), KYTY_TYPE_TROPHY_OVERVIEW_WINDOW);
	G_OBJECT_CLASS(kyty_trophy_overview_window_parent_class)->dispose(object);
}

static void kyty_trophy_overview_window_finalize(GObject* object) {
	auto* self = KYTY_TROPHY_OVERVIEW_WINDOW(object);
	delete self->state;
	self->state = nullptr;
	G_OBJECT_CLASS(kyty_trophy_overview_window_parent_class)->finalize(object);
}

static void kyty_trophy_overview_window_class_init(KytyTrophyOverviewWindowClass* klass) {
	auto* object_class     = G_OBJECT_CLASS(klass);
	auto* widget_class     = GTK_WIDGET_CLASS(klass);
	object_class->dispose  = kyty_trophy_overview_window_dispose;
	object_class->finalize = kyty_trophy_overview_window_finalize;
	gtk_widget_class_set_template_from_resource(widget_class, kTemplateResource);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyOverviewWindow, content_stack);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyOverviewWindow, spinner);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyOverviewWindow, totals_box);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyOverviewWindow, total_label);
	gtk_widget_class_bind_template_child(widget_class, KytyTrophyOverviewWindow, games_list);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_Escape, GdkModifierType(0),
	                                    "window.close", nullptr);
}

KytyTrophyOverviewWindow* kyty_trophy_overview_window_new(kyty::gtk::LauncherContext& context,
                                                          GtkWindow*                  parent) {
	auto* self =
	    KYTY_TROPHY_OVERVIEW_WINDOW(g_object_new(KYTY_TYPE_TROPHY_OVERVIEW_WINDOW, nullptr));
	auto& state   = *self->state;
	state.context = &context;
	if (parent != nullptr) {
		gtk_window_set_transient_for(GTK_WINDOW(self), parent);
	}
	gtk_stack_set_visible_child_name(self->content_stack, "loading");
	gtk_spinner_start(self->spinner);
	state.signals.push_back(
	    SignalScope::Connect(self->games_list, "row-activated", G_CALLBACK(&OnRowActivated), self));
	state.progress_group =
	    GObjectPtr<GtkSizeGroup>::Adopt(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
	for (std::size_t i = 0; i < std::size(kyty::gtk::kTrophyGrades); i++) {
		state.grade_groups.push_back(
		    GObjectPtr<GtkSizeGroup>::Adopt(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)));
	}

	auto load         = std::make_shared<OverviewLoad>();
	load->data_dir    = context.paths.data_dir;
	GListModel* games = context.library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		const core::GameEntry& entry = kyty_game_get_entry(game.get());
		// HC-29: the user ID and the console language of each game's effective configuration.
		const auto effective = core::ResolveEffectiveConfiguration(context.settings.loaded.settings,
		                                                           entry, std::nullopt);
		load->games.push_back(
		    {entry, effective.configuration.user_id, effective.configuration.console_language});
		state.entries.push_back(entry);
		state.icons.push_back(GObjectPtr<GdkTexture>::Ref(kyty_game_get_texture(game.get())));
	}
	state.load = load;
	// An unowned GTask, a recorded exception (p6-u9-6): the alive flag drops a late result.
	kyty::gtk::RunInBackground([load]() { Read(*load); },
	                           [self, alive = state.alive, load]() {
		                           g_debug("trophy overview: read %zu of %zu game(s)%s",
		                                   load->games_read, load->games.size(),
		                                   load->cancel.IsCancelled() ? ", cancelled" : "");
		                           if (*alive) {
			                           Apply(self, *load);
		                           }
	                           });
	return self;
}

bool kyty_trophy_overview_window_is_loaded(KytyTrophyOverviewWindow* window) {
	g_return_val_if_fail(KYTY_IS_TROPHY_OVERVIEW_WINDOW(window), false);
	return window->state != nullptr && window->state->loaded;
}
