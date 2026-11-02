#pragma once

// A UI case's world (task 22.1; phase 7 plan D13, D16). The constructor makes a temporary tree
// (KytyTests::TempDir, deleted with everything in it by the destructor):
//   app/           the launcher directory: kyty_emulator is a copy of
//   tests/ui/fake-kyty-emulator.sh,
//                  so it is the Emulator, and app/ is the Data_Directory (caches, _SaveData, ...)
//   cwd/           the launcher's working directory
//   config/ data/ home/   XDG_CONFIG_HOME (Kyty/Kyty.ini), XDG_DATA_HOME, HOME of the context's
//                  environment (a KytyTests::FakeEnvironment)
//   fixtures/      WriteFixtureGames()
// and the services: a LoopbackServer with a LoopbackHttpClient, and the fakes of TestServices. It
// sets KYTY_UI_FAKE_EMULATOR (the fake_emulator test helper) and KYTY_UI_ARGS_FILE
// (<tree>/emulator-args) for the fake Emulator. Either Context() (launcher windows opened by the
// case, for example through the dialog registry) or RunApplication() (the real KytyApplication)
// uses them; a case uses one of the two. Main thread only.

#include "launcher/gtk/application.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/mainWindow.h"
#include "support/gtk/loopbackHttp.h"
#include "support/gtk/testServices.h"
#include "support/tempDir.h"
#include "updater/core/updatePolicy.h"

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace KytyUi {

// A small Compatibility_Database with Alpha Game (PPSA01001) as "In game".
inline constexpr const char* kFixtureCompatibility =
    "{\"PPSA01001\": {\"status\": \"InGame\", \"comment\": \"UI test fixture\"}}";

class UiContext {
public:
	UiContext();
	~UiContext();

	UiContext(const UiContext&)            = delete;
	UiContext& operator=(const UiContext&) = delete;

	[[nodiscard]] const std::filesystem::path& Root() const { return m_tree.Path(); }
	[[nodiscard]] std::filesystem::path        AppDirectory() const { return Root() / "app"; }
	[[nodiscard]] std::filesystem::path        WorkingDirectory() const { return Root() / "cwd"; }
	[[nodiscard]] std::filesystem::path        SettingsFile() const;

	KytyTests::LoopbackServer&     Server() { return *m_server; }
	KytyTests::LoopbackHttpClient& Http() { return *m_http; }
	KytyTests::TestServices&       Services() { return *m_services; }

	// The fixture games (uiFixtures.h) under Root()/fixtures; returns their folder.
	std::filesystem::path WriteFixtureGames();
	// The content of Alpha Game for the content windows (uiFixtures.h WriteGameContent) below
	// games (WriteFixtureGames()) and AppDirectory() (the Data_Directory), for the default user ID;
	// Root()/addons as the add-ons folder in Kyty.ini; and the cheat indexes (CheatIndexes()) on
	// the server. Call it before Context(), which reads Kyty.ini once. False after a Fail.
	bool WriteGameContent(const std::filesystem::path& games);
	// Writes Kyty.ini with these Game_Folders (core::SaveSettingsStore).
	bool StoreGameFolders(const std::vector<std::filesystem::path>& folders);
	// Every later download of the Compatibility_Database gets body.
	void ServeCompatibility(std::string body = kFixtureCompatibility);

	// The LauncherContext over the services, made on the first call (the launcher directory is
	// AppDirectory()). The display gets the launcher's style sheet and icons first, as the
	// application has them.
	kyty::gtk::LauncherContext& Context();
	// A presented window, mapped and painted once, to be the parent of dialogs; destroyed by the
	// destructor.
	GtkWindow* ParentWindow();

	// Runs the real KytyApplication (plan D13) with the services, AppDirectory(), and update_mode,
	// from WorkingDirectory(). drive runs once its main window is mapped ("main window presented")
	// and should end with app.quit; when the application has not quit 10 s after drive returned,
	// the case fails and the application quits. Returns g_application_run's status.
	int RunApplication(const std::function<void(KytyApplication*, KytyMainWindow*)>& drive,
	                   std::optional<kyty::updater::UpdateUiMode>                    update_mode =
	                       kyty::updater::UpdateUiMode::Other);

private:
	KytyTests::TempDir                             m_tree;
	std::unique_ptr<KytyTests::LoopbackServer>     m_server;
	std::unique_ptr<KytyTests::LoopbackHttpClient> m_http;
	std::unique_ptr<KytyTests::TestServices>       m_services;
	std::unique_ptr<kyty::gtk::LauncherContext>    m_context;
	GtkWindow*                                     m_parent = nullptr;
};

// The application's main window once it is mapped (within timeout_ms) and painted once; nullptr
// after a Fail.
KytyMainWindow* WaitForMainWindow(GtkApplication* application, int timeout_ms = 5000);

} // namespace KytyUi
