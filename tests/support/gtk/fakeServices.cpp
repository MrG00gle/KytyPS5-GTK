#include "support/gtk/fakeServices.h"

#include "launcher/gtk/gobjectPtr.h"

#include <glib.h>
#include <utility>

namespace KytyTests {

namespace {

using kyty::gtk::GObjectPtr;

// Runs fn once from the main loop, unless cancellable (may be null) is cancelled or the fake whose
// alive flag this is was destroyed first.
void Later(const std::shared_ptr<bool>& alive, GCancellable* cancellable,
           std::function<void()> fn) {
	struct Call {
		std::shared_ptr<bool>    alive;
		GObjectPtr<GCancellable> cancellable;
		std::function<void()>    fn;
	};
	auto* call = new Call {alive, GObjectPtr<GCancellable>::Ref(cancellable), std::move(fn)};
	g_idle_add_full(
	    G_PRIORITY_DEFAULT_IDLE,
	    [](gpointer data) -> gboolean {
		    auto* pending = static_cast<Call*>(data);
		    if (*pending->alive &&
		        (!pending->cancellable ||
		         g_cancellable_is_cancelled(pending->cancellable.get()) == FALSE)) {
			    pending->fn();
		    }
		    return G_SOURCE_REMOVE;
	    },
	    call, [](gpointer data) { delete static_cast<Call*>(data); });
}

template <typename T>
T PopFront(std::deque<T>& queue, T fallback) {
	if (queue.empty()) {
		return fallback;
	}
	T front = std::move(queue.front());
	queue.pop_front();
	return front;
}

} // namespace

// ---- ScriptedFileChooser

ScriptedFileChooser::ScriptedFileChooser(): m_alive(std::make_shared<bool>(true)) {}

ScriptedFileChooser::~ScriptedFileChooser() {
	*m_alive = false;
}

void ScriptedFileChooser::SelectFolders(GtkWindow* /*parent*/, const std::string& title,
                                        FoldersFn done, GCancellable* cancellable) {
	calls.push_back("SelectFolders " + title);
	auto answer = PopFront(folder_answers, {});
	Later(m_alive, cancellable,
	      [done = std::move(done), answer = std::move(answer)]() { done(answer, ""); });
}

void ScriptedFileChooser::OpenFile(GtkWindow* /*parent*/, const std::string& title,
                                   const std::vector<kyty::gtk::FileFilter>& filters, FileFn done,
                                   GCancellable* cancellable) {
	std::string call = "OpenFile " + title;
	for (const auto& filter: filters) {
		call += " [" + filter.name;
		for (const auto& pattern: filter.patterns) {
			call += " " + pattern;
		}
		call += "]";
	}
	calls.push_back(call);
	auto answer = PopFront(file_answers, {});
	Later(m_alive, cancellable,
	      [done = std::move(done), answer = std::move(answer)]() { done(answer, ""); });
}

void ScriptedFileChooser::SaveFile(GtkWindow* /*parent*/, const std::string& title,
                                   const std::string& suggested_name, FileFn done,
                                   GCancellable* cancellable) {
	calls.push_back("SaveFile " + title + " | " + suggested_name);
	auto answer = PopFront(file_answers, {});
	Later(m_alive, cancellable,
	      [done = std::move(done), answer = std::move(answer)]() { done(answer, ""); });
}

// ---- RecordingDesktopLauncher

RecordingDesktopLauncher::RecordingDesktopLauncher(): m_alive(std::make_shared<bool>(true)) {}

RecordingDesktopLauncher::~RecordingDesktopLauncher() {
	*m_alive = false;
}

void RecordingDesktopLauncher::OpenFolder(GtkWindow* /*parent*/,
                                          const std::filesystem::path& folder, DoneFn done,
                                          GCancellable* cancellable) {
	calls.push_back("OpenFolder " + folder.string());
	Later(m_alive, cancellable, [done = std::move(done), error = next_error]() { done(error); });
}

void RecordingDesktopLauncher::RevealFile(GtkWindow* /*parent*/, const std::filesystem::path& file,
                                          DoneFn done, GCancellable* cancellable) {
	calls.push_back("RevealFile " + file.string());
	Later(m_alive, cancellable, [done = std::move(done), error = next_error]() { done(error); });
}

void RecordingDesktopLauncher::OpenUri(GtkWindow* /*parent*/, const std::string& uri, DoneFn done,
                                       GCancellable* cancellable) {
	calls.push_back("OpenUri " + uri);
	Later(m_alive, cancellable, [done = std::move(done), error = next_error]() { done(error); });
}

// ---- FakeDeviceLists

kyty::gtk::DeviceList FakeDeviceLists::VulkanDevices() {
	vulkan_calls++;
	return vulkan;
}

kyty::gtk::DeviceList FakeDeviceLists::RecordingDevices() {
	recording_calls++;
	return recording;
}

// ---- FakeControllerService

FakeControllerService::ListenerId FakeControllerService::AddListener(ChangedFn on_changed) {
	const ListenerId id = ++m_next;
	m_listeners.emplace(id, std::move(on_changed));
	return id;
}

void FakeControllerService::RemoveListener(ListenerId id) {
	m_listeners.erase(id);
}

void FakeControllerService::SetLightbarColor(std::optional<kyty::gtk::LightbarColor> color) {
	lightbar_calls.push_back({g_get_monotonic_time(), color});
	m_color = color;
}

void FakeControllerService::SetPaused(bool paused) {
	pause_calls++;
	if (paused == m_state.paused) {
		return;
	}
	m_state.paused = paused;
	Notify();
}

bool FakeControllerService::Polling() const {
	return !m_state.paused && (!m_listeners.empty() || m_color.has_value());
}

void FakeControllerService::Set(kyty::gtk::ControllerState state) {
	m_state = std::move(state);
	Notify();
}

// A listener may add or remove listeners: each runs from a copy, unless it was removed meanwhile.
void FakeControllerService::Notify() {
	const auto listeners = m_listeners;
	for (const auto& [id, fn]: listeners) {
		if (m_listeners.count(id) != 0 && fn) {
			fn();
		}
	}
}

// ---- FakeProcessRunner

kyty::updater::ProcessResult FakeProcessRunner::Run(const std::filesystem::path& program,
                                                    std::vector<std::string>     args,
                                                    const std::filesystem::path& cwd) {
	calls.push_back({program, std::move(args), cwd, {}, false});
	if (on_run) {
		on_run(program);
	}
	return PopFront(run_results, kyty::updater::ProcessResult {0, "", ""});
}

kyty::core::Expected<void, std::string>
FakeProcessRunner::StartDetached(const std::filesystem::path& program,
                                 std::vector<std::string> args, const std::filesystem::path& cwd,
                                 const std::filesystem::path& log_file) {
	calls.push_back({program, std::move(args), cwd, log_file, true});
	std::string error = PopFront(detached_errors, std::string());
	if (!error.empty()) {
		return kyty::core::MakeUnexpected(std::move(error));
	}
	return {};
}

} // namespace KytyTests
