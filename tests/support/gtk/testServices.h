#pragma once

// The services of a test's LauncherContext (phase 7 plan D2): the fakes of fakeServices.h, an
// Environment, a scheduler, the test's HTTP client, and a real TitleSoundPlayer (the tests run with
// SDL's dummy audio driver), so that no test opens a portal, an SDL device, a process, or the
// network. Get() hands them to a kyty::gtk::LauncherContext, which holds them by reference: the
// TestServices must outlive every context made from it. Main thread only.

#include "launcher/core/environment.h"
#include "launcher/core/httpClient.h"
#include "launcher/core/scheduler.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/services/glibScheduler.h"
#include "launcher/gtk/services/titleSoundPlayer.h"
#include "support/fakeEnvironment.h"
#include "support/gtk/fakeServices.h"

#include <memory>

namespace KytyTests {

class TestServices {
public:
	// http is the test's client (a FakeHttpClient, a loopback client). The context's timers run on
	// scheduler, or on an own GlibScheduler (the default main context) when it is nullptr; a test
	// with a FakeHttpClient passes that client's FakeScheduler, so that both use its virtual clock.
	explicit TestServices(kyty::core::HttpClient& http, kyty::core::Scheduler* scheduler = nullptr);
	~TestServices();

	TestServices(const TestServices&)            = delete;
	TestServices& operator=(const TestServices&) = delete;

	// From now on Get() hands out core::MakePosixEnvironment() (the process's own environment and
	// file system) instead of environment.
	void UsePosixEnvironment();

	[[nodiscard]] kyty::gtk::Services Get();

	// Get() hands out environment (no variable and no existing path until the test adds them)
	// unless UsePosixEnvironment() was called.
	FakeEnvironment             environment;
	ScriptedFileChooser         file_chooser;
	RecordingDesktopLauncher    desktop_launcher;
	FakeDeviceLists             device_lists;
	FakeControllerService       controller;
	FakeProcessRunner           process_runner;
	kyty::gtk::TitleSoundPlayer title_sound;

private:
	kyty::core::HttpClient&                  m_http;
	kyty::gtk::GlibScheduler                 m_glib_scheduler;
	kyty::core::Scheduler&                   m_scheduler;
	std::unique_ptr<kyty::core::Environment> m_posix_environment;
};

} // namespace KytyTests
