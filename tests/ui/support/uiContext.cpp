#include "ui/support/uiContext.h"

#include "launcher/core/compatibility.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/gobjectPtr.h"
#include "support/helperPaths.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <string>
#include <system_error>
#include <utility>

namespace KytyUi {

namespace fs = std::filesystem;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SourceId;

namespace {

// What a KytyApplication has without being asked: the launcher's style sheet (libadwaita loads it
// from the application's resource base path) and its icons. Once per process.
void UseLauncherStyle() {
	static bool done = false;
	if (done) {
		return;
	}
	done                = true;
	GdkDisplay* display = gdk_display_get_default();
	const auto  base    = std::string(kyty::gtk::kResourceBasePath);
	gtk_window_set_default_icon_name(kyty::gtk::kApplicationId);
	gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display),
	                                 (base + "/icons").c_str());
	auto css = GObjectPtr<GtkCssProvider>::Adopt(gtk_css_provider_new());
	gtk_css_provider_load_from_resource(css.get(), (base + "/style.css").c_str());
	gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css.get()),
	                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

KytyMainWindow* MainWindowOf(GtkApplication* application) {
	for (GList* item = gtk_application_get_windows(application); item != nullptr;
	     item        = item->next) {
		if (KYTY_IS_MAIN_WINDOW(item->data)) {
			return KYTY_MAIN_WINDOW(item->data);
		}
	}
	return nullptr;
}

struct ApplicationRun {
	std::function<void(KytyApplication*, KytyMainWindow*)> drive;
	bool                                                   driven          = false;
	int                                                    quit_timeout_ms = 10000;
	SourceId                                               watchdog;
};

void OnActivated(GApplication* application, gpointer data) {
	auto* run = static_cast<ApplicationRun*>(data);
	if (run->driven) {
		return;
	}
	run->driven            = true;
	KytyMainWindow* window = WaitForMainWindow(GTK_APPLICATION(application));
	if (window == nullptr) {
		g_application_quit(application);
		return;
	}
	run->drive(KYTY_APPLICATION(application), window);
	run->watchdog = SourceId(g_timeout_add(
	    static_cast<guint>(run->quit_timeout_ms),
	    [](gpointer target) -> gboolean {
		    Fail("the application did not quit within the case's time after the case");
		    g_application_quit(G_APPLICATION(target));
		    return G_SOURCE_REMOVE;
	    },
	    application));
}

} // namespace

UiContext::UiContext()
    : m_tree("kyty_ui"), m_server(std::make_unique<KytyTests::LoopbackServer>()),
      m_http(std::make_unique<KytyTests::LoopbackHttpClient>(*m_server)),
      m_services(std::make_unique<KytyTests::TestServices>(*m_http)) {
	for (const char* directory: {"app", "cwd", "config", "data", "home"}) {
		m_tree.MakeDirectories(directory);
	}
	auto& environment = m_services->environment;
	environment.Set("HOME", (Root() / "home").string());
	environment.Set("XDG_CONFIG_HOME", (Root() / "config").string());
	environment.Set("XDG_DATA_HOME", (Root() / "data").string());

	std::error_code ec;
	const fs::path  emulator = AppDirectory() / "kyty_emulator";
	fs::copy_file(fs::path(KYTY_TEST_SOURCE_DIR) / "tests" / "ui" / "fake-kyty-emulator.sh",
	              emulator, fs::copy_options::overwrite_existing, ec);
	Check(!ec, "copy the fake Emulator to " + emulator.string() + ": " + ec.message());
	fs::permissions(emulator,
	                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
	                    fs::perms::others_read | fs::perms::others_exec,
	                fs::perm_options::replace, ec);
	Check(!ec, "make " + emulator.string() + " executable: " + ec.message());
	g_setenv("KYTY_UI_FAKE_EMULATOR", KytyTests::HelperPath("fake_emulator").c_str(), TRUE);
	g_setenv("KYTY_UI_ARGS_FILE", (Root() / "emulator-args").c_str(), TRUE);
}

UiContext::~UiContext() {
	if (m_parent != nullptr && IsToplevel(m_parent)) {
		gtk_window_destroy(m_parent);
	}
	m_parent = nullptr;
	// Idles of the windows that are gone may still use the context and the services.
	Pump(50);
	m_context.reset();
	Pump(50);
}

fs::path UiContext::SettingsFile() const {
	return Root() / "config" / "Kyty" / "Kyty.ini";
}

fs::path UiContext::WriteFixtureGames() {
	return WriteFixtures(Root() / "fixtures");
}

bool UiContext::WriteGameContent(const fs::path& games) {
	if (!Check(!m_context, "the game content is written before the context reads Kyty.ini")) {
		return false;
	}
	const fs::path addons = Root() / "addons";
	auto           store  = kyty::core::OpenSettingsStore(SettingsFile());
	auto           next   = store.loaded.settings;
	next.addons_dir       = addons.string();
	const auto written    = kyty::core::SaveSettingsStore(store, next);
	if (!Check(static_cast<bool>(written), "write " + SettingsFile().string() +
	                                           (written ? "" : ": " + written.error().message))) {
		return false;
	}
	for (auto& [url, body]: CheatIndexes()) {
		KytyTests::LoopbackResponse response;
		response.body         = std::move(body);
		response.content_type = "text/plain";
		m_server->Script(url, std::move(response));
	}
	return KytyUi::WriteGameContent(games, AppDirectory(), addons, kyty::core::kDefaultUserId);
}

bool UiContext::StoreGameFolders(const std::vector<fs::path>& folders) {
	auto store = kyty::core::OpenSettingsStore(SettingsFile());
	auto next  = store.loaded.settings;
	next.game_folders.clear();
	for (const auto& folder: folders) {
		next.game_folders.push_back(folder.string());
	}
	const auto written = kyty::core::SaveSettingsStore(store, next);
	return Check(static_cast<bool>(written), "write " + SettingsFile().string() +
	                                             (written ? "" : ": " + written.error().message));
}

void UiContext::ServeCompatibility(std::string body) {
	KytyTests::LoopbackResponse response;
	response.body = std::move(body);
	m_server->Script(kyty::core::kCompatibilityUrl, std::move(response));
}

kyty::gtk::LauncherContext& UiContext::Context() {
	if (!m_context) {
		UseLauncherStyle();
		m_context =
		    kyty::gtk::CreateLauncherContext({.working_directory  = WorkingDirectory(),
		                                      .launcher_directory = AppDirectory(),
		                                      .update_mode = kyty::updater::UpdateUiMode::Other,
		                                      .local       = false},
		                                     m_services->Get());
	}
	return *m_context;
}

GtkWindow* UiContext::ParentWindow() {
	if (m_parent == nullptr) {
		UseLauncherStyle();
		m_parent = GTK_WINDOW(adw_window_new());
		gtk_window_set_title(m_parent, "launcher_ui_tests parent");
		gtk_window_set_default_size(m_parent, 900, 600);
		gtk_window_present(m_parent);
		// Mapped is GTK's state; the compositor shows the window with its first painted frame. A
		// dialog opened before that is stacked below it on weston and, fully covered, never gets
		// a frame callback.
		WaitFor([this]() { return gtk_widget_get_mapped(GTK_WIDGET(m_parent)) != FALSE; }, 5000,
		        "the parent window to be mapped") &&
		    WaitForPaintedFrame(GTK_WIDGET(m_parent));
	}
	return m_parent;
}

int UiContext::RunApplication(const std::function<void(KytyApplication*, KytyMainWindow*)>& drive,
                              std::optional<kyty::updater::UpdateUiMode> update_mode) {
	return RunApplication(drive, RunOptions {.update_mode = update_mode});
}

int UiContext::RunApplication(const std::function<void(KytyApplication*, KytyMainWindow*)>& drive,
                              const RunOptions& options) {
	const kyty::gtk::Services             services = m_services->Get();
	const kyty::gtk::ApplicationTestHooks hooks {
	    .services           = options.services != nullptr ? options.services : &services,
	    .launcher_directory = AppDirectory(),
	    .update_mode        = options.update_mode,
	    .release_tag        = options.release_tag};
	auto application = GObjectPtr<KytyApplication>::Adopt(kyty_application_new_for_testing(hooks));
	ApplicationRun run;
	run.drive           = drive;
	run.quit_timeout_ms = options.quit_timeout_ms;
	g_signal_connect_after(application.get(), "activate", G_CALLBACK(&OnActivated), &run);

	std::error_code ec;
	const fs::path  previous = fs::current_path(ec);
	fs::current_path(WorkingDirectory(), ec);
	Check(!ec, "change to " + WorkingDirectory().string() + ": " + ec.message());
	const int status = g_application_run(G_APPLICATION(application.get()), 0, nullptr);
	fs::current_path(previous, ec);
	run.watchdog.Remove();
	application.reset();
	Check(run.driven, "the application was activated and driven");
	return status;
}

KytyMainWindow* WaitForMainWindow(GtkApplication* application, int timeout_ms) {
	KytyMainWindow* window = nullptr;
	const bool      mapped = WaitFor(
	    [&]() {
		    window = MainWindowOf(application);
		    return window != nullptr && gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE;
	    },
	    timeout_ms, "the main window to be mapped");
	// As for ParentWindow(): the compositor shows it with its first painted frame.
	return mapped && WaitForPaintedFrame(GTK_WIDGET(window)) ? window : nullptr;
}

} // namespace KytyUi
