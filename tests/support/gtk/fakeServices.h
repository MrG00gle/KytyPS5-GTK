#pragma once

// Fakes of the GTK launcher's services (src/launcher/gtk/services/ and updater::ProcessRunner;
// phase 7 plan D2) for the display-free GTK tests and the UI tests, ported from the phase 6 visual
// harness. They open no portal, no SDL device, no process, and no desktop application. Each keeps
// the contract of its interface: an asynchronous call answers from the main loop (an idle source of
// the default main context), never inside the call, and never after its cancellable was cancelled
// or the fake was destroyed. Main thread only.

#include "launcher/core/expected.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/services/desktopLauncher.h"
#include "launcher/gtk/services/deviceLists.h"
#include "launcher/gtk/services/fileChooser.h"
#include "updater/core/processRunner.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace KytyTests {

// A FileChooser that answers from queues (FIFO) and records every call, in call order:
// "SelectFolders <title>", "OpenFile <title> [<filter name> <pattern>...]...", and
// "SaveFile <title> | <suggested name>". An empty queue answers that nothing was chosen (no
// folder, no file, no error).
class ScriptedFileChooser final: public kyty::gtk::FileChooser {
public:
	ScriptedFileChooser();
	~ScriptedFileChooser() override;

	ScriptedFileChooser(const ScriptedFileChooser&)            = delete;
	ScriptedFileChooser& operator=(const ScriptedFileChooser&) = delete;

	void SelectFolders(GtkWindow* parent, const std::string& title, FoldersFn done,
	                   GCancellable* cancellable) override;
	void OpenFile(GtkWindow* parent, const std::string& title,
	              const std::vector<kyty::gtk::FileFilter>& filters, FileFn done,
	              GCancellable* cancellable) override;
	void SaveFile(GtkWindow* parent, const std::string& title, const std::string& suggested_name,
	              FileFn done, GCancellable* cancellable) override;

	std::deque<std::vector<std::filesystem::path>>   folder_answers; // SelectFolders
	std::deque<std::optional<std::filesystem::path>> file_answers;   // OpenFile and SaveFile
	std::vector<std::string>                         calls;

private:
	std::shared_ptr<bool> m_alive;
};

// A DesktopLauncher that records every call ("OpenFolder <path>", "RevealFile <path>",
// "OpenUri <uri>") and answers each with next_error ("" is success).
class RecordingDesktopLauncher final: public kyty::gtk::DesktopLauncher {
public:
	RecordingDesktopLauncher();
	~RecordingDesktopLauncher() override;

	RecordingDesktopLauncher(const RecordingDesktopLauncher&)            = delete;
	RecordingDesktopLauncher& operator=(const RecordingDesktopLauncher&) = delete;

	void OpenFolder(GtkWindow* parent, const std::filesystem::path& folder, DoneFn done,
	                GCancellable* cancellable) override;
	void RevealFile(GtkWindow* parent, const std::filesystem::path& file, DoneFn done,
	                GCancellable* cancellable) override;
	void OpenUri(GtkWindow* parent, const std::string& uri, DoneFn done,
	             GCancellable* cancellable) override;

	std::vector<std::string> calls;
	std::string              next_error;

private:
	std::shared_ptr<bool> m_alive;
};

// DeviceLists with the lists the test sets (empty by default); counts the calls.
class FakeDeviceLists final: public kyty::gtk::DeviceLists {
public:
	kyty::gtk::DeviceList VulkanDevices() override;
	kyty::gtk::DeviceList RecordingDevices() override;

	kyty::gtk::DeviceList vulkan;
	kyty::gtk::DeviceList recording;
	int                   vulkan_calls    = 0;
	int                   recording_calls = 0;
};

// A ControllerService without SDL: the test sets the state (Set), every listener runs at once on
// each change, and the lightbar colors and pause calls are recorded. SetLightbarColor and
// SetPaused do what the SDL service does with the state: an unchanged value changes nothing, a
// changed pause flag runs the listeners, and Polling() is true while there is a listener or a
// lightbar color and the service is not paused.
class FakeControllerService final: public kyty::gtk::ControllerService {
public:
	struct LightbarCall {
		int64_t                                 time_us = 0; // g_get_monotonic_time()
		std::optional<kyty::gtk::LightbarColor> color;
	};

	ListenerId AddListener(ChangedFn on_changed) override;
	void       RemoveListener(ListenerId id) override;
	void       SetLightbarColor(std::optional<kyty::gtk::LightbarColor> color) override;
	void       SetPaused(bool paused) override;
	[[nodiscard]] const kyty::gtk::ControllerState& State() const override { return m_state; }
	[[nodiscard]] bool                              Polling() const override;

	// Replaces the state (its paused flag included) and runs every listener.
	void Set(kyty::gtk::ControllerState state);

	[[nodiscard]] std::size_t Listeners() const { return m_listeners.size(); }
	// The color of the last SetLightbarColor call; none before the first one.
	[[nodiscard]] std::optional<kyty::gtk::LightbarColor> Lightbar() const { return m_color; }

	std::vector<LightbarCall> lightbar_calls; // every SetLightbarColor call
	int                       pause_calls = 0;

private:
	void Notify();

	std::map<ListenerId, ChangedFn>         m_listeners;
	ListenerId                              m_next = 0;
	kyty::gtk::ControllerState              m_state;
	std::optional<kyty::gtk::LightbarColor> m_color;
};

// An updater::ProcessRunner that starts nothing: it records every call, runs on_run inside Run()
// (an update session is Extracting or Validating then, so a UI test can look at the window), and
// answers from scripted results (FIFO): Run() from run_results (an empty queue: exit status 0),
// StartDetached() from detached_errors ("" or an empty queue: started).
class FakeProcessRunner final: public kyty::updater::ProcessRunner {
public:
	struct Call {
		std::filesystem::path    program;
		std::vector<std::string> args;
		std::filesystem::path    cwd;
		std::filesystem::path    log_file; // StartDetached only
		bool                     detached = false;
	};

	kyty::updater::ProcessResult Run(const std::filesystem::path& program,
	                                 std::vector<std::string>     args,
	                                 const std::filesystem::path& cwd) override;
	kyty::core::Expected<void, kyty::updater::ProcessError>
	StartDetached(const std::filesystem::path& program, std::vector<std::string> args,
	              const std::filesystem::path& cwd, const std::filesystem::path& log_file) override;

	std::vector<Call>                                         calls;
	std::function<void(const std::filesystem::path& program)> on_run;
	std::deque<kyty::updater::ProcessResult>                  run_results;
	std::deque<std::string>                                   detached_errors;
};

} // namespace KytyTests
