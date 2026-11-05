// The GTK launcher's entry point (task 19.3, Requirements 6.15 and 6.12; plan D10, D16).
//
// Without a display, it prints "kyty_launcher: cannot open display: <reason>" and exits with 1
// before anything reads the Settings_Store. The prefix is the executable's name.

#include "launcher/gtk/application.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/installation.h"

#include <adwaita.h>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <gtk/gtk.h>
#include <libintl.h>
#include <string>

// From the source that glib-compile-resources --manual-register generates (CMakeLists.txt).
extern "C" void kyty_launcher_register_resource(void);

namespace {

// Why gtk_init_check() could not open a display, named by the variables that were set.
std::string NoDisplayReason() {
	const char* wayland = std::getenv("WAYLAND_DISPLAY");
	const char* x11     = std::getenv("DISPLAY");
	std::string targets;
	if (wayland != nullptr && *wayland != '\0') {
		targets = std::string("WAYLAND_DISPLAY=") + wayland;
	}
	if (x11 != nullptr && *x11 != '\0') {
		targets += (targets.empty() ? "" : ", ") + std::string("DISPLAY=") + x11;
	}
	if (targets.empty()) {
		return _("neither WAYLAND_DISPLAY nor DISPLAY is set");
	}
	const kyty::gtk::GCharPtr reason(
	    g_strdup_printf(_("could not connect to %s"), targets.c_str()));
	return std::string(reason.View());
}

} // namespace

int main(int argc, char** argv) {
	// First, so that SIGINT and SIGTERM during startup end the launcher with status 0 as well.
	kyty::gtk::InstallQuitSignalHandlers();

	std::setlocale(LC_ALL, "");
	// No translations are installed yet; the Flatpak locale directory is decided with the package.
	const auto locale_directory = kyty::gtk::LauncherDirectory() / "locale";
	bindtextdomain(GETTEXT_PACKAGE, locale_directory.c_str());
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

	if (gtk_init_check() == FALSE) {
		const kyty::gtk::GCharPtr message(g_strdup_printf(
		    _("kyty_launcher: cannot open display: %s"), NoDisplayReason().c_str()));
		std::fprintf(stderr, "%s\n", message.get());
		return 1;
	}
	adw_init();
	kyty_launcher_register_resource();

	auto application = kyty::gtk::GObjectPtr<KytyApplication>::Adopt(kyty_application_new());
	return g_application_run(G_APPLICATION(application.get()), argc, argv);
}
