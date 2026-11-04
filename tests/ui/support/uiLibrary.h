#pragma once

// The game library in UI cases (task 22.4; phase 7 plan section 6 U5), shared by
// LibraryTests.cpp, CompatibilityTests.cpp, and TitleSoundTests.cpp: the fixture library in
// Kyty.ini, the real application over it (UiContext::RunApplication), and lookups of the library's
// games and views. The same recipes as InventoryTests.cpp's own helpers (U4). Main thread only;
// failures go to the running case (uiHarness.h).

#include "launcher/core/gameLibrary.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/mainWindow.h"
#include "support/gtk/testServices.h"
#include "ui/support/uiContext.h"
#include "updater/core/updatePolicy.h"

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace KytyUi {

using Drive = std::function<void(KytyApplication*, KytyMainWindow*)>;

// Two of the fixture games (uiFixtures.h WriteFixtures).
inline constexpr const char* kAlphaTitle = "Alpha Game";
inline constexpr const char* kBetaTitle  = "Beta Quest";

// The file's bytes ("" when it cannot be read).
std::string ReadText(const std::filesystem::path& path);
// Writes text, creating the parent directories; false after a Fail.
bool WriteText(const std::filesystem::path& path, std::string_view text);

// What Kyty.ini holds now (the defaults when it does not exist).
kyty::core::LauncherSettings StoredSettings(UiContext& ui);
// Changes Kyty.ini (before the application or the context reads it); false after a Fail.
bool StoreSettings(UiContext& ui, const std::function<void(kyty::core::LauncherSettings&)>& change);

// The fixture games in a Game_Folder of Kyty.ini and the compatibility database on the server;
// returns the games' folder (empty after a Fail).
std::filesystem::path PrepareLibrary(UiContext& ui);

// The window's first scan, the games' details, and the compatibility result are there.
bool WaitForLibrary(KytyMainWindow* window, int timeout_ms = 8000);

// app.quit.
void QuitApplication(KytyApplication* application);

// RunApplication: drive runs once the library is there (WaitForLibrary), then the application
// quits; the case fails unless it ends with status 0.
void RunWithLibrary(UiContext& ui, const Drive& drive,
                    kyty::updater::UpdateUiMode mode = kyty::updater::UpdateUiMode::Other);

kyty::gtk::LauncherContext& ContextOf(KytyApplication* application);

// The listed game titled title (transfer none: the library's store keeps it), or nullptr.
KytyGame* GameByTitle(kyty::gtk::GameListModel& library, std::string_view title);
// Selects the listed game titled title (as a click on its row does); false after a Fail.
bool SelectTitle(kyty::gtk::GameListModel& library, std::string_view title);
// The position of game in the shown (sorted and filtered) games, or GTK_INVALID_LIST_POSITION.
guint ShownPosition(kyty::gtk::GameListModel& library, KytyGame* game);

// The titles the views show, in their order.
std::vector<std::string> ShownTitles(const kyty::gtk::GameListModel& library);
// The titles of every game in the order core::SortRows gives them.
std::vector<std::string> ExpectedTitles(const kyty::gtk::GameListModel& library,
                                        kyty::core::Column column, bool descending);

// The main window's list and grid views and the list's column with the id (gameListView.cpp).
GtkColumnView*       ListViewOf(KytyMainWindow* window);
GtkGridView*         GridViewOf(KytyMainWindow* window);
GtkColumnViewColumn* ColumnById(GtkColumnView* view, std::string_view id);

// Shows the grid view (grid) or the list view through app.toggle-grid's state and waits until it
// is mapped and painted; false after a Fail.
bool ShowGridView(KytyApplication* application, KytyMainWindow* window, bool grid);

// A synthetic mono ATRAC9 title sound (snd0.at9) of superframes 384-byte superframes (48 kHz, 4
// frames of 256 samples each: about 8.5 s for 400), the builder of
// tests/launcher_core/TitleSoundDecodeTests.cpp and of InventoryTests.cpp's lfi_09.
std::string At9TitleSound(int superframes);

// ---- Games of a case's own

// A game directory with this sce_sys/param.json text and an eboot.bin; false after a Fail.
bool WriteGameWithParam(const std::filesystem::path& directory, std::string_view param_json);
// A param.json as uiFixtures.h ParamJson writes it, with the raw requiredSystemSoftwareVersion.
std::string ParamJsonWithFirmware(std::string_view title, std::string_view title_id,
                                  std::string_view version, std::string_view firmware);
// Packs the game directory staging into <staging>.zar (core::ArchiveJob) and removes staging;
// returns the archive's path (empty after a Fail).
std::filesystem::path PackGame(const std::filesystem::path& staging);

// ---- The views' drawn cells (launcher/gtk/gameCell.h)

// The mapped KytyGameCell widgets below root whose text is text (any text when text is empty).
std::vector<GtkWidget*> MappedCells(GtkWidget* root, std::string_view text = {});
// The list view's row of the game titled title: the child widget of each of its cells, column by
// column (a KytyGameCell, or the --local editors' box or entry); empty when the row is not shown.
std::vector<GtkWidget*> RowCells(GtkColumnView* view, std::string_view title);
// The grid view's mapped tile of the game titled title, or nullptr.
GtkWidget* GridTile(GtkGridView* view, std::string_view title);

// ---- A main window over a context of the case's own

// The test environment UiContext gives its own services (HOME, XDG_CONFIG_HOME, XDG_DATA_HOME
// below ui.Root()), for another TestServices of the case.
void UseCaseEnvironment(UiContext& ui, KytyTests::TestServices& services);

// The context over services (the launcher directory UiContext::AppDirectory(), the working
// directory ui.WorkingDirectory()), a main window over it without an application, presented,
// mapped, and painted, and both destroyed in order. For the cases that set the context up before
// the window exists (--local) or use services of their own (a virtual clock), and the title sound
// cases (the application's finalize quits SDL under the services' player).
class OwnMainWindow {
public:
	OwnMainWindow(UiContext& ui, kyty::gtk::Services services, bool local = false);
	~OwnMainWindow();
	OwnMainWindow(const OwnMainWindow&)            = delete;
	OwnMainWindow& operator=(const OwnMainWindow&) = delete;

	[[nodiscard]] bool                        Ok() const { return !m_failed; }
	[[nodiscard]] kyty::gtk::LauncherContext& Context() { return *m_context; }
	[[nodiscard]] KytyMainWindow*             Window() { return m_window; }

private:
	std::unique_ptr<kyty::gtk::LauncherContext> m_context;
	KytyMainWindow*                             m_window     = nullptr;
	GtkWindow*                                  m_gtk_window = nullptr;
	bool                                        m_failed     = false;
};

// Sets the fake Emulator's behavior (tests/ui/fake-kyty-emulator.sh) until destroyed.
class FakeEmulatorBehavior {
public:
	explicit FakeEmulatorBehavior(const char* args) { g_setenv("KYTY_UI_FAKE_ARGS", args, TRUE); }
	~FakeEmulatorBehavior() { g_unsetenv("KYTY_UI_FAKE_ARGS"); }
	FakeEmulatorBehavior(const FakeEmulatorBehavior&)            = delete;
	FakeEmulatorBehavior& operator=(const FakeEmulatorBehavior&) = delete;
};

} // namespace KytyUi
