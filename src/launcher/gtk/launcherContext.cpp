#include "launcher/gtk/launcherContext.h"

#include <glib.h>
#include <system_error>

namespace kyty::gtk {

std::unique_ptr<LauncherContext> CreateLauncherContext(const core::Environment&      environment,
                                                       const LauncherContextOptions& options) {
	auto context         = std::make_unique<LauncherContext>();
	context->emulator    = LocateEmulator(options.launcher_directory);
	context->paths       = core::ResolvePaths(environment, options.working_directory,
	                                          context->emulator.executable.parent_path());
	context->update_mode = options.update_mode;
	context->local       = options.local;
	if (!context->emulator.found) {
		g_message("the emulator was not found at %s", context->emulator.executable.c_str());
	}
	if (context->paths.flatpak) {
		const auto      directory = core::SettingsDirectory(context->paths);
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		if (ec) {
			g_message("cannot create the settings directory %s: %s", directory.c_str(),
			          ec.message().c_str());
		}
	}
	context->settings = core::OpenSettingsStore(context->paths.settings_file);
	if (context->settings.load_error) {
		g_message("cannot load %s: %s", context->paths.settings_file.c_str(),
		          context->settings.load_error->message.c_str());
	}
	g_debug("settings store %s, data directory %s", context->paths.settings_file.c_str(),
	        context->paths.data_dir.c_str());
	return context;
}

} // namespace kyty::gtk
