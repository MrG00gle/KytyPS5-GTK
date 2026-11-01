#include "launcher/gtk/launcherContext.h"

#include "updater/core/buildInfo.h"

#include <glib.h>
#include <system_error>

namespace kyty::gtk {

std::unique_ptr<LauncherContext> CreateLauncherContext(const LauncherContextOptions& options,
                                                       const Services&               services) {
	auto context         = std::make_unique<LauncherContext>(services);
	context->emulator    = LocateEmulator(options.launcher_directory);
	context->paths       = core::ResolvePaths(services.environment, options.working_directory,
	                                          context->emulator.executable.parent_path());
	context->update_mode = options.update_mode;
	context->local       = options.local;
	// This installation, for the update UI (task 21.6).
	context->launcher_directory = options.launcher_directory;
	context->working_directory  = options.working_directory;
	context->release_tag        = std::string(updater::CurrentBuildFlags().release_tag);
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
	context->settings        = core::OpenSettingsStore(context->paths.settings_file);
	context->stored_settings = context->settings.loaded.settings;
	if (context->settings.load_error) {
		g_message("cannot load %s: %s", context->paths.settings_file.c_str(),
		          context->settings.load_error->message.c_str());
	}
	g_debug("settings store %s, data directory %s", context->paths.settings_file.c_str(),
	        context->paths.data_dir.c_str());
	return context;
}

core::Expected<void, core::IoError> WriteSettingsStore(LauncherContext&              context,
                                                       const core::LauncherSettings& settings) {
	auto written = core::SaveSettingsStore(context.settings, settings);
	if (written) {
		context.stored_settings = settings;
	}
	return written;
}

std::optional<core::IoError> SaveSettingsAutomatically(LauncherContext&       context,
                                                       core::LauncherSettings next) {
	auto& store           = context.settings;
	store.loaded.settings = std::move(next);
	if (store.load_error && !store.overwrite_confirmed) {
		g_debug("not saving %s: it could not be loaded and overwriting it is not allowed",
		        store.path.c_str());
		return std::nullopt;
	}
	auto written = WriteSettingsStore(context, store.loaded.settings);
	if (!written) {
		g_message("cannot save %s: %s", store.path.c_str(), written.error().message.c_str());
		return written.error();
	}
	return std::nullopt;
}

} // namespace kyty::gtk
