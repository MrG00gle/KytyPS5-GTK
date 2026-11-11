#include "ui/support/uiLibrary.h"

#include "launcher/core/archiveJob.h"
#include "launcher/core/cancelToken.h"
#include "launcher/gtk/gameCell.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

namespace KytyUi {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;

std::string ReadText(const fs::path& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

bool WriteText(const fs::path& path, std::string_view text) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	return Check(static_cast<bool>(out), "write " + path.string());
}

core::LauncherSettings StoredSettings(UiContext& ui) {
	return core::OpenSettingsStore(ui.SettingsFile()).loaded.settings;
}

bool StoreSettings(UiContext& ui, const std::function<void(core::LauncherSettings&)>& change) {
	auto store = core::OpenSettingsStore(ui.SettingsFile());
	auto next  = store.loaded.settings;
	change(next);
	const auto written = core::SaveSettingsStore(store, next);
	return Check(static_cast<bool>(written), "write " + ui.SettingsFile().string() +
	                                             (written ? "" : ": " + written.error().message));
}

fs::path PrepareLibrary(UiContext& ui) {
	const auto games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.StoreGameFolders({games})) {
		return {};
	}
	ui.ServeCompatibility();
	return games;
}

bool WaitForLibrary(KytyMainWindow* window, int timeout_ms) {
	auto& library = kyty::gtk::MainWindowLibrary(window);
	return WaitFor(
	    [&library]() {
		    const auto& stats = library.Stats();
		    return stats.scans_applied >= 1 && stats.details_done >= 1 &&
		           stats.compatibility_finished;
	    },
	    timeout_ms, "the games, their details, and the compatibility statuses");
}

void QuitApplication(KytyApplication* application) {
	g_action_group_activate_action(G_ACTION_GROUP(application), "quit", nullptr);
}

void RunWithLibrary(UiContext& ui, const Drive& drive, kyty::updater::UpdateUiMode mode) {
	const int status = ui.RunApplication(
	    [&drive](KytyApplication* application, KytyMainWindow* window) {
		    if (WaitForLibrary(window)) {
			    drive(application, window);
		    }
		    QuitApplication(application);
	    },
	    mode);
	Check(status == 0, "the application ends with status 0 (" + std::to_string(status) + ")");
}

kyty::gtk::LauncherContext& ContextOf(KytyApplication* application) {
	return *kyty::gtk::ApplicationContext(application);
}

KytyGame* GameByTitle(kyty::gtk::GameListModel& library, std::string_view title) {
	GListModel* games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		if (kyty_game_get_entry(game.get()).title_name == title) {
			return game.get(); // the store keeps its reference
		}
	}
	return nullptr;
}

bool SelectTitle(kyty::gtk::GameListModel& library, std::string_view title) {
	KytyGame* game = GameByTitle(library, title);
	return Check(game != nullptr && library.Select(kyty_game_get_entry(game).path) &&
	                 library.Selected() == game,
	             "select " + std::string(title));
}

guint ShownPosition(kyty::gtk::GameListModel& library, KytyGame* game) {
	GListModel* shown = library.Sorted();
	for (guint i = 0; i < g_list_model_get_n_items(shown); i++) {
		auto item =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(shown, i)));
		if (item.get() == game) {
			return i;
		}
	}
	return GTK_INVALID_LIST_POSITION;
}

std::vector<std::string> ShownTitles(const kyty::gtk::GameListModel& library) {
	std::vector<std::string> titles;
	GListModel*              shown = library.Sorted();
	for (guint i = 0; i < g_list_model_get_n_items(shown); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(shown, i)));
		titles.push_back(kyty_game_get_entry(game.get()).title_name);
	}
	return titles;
}

std::vector<std::string> ExpectedTitles(const kyty::gtk::GameListModel& library,
                                        core::Column column, bool descending) {
	std::vector<core::GameRow> rows;
	std::vector<std::string>   titles;
	GListModel*                games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		rows.push_back(kyty_game_get_row(game.get()));
		titles.push_back(kyty_game_get_entry(game.get()).title_name);
	}
	std::vector<std::string> expected;
	for (const std::size_t index: core::SortRows(rows, column, descending)) {
		expected.push_back(titles[index]);
	}
	return expected;
}

GtkColumnView* ListViewOf(KytyMainWindow* window) {
	return GTK_COLUMN_VIEW(FindByType(GTK_WIDGET(window), GTK_TYPE_COLUMN_VIEW));
}

GtkGridView* GridViewOf(KytyMainWindow* window) {
	return GTK_GRID_VIEW(FindByType(GTK_WIDGET(window), GTK_TYPE_GRID_VIEW));
}

GtkColumnViewColumn* ColumnById(GtkColumnView* view, std::string_view id) {
	GListModel* columns = gtk_column_view_get_columns(view);
	for (guint i = 0; i < g_list_model_get_n_items(columns); i++) {
		auto column = GObjectPtr<GtkColumnViewColumn>::Adopt(
		    static_cast<GtkColumnViewColumn*>(g_list_model_get_item(columns, i)));
		if (g_strcmp0(gtk_column_view_column_get_id(column.get()), std::string(id).c_str()) == 0) {
			return column.get(); // the view keeps its reference
		}
	}
	return nullptr;
}

bool ShowGridView(KytyApplication* application, KytyMainWindow* window, bool grid) {
	g_action_group_change_action_state(G_ACTION_GROUP(application), "toggle-grid",
	                                   g_variant_new_boolean(grid ? TRUE : FALSE));
	GtkWidget* view = grid ? GTK_WIDGET(GridViewOf(window)) : GTK_WIDGET(ListViewOf(window));
	return Check(view != nullptr, grid ? "the main window has its grid view"
	                                   : "the main window has its list view") &&
	       WaitFor([view]() { return gtk_widget_get_mapped(view) != FALSE; }, 3000,
	               grid ? "the grid view to be shown" : "the list view to be shown") &&
	       WaitForPaintedFrame(GTK_WIDGET(window));
}

namespace {

void AppendLe16(std::string& out, uint32_t value) {
	out += static_cast<char>(value & 0xffu);
	out += static_cast<char>((value >> 8u) & 0xffu);
}

void AppendLe32(std::string& out, uint32_t value) {
	AppendLe16(out, value & 0xffffu);
	AppendLe16(out, value >> 16u);
}

std::string RiffChunk(std::string_view id, const std::string& payload) {
	std::string chunk(id);
	AppendLe32(chunk, static_cast<uint32_t>(payload.size()));
	chunk += payload;
	if (payload.size() % 2 != 0) {
		chunk += '\0';
	}
	return chunk;
}

} // namespace

std::string At9TitleSound(int superframes) {
	constexpr std::size_t kSuperframeBytes = 384;
	constexpr int         kFrameSamples    = 256;
	constexpr int         kFrames          = 4;
	static const uint8_t  kMono[4]         = {0xfe, 0x70, 0x0b, 0xf0};
	std::string           fmt;
	AppendLe16(fmt, 0xfffe);
	AppendLe16(fmt, 1);
	AppendLe32(fmt, 48000);
	AppendLe32(fmt, 48000 * kSuperframeBytes / (kFrames * kFrameSamples));
	AppendLe16(fmt, kSuperframeBytes);
	AppendLe16(fmt, 0);
	AppendLe16(fmt, 0x22);
	AppendLe16(fmt, kFrameSamples);
	AppendLe32(fmt, 0x4u);
	static const uint8_t kGuid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                  0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	fmt.append(reinterpret_cast<const char*>(kGuid), sizeof(kGuid));
	AppendLe32(fmt, 1);
	fmt.append(reinterpret_cast<const char*>(kMono), 4);
	fmt.resize(0x34, '\0');
	static const uint8_t kMonoBlock[36] = {0, 0, 0x04, 0x20, 0x04, 0xc0, 0, 0, 0x1f, 0xc0};
	std::string          data;
	for (int superframe = 0; superframe < superframes; superframe++) {
		std::string bytes;
		for (int frame = 0; frame < kFrames; frame++) {
			std::string block(reinterpret_cast<const char*>(kMonoBlock), sizeof(kMonoBlock));
			if (frame != 0) {
				block[0] = static_cast<char>(block[0] | 0x80);
			}
			bytes += block;
		}
		bytes.resize(kSuperframeBytes, '\0');
		data += bytes;
	}
	const std::string body = RiffChunk("fmt ", fmt) + RiffChunk("data", data);
	std::string       file = "RIFF";
	AppendLe32(file, static_cast<uint32_t>(4 + body.size()));
	file += "WAVE";
	return file + body;
}

bool WriteGameWithParam(const fs::path& directory, std::string_view param_json) {
	return WriteText(directory / "sce_sys" / "param.json", param_json) &&
	       WriteText(directory / "eboot.bin", "fake eboot\n");
}

std::string ParamJsonWithFirmware(std::string_view title, std::string_view title_id,
                                  std::string_view version, std::string_view firmware) {
	std::string json = ParamJson(title, title_id, version);
	const auto  key  = json.find("0x0150000000000000");
	if (key != std::string::npos) {
		json.replace(key, std::string_view("0x0150000000000000").size(), firmware);
	}
	return json;
}

fs::path PackGame(const fs::path& staging) {
	auto job = core::ArchiveJob::Prepare(staging);
	if (!job) {
		Fail("cannot prepare the archive of " + staging.string() + ": " + job.error().message);
		return {};
	}
	core::CancelToken cancel;
	job->Run([](int) {}, cancel);
	if (job->Outcome() != core::ArchiveOutcome::Completed) {
		Fail("packing " + staging.string() + " failed: " + job->Error());
		return {};
	}
	std::error_code ec;
	fs::remove_all(staging, ec);
	fs::path archive = staging;
	archive += ".zar";
	return Check(fs::exists(archive, ec), "the archive " + archive.string() + " exists")
	           ? archive
	           : fs::path();
}

std::vector<GtkWidget*> MappedCells(GtkWidget* root, std::string_view text) {
	return FindAll(root, [text](GtkWidget* widget) {
		return KYTY_IS_GAME_CELL(widget) && gtk_widget_get_mapped(widget) != FALSE &&
		       (text.empty() || kyty_game_cell_get_text(KYTY_GAME_CELL(widget)) == text);
	});
}

std::vector<GtkWidget*> RowCells(GtkColumnView* view, std::string_view title) {
	for (GtkWidget* cell: MappedCells(GTK_WIDGET(view), title)) {
		if (kyty_game_cell_get_kind(KYTY_GAME_CELL(cell)) != kyty::gtk::GameCellKind::Title) {
			continue;
		}
		// The cell's widget, then the row's.
		GtkWidget* column_cell = gtk_widget_get_parent(cell);
		GtkWidget* row = column_cell != nullptr ? gtk_widget_get_parent(column_cell) : nullptr;
		if (row == nullptr) {
			break;
		}
		std::vector<GtkWidget*> cells;
		for (GtkWidget* child = gtk_widget_get_first_child(row); child != nullptr;
		     child            = gtk_widget_get_next_sibling(child)) {
			cells.push_back(gtk_widget_get_first_child(child));
		}
		return cells;
	}
	return {};
}

GtkWidget* GridTile(GtkGridView* view, std::string_view title) {
	for (GtkWidget* cell: MappedCells(GTK_WIDGET(view), title)) {
		if (kyty_game_cell_get_kind(KYTY_GAME_CELL(cell)) == kyty::gtk::GameCellKind::Tile) {
			return cell;
		}
	}
	return nullptr;
}

void UseCaseEnvironment(UiContext& ui, KytyTests::TestServices& services) {
	services.environment.Set("HOME", (ui.Root() / "home").string());
	services.environment.Set("XDG_CONFIG_HOME", (ui.Root() / "config").string());
	services.environment.Set("XDG_DATA_HOME", (ui.Root() / "data").string());
}

OwnMainWindow::OwnMainWindow(UiContext& ui, kyty::gtk::Services services, bool local) {
	ui.ParentWindow(); // the launcher's style sheet and icons
	m_context = kyty::gtk::CreateLauncherContext({.working_directory  = ui.WorkingDirectory(),
	                                              .launcher_directory = ui.AppDirectory(),
	                                              .update_mode = kyty::updater::UpdateUiMode::Other,
	                                              .local       = local},
	                                             services);
	m_window  = kyty_main_window_new(*m_context, nullptr);
	m_gtk_window = GTK_WINDOW(m_window);
	gtk_window_present(m_gtk_window);
	if (!WaitFor([this]() { return gtk_widget_get_mapped(GTK_WIDGET(m_window)) != FALSE; }, 5000,
	             "the main window to be mapped") ||
	    !WaitForPaintedFrame(GTK_WIDGET(m_window))) {
		m_failed = true;
	}
}

OwnMainWindow::~OwnMainWindow() {
	// By address: the window may be gone (a cast of a destroyed window is a critical).
	if (m_gtk_window != nullptr && IsToplevel(m_gtk_window)) {
		gtk_window_destroy(m_gtk_window);
	}
	Pump(100);
	m_context.reset();
	Pump(50);
}

} // namespace KytyUi
