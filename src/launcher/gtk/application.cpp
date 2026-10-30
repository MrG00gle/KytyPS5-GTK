#include "launcher/gtk/application.h"

#include "launcher/core/environment.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/installation.h"
#include "updater/core/buildInfo.h"

#include <csignal>
#include <glib-unix.h>
#include <string>
#include <system_error>

namespace kyty::gtk {

// The C++ members of a KytyApplication.
struct KytyApplicationState {
	std::unique_ptr<LauncherContext>      context;
	std::unique_ptr<EmulatorVersionProbe> version_probe;
	bool                                  local = false;
};

} // namespace kyty::gtk

struct _KytyApplication {
	AdwApplication                   parent_instance;
	kyty::gtk::KytyApplicationState* state;
};

G_DEFINE_FINAL_TYPE(KytyApplication, kyty_application, ADW_TYPE_APPLICATION)

namespace kyty::gtk {
namespace {

// The application between startup and shutdown, for the signal handlers.
KytyApplication* running_application = nullptr;
// A quit signal arrived before startup.
bool quit_requested = false;

gboolean OnQuitSignal(gpointer data) {
	g_debug("received signal %d, quitting", GPOINTER_TO_INT(data));
	if (running_application != nullptr) {
		g_action_group_activate_action(G_ACTION_GROUP(running_application), "quit", nullptr);
	} else {
		quit_requested = true;
	}
	return G_SOURCE_CONTINUE;
}

void OnQuitAction(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer data) {
	g_application_quit(G_APPLICATION(data));
}

// GtkApplication adds <resource base path>/icons by itself; adding it again would only duplicate
// the search path entry.
void AddIconResourcePath() {
	GdkDisplay* display = gdk_display_get_default();
	if (display == nullptr) {
		return;
	}
	GtkIconTheme*     theme = gtk_icon_theme_get_for_display(display);
	const std::string path  = std::string(kResourceBasePath) + "/icons";
	gchar**           paths = gtk_icon_theme_get_resource_path(theme);
	const bool        known = paths != nullptr && g_strv_contains(paths, path.c_str()) != FALSE;
	g_strfreev(paths);
	if (!known) {
		gtk_icon_theme_add_resource_path(theme, path.c_str());
	}
}

void StartVersionProbe(KytyApplication* self) {
	auto* state = self->state;
	if (!state->context->emulator.found) {
		return;
	}
	state->version_probe = EmulatorVersionProbe::Start(
	    state->context->emulator.executable, [self](std::string version) {
		    auto* probe_state                      = self->state;
		    probe_state->context->emulator_version = std::move(version);
		    g_debug("emulator version: %s", probe_state->context->emulator_version.c_str());
		    probe_state->version_probe.reset();
	    });
}

} // namespace

LauncherContext* ApplicationContext(KytyApplication* application) {
	g_return_val_if_fail(KYTY_IS_APPLICATION(application), nullptr);
	return application->state->context.get();
}

void InstallQuitSignalHandlers() {
	struct QuitSignalSources {
		SourceId interrupt;
		SourceId terminate;
	};
	// Never destroyed: removing the last GLib handler restores the default action, and a SIGTERM
	// during the final teardown would then end the process with signal status instead of 0.
	static auto* sources = new QuitSignalSources {
	    SourceId(g_unix_signal_add(SIGINT, OnQuitSignal, GINT_TO_POINTER(SIGINT))),
	    SourceId(g_unix_signal_add(SIGTERM, OnQuitSignal, GINT_TO_POINTER(SIGTERM)))};
	static_cast<void>(sources);
}

} // namespace kyty::gtk

static gint kyty_application_handle_local_options(GApplication* application,
                                                  GVariantDict* options) {
	KYTY_APPLICATION(application)->state->local =
	    g_variant_dict_contains(options, "local") != FALSE;
	return -1; // continue with the default processing
}

static void kyty_application_startup(GApplication* application) {
	G_APPLICATION_CLASS(kyty_application_parent_class)->startup(application);
	auto* self  = KYTY_APPLICATION(application);
	auto* state = self->state;

	std::error_code ec;
	auto            working_directory = std::filesystem::current_path(ec);
	if (ec) {
		g_message("cannot read the working directory: %s", ec.message().c_str());
		working_directory = "/";
	}
	const auto environment = kyty::core::MakePosixEnvironment();
	state->context         = kyty::gtk::CreateLauncherContext(
	    *environment, {.working_directory  = working_directory,
	                   .launcher_directory = kyty::gtk::LauncherDirectory(),
	                   .update_mode        = kyty::updater::CurrentUpdateUiMode(),
	                   .local              = state->local});

	gtk_window_set_default_icon_name(kyty::gtk::kApplicationId);
	kyty::gtk::AddIconResourcePath();
	adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_DEFAULT);

	static const GActionEntry kActions[] = {
	    {"quit", kyty::gtk::OnQuitAction, nullptr, nullptr, nullptr, {0, 0, 0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), kActions, G_N_ELEMENTS(kActions),
	                                application);
	static const char* const kQuitAccels[] = {"<Control>q", nullptr};
	gtk_application_set_accels_for_action(GTK_APPLICATION(application), "app.quit", kQuitAccels);

	kyty::gtk::StartVersionProbe(self);
	kyty::gtk::running_application = self;
	g_debug("startup complete");
	if (kyty::gtk::quit_requested) {
		g_application_quit(application);
	}
}

static void kyty_application_activate(GApplication* /*application*/) {
	// The main window (task 20.3) is created here; until then nothing opens and the process ends.
}

static void kyty_application_shutdown(GApplication* application) {
	auto* self                     = KYTY_APPLICATION(application);
	kyty::gtk::running_application = nullptr;
	self->state->version_probe.reset();
	G_APPLICATION_CLASS(kyty_application_parent_class)->shutdown(application);
}

static void kyty_application_finalize(GObject* object) {
	delete KYTY_APPLICATION(object)->state;
	G_OBJECT_CLASS(kyty_application_parent_class)->finalize(object);
}

static void kyty_application_class_init(KytyApplicationClass* klass) {
	auto* object_class                      = G_OBJECT_CLASS(klass);
	auto* application_class                 = G_APPLICATION_CLASS(klass);
	object_class->finalize                  = kyty_application_finalize;
	application_class->handle_local_options = kyty_application_handle_local_options;
	application_class->startup              = kyty_application_startup;
	application_class->activate             = kyty_application_activate;
	application_class->shutdown             = kyty_application_shutdown;
}

static void kyty_application_init(KytyApplication* self) {
	self->state = new kyty::gtk::KytyApplicationState;
	g_application_add_main_option(
	    G_APPLICATION(self), "local", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
	    _("Use compatibility_db.json in the working directory as an editable "
	      "compatibility list"),
	    nullptr);
}

KytyApplication* kyty_application_new(void) {
	return KYTY_APPLICATION(g_object_new(KYTY_TYPE_APPLICATION, "application-id",
	                                     kyty::gtk::kApplicationId, "flags",
	                                     G_APPLICATION_NON_UNIQUE, nullptr));
}
