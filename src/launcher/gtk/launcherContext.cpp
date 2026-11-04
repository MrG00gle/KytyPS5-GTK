#include "launcher/gtk/launcherContext.h"

#include "launcher/core/cancelToken.h"
#include "launcher/gtk/gobjectPtr.h"
#include "updater/core/buildInfo.h"

#include <algorithm>
#include <glib.h>
#include <system_error>
#include <utility>
#include <vector>

namespace kyty::gtk {

// ---- BackgroundJobs

struct BackgroundJobs::State {
	struct Entry {
		std::uint64_t                      id = 0;
		std::shared_ptr<core::CancelToken> cancel;
		std::string                        what;
	};
	std::vector<Entry> jobs;
	std::uint64_t      next    = 1;
	bool               waiting = false;
};

void BackgroundJobs::Job::Finished() {
	const auto state = m_state.lock();
	if (!state || m_id == 0) {
		return;
	}
	auto&      jobs = state->jobs;
	const auto found =
	    std::find_if(jobs.begin(), jobs.end(), [this](const auto& job) { return job.id == m_id; });
	if (found != jobs.end()) {
		g_debug("background job finished: %s", found->what.c_str());
		jobs.erase(found);
	}
	m_id = 0;
}

BackgroundJobs::BackgroundJobs(): m_state(std::make_shared<State>()) {}

BackgroundJobs::~BackgroundJobs() {
	// A job that still runs stops at its next cancellation point; its result finds no registry.
	for (const auto& job: m_state->jobs) {
		job.cancel->Cancel();
	}
}

BackgroundJobs::Job BackgroundJobs::Register(std::shared_ptr<core::CancelToken> cancel,
                                             std::string                        what) {
	Job job;
	job.m_state = m_state;
	job.m_id    = m_state->next++;
	m_state->jobs.push_back({job.m_id, std::move(cancel), std::move(what)});
	return job;
}

std::size_t BackgroundJobs::Count() const {
	return m_state->jobs.size();
}

bool BackgroundJobs::CancelAndWait(std::chrono::milliseconds timeout) {
	const auto state = m_state; // the jobs' results may run while this waits
	if (state->jobs.empty()) {
		return true;
	}
	for (const auto& job: state->jobs) {
		g_debug("background job cancelled: %s", job.what.c_str());
		job.cancel->Cancel();
	}
	if (state->waiting) {
		return state->jobs.empty();
	}
	state->waiting           = true;
	const gint64 started     = g_get_monotonic_time();
	bool         out_of_time = false;
	// Wakes the iteration below when the time is up.
	const SourceId deadline(g_timeout_add(
	    static_cast<guint>(timeout.count()),
	    [](gpointer data) -> gboolean {
		    *static_cast<bool*>(data) = true;
		    return G_SOURCE_REMOVE;
	    },
	    &out_of_time));
	while (!state->jobs.empty() && !out_of_time) {
		g_main_context_iteration(nullptr, TRUE);
	}
	state->waiting       = false;
	const auto waited_ms = (g_get_monotonic_time() - started) / 1000;
	if (!state->jobs.empty()) {
		for (const auto& job: state->jobs) {
			g_message("background job still running after %lld ms: %s",
			          static_cast<long long>(waited_ms), job.what.c_str());
		}
		return false;
	}
	g_debug("background jobs stopped after %lld ms", static_cast<long long>(waited_ms));
	return true;
}

// ---- The context

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
