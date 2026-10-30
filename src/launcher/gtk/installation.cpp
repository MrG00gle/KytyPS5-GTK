#include "launcher/gtk/installation.h"

#include "launcher/core/text.h"
#include "launcher/gtk/gobjectPtr.h"

#include <gio/gio.h>
#include <system_error>
#include <utility>
#include <vector>

namespace kyty::gtk {

std::filesystem::path LauncherDirectory() {
	std::error_code ec;
	const auto      executable = std::filesystem::read_symlink("/proc/self/exe", ec);
	if (ec || executable.empty()) {
		return {};
	}
	return executable.parent_path();
}

EmulatorLocation LocateEmulator(const std::filesystem::path& launcher_dir) {
	const auto      first  = (launcher_dir / kEmulatorExecutableName).lexically_normal();
	const auto      second = (launcher_dir / ".." / kEmulatorExecutableName).lexically_normal();
	std::error_code ec;
	if (std::filesystem::exists(first, ec)) {
		return {first, true};
	}
	if (std::filesystem::exists(second, ec)) {
		return {second, true};
	}
	return {first, false};
}

std::string EmulatorVersionFromOutput(std::string_view output) {
	std::vector<std::string_view> lines;
	std::size_t                   start = 0;
	while (start < output.size() && lines.size() < 2) {
		std::size_t end = output.find_first_of("\r\n", start);
		if (end == std::string_view::npos) {
			end = output.size();
		}
		if (end > start) {
			lines.push_back(output.substr(start, end - start));
		}
		start = end + 1;
	}
	if (lines.empty()) {
		return {};
	}
	if (lines[0].starts_with("exe_name")) {
		return lines.size() >= 2 ? core::SanitizeUtf8(lines[1]) : std::string();
	}
	return core::SanitizeUtf8(lines[0]);
}

// The pending run. It is deleted by its completion callback, which GLib calls exactly once, also
// after a cancellation; owner is null once the probe is gone.
struct EmulatorVersionProbe::State {
	EmulatorVersionProbe*    owner = nullptr;
	DoneFn                   done;
	GObjectPtr<GSubprocess>  process;
	GObjectPtr<GCancellable> cancellable;
	SourceId                 timer;
	bool                     timed_out = false;

	static gboolean OnTimeout(gpointer data) {
		auto* state      = static_cast<State*>(data);
		state->timed_out = true;
		g_subprocess_force_exit(state->process.get());
		return G_SOURCE_REMOVE;
	}

	static void OnCommunicated(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<State> state(static_cast<State*>(data));
		GBytes*                stdout_bytes = nullptr;
		GErrorPtr              error;
		const bool ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &stdout_bytes,
		                                                nullptr, error.Out()) != FALSE;
		state->timer.Remove();
		std::string version;
		if (ok && !state->timed_out && stdout_bytes != nullptr) {
			gsize       size  = 0;
			const auto* bytes = static_cast<const char*>(g_bytes_get_data(stdout_bytes, &size));
			version =
			    EmulatorVersionFromOutput(std::string_view(bytes != nullptr ? bytes : "", size));
		}
		if (stdout_bytes != nullptr) {
			g_bytes_unref(stdout_bytes);
		}
		if (state->owner == nullptr) {
			return;
		}
		if (!ok) {
			g_message("the emulator version probe failed: %s",
			          std::string(error.Message()).c_str());
		} else if (state->timed_out) {
			g_message("the emulator version probe did not finish in time");
		}
		state->owner->m_state = nullptr;
		DoneFn done           = std::move(state->done);
		state.reset();
		// done may destroy the probe; it no longer refers to the state.
		done(std::move(version));
	}
};

std::unique_ptr<EmulatorVersionProbe>
EmulatorVersionProbe::Start(const std::filesystem::path& executable, DoneFn done,
                            std::chrono::milliseconds limit) {
	const std::string path   = executable.string();
	const char*       argv[] = {path.c_str(), nullptr};
	GErrorPtr         error;
	auto              process = GObjectPtr<GSubprocess>::Adopt(g_subprocess_newv(
	    argv, GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
	    error.Out()));
	if (!process) {
		g_message("cannot run %s for its version: %s", path.c_str(),
		          std::string(error.Message()).c_str());
		return nullptr;
	}
	auto* state        = new State;
	state->done        = std::move(done);
	state->process     = std::move(process);
	state->cancellable = GObjectPtr<GCancellable>::Adopt(g_cancellable_new());
	state->timer =
	    SourceId(g_timeout_add(static_cast<guint>(limit.count()), &State::OnTimeout, state));
	g_subprocess_communicate_async(state->process.get(), nullptr, state->cancellable.get(),
	                               &State::OnCommunicated, state);
	std::unique_ptr<EmulatorVersionProbe> probe(new EmulatorVersionProbe(state));
	state->owner = probe.get();
	return probe;
}

EmulatorVersionProbe::~EmulatorVersionProbe() {
	if (m_state == nullptr) {
		return;
	}
	m_state->owner = nullptr;
	m_state->timer.Remove();
	g_cancellable_cancel(m_state->cancellable.get());
	g_subprocess_force_exit(m_state->process.get());
}

} // namespace kyty::gtk
