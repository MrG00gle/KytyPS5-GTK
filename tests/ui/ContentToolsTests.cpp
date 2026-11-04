// UI tests of the content tools (task 22.5; Requirements 11.1-11.6, 11.8-11.11, 11.13-11.16, 9.9;
// design.md LFI-24-LFI-31; phase 7 plan D29, W4-13-W4-28), each on Wayland and on X11. Most cases
// open the content windows over UiContext::Context() with the fixture games and Alpha Game's
// content (uiFixtures.h WriteGameContent), drive them the way the user would (buttons, rows, keys,
// the scripted file chooser, the fake controller), and check the windows and the files afterwards;
// the compression cases that need the main window's rescan or app.quit run the real
// KytyApplication (UiContext::RunApplication).
//
// - Confirmation flows (11.5, 11.11, 11.13, 9.9): content_clear_config_confirm,
//   content_remove_save_data_confirm, content_cheats_import_confirm,
//   content_compress_delete_original, content_import_settings_other_title.
// - Trophies (11.1, 11.2, LFI-25, LFI-26): content_trophy_viewer, content_trophy_details,
//   content_trophy_overview, content_trophy_overview_alignment (W4-20),
//   content_trophy_overview_cancel (W4-16).
// - Add-ons (11.3, LFI-28): content_addons_list, content_addons_refresh_focused (W4-27).
// - Compression (11.8-11.11, 11.16, LFI-29): content_compress_progress_cancel,
//   content_compress_verify_failed, content_compress_quit_midway (W4-17).
// - Cheats (11.4-11.6, LFI-27): content_cheats_remote_list (W4-24, W4-28),
//   content_cheats_remote_not_json (W4-25), content_cheats_local_states (W4-22, W4-23),
//   content_cheats_apply_enable_disable.
// - Input mapping (11.14, LFI-24): content_input_mapping_capture (key, mouse, and pad),
//   content_input_mapping_per_game, content_input_mapping_per_game_note (W4-14),
//   content_input_mapping_markup (W4-13, a translated section title with "<" and "&" through a
//   catalog the case writes), content_controller_preview (the pressed-state highlight, 11.11's
//   preview of the input mapping editor).

#include "launcher/core/archiveJob.h"
#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/inputMapping.h"
#include "launcher/core/links.h"
#include "launcher/core/saveData.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/trophyModel.h"
#include "launcher/gtk/additionalContentWindow.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/bindingCaptureDialog.h"
#include "launcher/gtk/cheatsWindow.h"
#include "launcher/gtk/compressionDialog.h"
#include "launcher/gtk/dualsenseWidget.h"
#include "launcher/gtk/gameActions.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/inputMappingWindow.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/trophyDetailsDialog.h"
#include "launcher/gtk/trophyOverviewWindow.h"
#include "launcher/gtk/trophyWindow.h"
#include "support/contentGenerators.h"
#include "ui/support/uiCatalog.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"
#include "ui/support/uiWindows.h"

#include <adwaita.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
namespace gen  = KytyTests::ContentGen;
using kyty::gtk::GObjectPtr;
using KytyUi::Check;
using KytyUi::kAlphaTitle;
using KytyUi::kBetaTitle;

// ---- Fixtures

// The fixture games with Alpha Game's content in a Game_Folder of Kyty.ini and the compatibility
// database on the server; returns the games' folder (empty after a Fail).
fs::path ContentLibrary(KytyUi::UiContext& ui) {
	const auto games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written") || !ui.WriteGameContent(games) ||
	    !ui.StoreGameFolders({games})) {
		return {};
	}
	ui.ServeCompatibility();
	return games;
}

// Alpha Game (or Beta Quest) as the scan finds it, with its version and kind.
core::GameEntry Entry(const fs::path& games, std::string_view title) {
	auto entry         = KytyUi::FixtureEntry(games, title);
	entry.game_version = title == kAlphaTitle ? "01.000" : "01.020";
	entry.kind         = core::GameEntry::Kind::Directory;
	return entry;
}

gen::TrophySpec Trophy(int id, char grade, std::string name, bool hidden = false) {
	gen::TrophySpec spec;
	spec.id     = id;
	spec.grade  = grade;
	spec.hidden = hidden;
	spec.name   = std::move(name);
	spec.detail = "Details of trophy " + std::to_string(id) + ".";
	return spec;
}

// A trophy package with these trophies in <game>/sce_sys/trophy2/<file>, and the unlocks of
// user_id kDefaultUserId in data_dir.
bool WriteTrophies(const fs::path& game, const std::string& title_id, const fs::path& data_dir,
                   const std::string& title, std::vector<gen::TrophySpec> trophies,
                   std::set<int> unlocked, const char* file = "trophy00.ucp") {
	gen::TrophyPackageSpec package;
	package.title    = title;
	package.trophies = std::move(trophies);
	core::TrophyUnlocks unlocks;
	unlocks.unlocked = std::move(unlocked);
	return KytyUi::WriteText(game / "sce_sys" / "trophy2" / file,
	                         gen::TrophyPackageBytes(package)) &&
	       Check(gen::WriteUnlocks(data_dir, title_id, core::kDefaultUserId, 0, unlocks),
	             "write the trophy unlocks of " + title_id);
}

// The library's games become entries (for the windows that read context.library).
void FillLibrary(kyty::gtk::LauncherContext& context, std::vector<core::GameEntry> entries) {
	context.library.Replace(std::move(entries));
}

// A window of type the case presents itself (transient for the context's parent window), mapped
// and painted; nullptr after a Fail.
GtkWindow* Present(GtkWindow* window, const std::string& what) {
	gtk_window_present(window);
	return KytyUi::WaitForWindow(G_OBJECT_TYPE(window), what);
}

// The mapped widgets below root of type whose accessible or own name starts with prefix.
std::vector<GtkWidget*> Named(GtkWidget* root, GType type, std::string_view prefix) {
	return KytyUi::FindAll(root, [type, prefix](GtkWidget* widget) {
		return G_TYPE_CHECK_INSTANCE_TYPE(widget, type) && gtk_widget_get_mapped(widget) != FALSE &&
		       KytyUi::ControlName(widget).starts_with(prefix);
	});
}

GtkWidget* FirstNamed(GtkWidget* root, GType type, std::string_view prefix) {
	const auto found = Named(root, type, prefix);
	return found.empty() ? nullptr : found.front();
}

bool HasText(GtkWidget* root, std::string_view text) {
	return KytyUi::AllText(root).find(text) != std::string::npos;
}

// The widget's position in root's coordinates.
graphene_point_t PositionIn(GtkWidget* widget, GtkWidget* root) {
	graphene_point_t point = GRAPHENE_POINT_INIT(0, 0);
	graphene_point_t out   = GRAPHENE_POINT_INIT(-1, -1);
	if (gtk_widget_compute_point(widget, root, &point, &out) == FALSE) {
		KytyUi::Fail("the position of " + KytyUi::WidgetPath(widget));
	}
	return out;
}

// The names of the entries in directory (empty when it does not exist).
std::set<std::string> Listing(const fs::path& directory) {
	std::set<std::string> names;
	std::error_code       ec;
	for (auto it = fs::directory_iterator(directory, ec); !ec && it != fs::directory_iterator();
	     it.increment(ec)) {
		names.insert(it->path().filename().string());
	}
	return names;
}

// ---- Compression

// A packer for the dialog cases: writes the archive, then reports steps progress calls interval_ms
// apart and stops when one returns false; at the end it fails, succeeds, or writes bytes that are
// no archive (then the verifier refuses it).
enum class PackEnd { Succeed, Fail, NotAnArchive };

core::ArchivePacker SlowPacker(int steps, int interval_ms, PackEnd end,
                               std::shared_ptr<std::atomic<int>> calls) {
	return [=](const fs::path& /*directory*/, const fs::path& archive,
	           const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
		if (!gen::WriteHostFile(archive, "partial archive")) {
			return false;
		}
		for (int i = 1; i <= steps; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
			calls->fetch_add(1);
			if (!progress(static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(steps) + 1)) {
				std::error_code ec;
				fs::remove(archive, ec);
				return false;
			}
		}
		if (end == PackEnd::Fail) {
			std::error_code ec;
			fs::remove(archive, ec);
			return false;
		}
		return end == PackEnd::Succeed || gen::WriteHostFile(archive, "this is not a ZAR archive");
	};
}

double Fraction(GtkWindow* dialog) {
	auto* bar = GTK_PROGRESS_BAR(KytyUi::FindByType(GTK_WIDGET(dialog), GTK_TYPE_PROGRESS_BAR));
	return bar != nullptr ? gtk_progress_bar_get_fraction(bar) : -1;
}

// A file of size random bytes (zstd cannot shrink it, so packing it takes a while).
bool WriteRandomFile(const fs::path& path, std::size_t size) {
	std::string     bytes(size, '\0');
	std::mt19937_64 random(42);
	for (std::size_t i = 0; i + 8 <= size; i += 8) {
		const uint64_t value = random();
		for (int b = 0; b < 8; b++) {
			bytes[i + static_cast<std::size_t>(b)] = static_cast<char>((value >> (8 * b)) & 0xffu);
		}
	}
	return KytyUi::WriteText(path, bytes);
}

// ---- Cheats

GtkWindow* OpenCheats(KytyUi::UiContext& ui, const core::GameEntry& entry) {
	return Present(GTK_WINDOW(kyty_cheats_window_new(ui.Context(), ui.ParentWindow(), entry)),
	               "the cheats window");
}

// The remote file row whose accessible name ends with name.
GtkWidget* FileRow(GtkWidget* root, std::string_view name) {
	return KytyUi::FindFirst(root, [name](GtkWidget* widget) {
		return GTK_IS_LIST_BOX_ROW(widget) && gtk_widget_get_mapped(widget) != FALSE &&
		       KytyUi::ControlName(widget).ends_with(name);
	});
}

bool WaitForRemoteList(GtkWidget* root) {
	return KytyUi::WaitFor(
	    [root]() { return FileRow(root, ", PPSA01001_01.000_alpha.json") != nullptr; }, 8000,
	    "the remote list to show Alpha Game's files");
}

// Serves Alpha Game's 01.000 JSON file with body.
void ServeAlphaFile(KytyUi::UiContext& ui, std::string body) {
	core::RemoteCheatFile file;
	file.name    = gen::CheatFileName("PPSA01001", "01.000", "alpha", "json");
	file.version = "01.000";
	file.format  = "json";
	KytyTests::LoopbackResponse response;
	response.body         = std::move(body);
	response.content_type = "text/plain";
	ui.Server().Script(core::RemoteCheatFileUrl(file), std::move(response));
}

GtkWidget* Widget(GtkWidget* root, std::string_view id) {
	return KytyUi::FindByBuildableId(root, id);
}

std::string LabelText(GtkWidget* root, std::string_view id) {
	return KytyUi::WidgetText(Widget(root, id));
}

// The local cheats' names and states as the file holds them.
std::vector<std::pair<std::string, bool>> StoredCheats(const fs::path& data_dir) {
	std::vector<std::pair<std::string, bool>> states;
	const auto                                local = core::LoadLocalCheats(data_dir, "PPSA01001");
	if (local.document) {
		for (const auto& cheat: local.document->cheats) {
			states.emplace_back(cheat.name, cheat.enabled);
		}
	}
	return states;
}

// ---- Input mapping

std::vector<GtkWidget*> MappedBindings(GtkWidget* root) {
	return KytyUi::FindAll(root, [](GtkWidget* widget) {
		return GTK_IS_BUTTON(widget) && gtk_widget_has_css_class(widget, "binding") != FALSE &&
		       gtk_widget_get_mapped(widget) != FALSE;
	});
}

// The binding button of the row titled title on the shown tab.
GtkWidget* BindingOf(GtkWidget* root, std::string_view title) {
	for (GtkWidget* button: MappedBindings(root)) {
		GtkWidget* row = gtk_widget_get_ancestor(button, ADW_TYPE_ACTION_ROW);
		if (row != nullptr && KytyUi::WidgetText(row) == title) {
			return button;
		}
	}
	KytyUi::Fail("no binding row titled \"" + std::string(title) + "\"");
	return nullptr;
}

KytyInputMappingWindow* OpenEditor(KytyUi::UiContext& ui, const core::GameEntry* entry) {
	auto* editor =
	    entry != nullptr
	        ? kyty_input_mapping_window_new_for_game(ui.Context(), ui.ParentWindow(), *entry)
	        : kyty_input_mapping_window_new_global(ui.Context(), ui.ParentWindow());
	return KYTY_INPUT_MAPPING_WINDOW(Present(GTK_WINDOW(editor), "the input mapping editor"));
}

// Clicks binding, waits for the capture dialog, and runs answer on it; the capture closes after.
bool Capture(GtkWidget* binding, const std::function<void(KytyBindingCaptureDialog*)>& answer) {
	if (!Check(KytyUi::Activate(binding), "a click on a binding")) {
		return false;
	}
	auto* capture = KYTY_BINDING_CAPTURE_DIALOG(
	    KytyUi::WaitForWindow(KYTY_TYPE_BINDING_CAPTURE_DIALOG, "the binding capture"));
	if (capture == nullptr) {
		return false;
	}
	GtkWindow* window = GTK_WINDOW(capture);
	answer(capture);
	return KytyUi::WaitClosed(window, "the capture to close with the input");
}

std::string Bound(KytyInputMappingWindow* editor, core::InputDevice device, const char* control) {
	const auto& model    = kyty_input_mapping_window_get_model(editor);
	const auto& bindings = device == core::InputDevice::Pad ? model.pad : model.keyboard;
	const auto  found    = bindings.find(control);
	return found != bindings.end() ? found->second : "";
}

bool SaveEditor(KytyInputMappingWindow* editor) {
	GtkWindow* window = GTK_WINDOW(editor);
	return Check(KytyUi::Activate(Widget(GTK_WIDGET(editor), "save_button")), "a click on Save") &&
	       KytyUi::WaitClosed(window, "Save to close the editor");
}

// ---- Save data

// Fails for every save file (*.bin) with EACCES; everything else is removed on the host.
class FailingSaveFiles final: public core::PathRemover {
public:
	std::error_code Remove(const fs::path& path) override {
		if (path.extension() == ".bin") {
			return std::make_error_code(std::errc::permission_denied);
		}
		return core::HostPathRemover().Remove(path);
	}
};

} // namespace

// ---- Confirmation flows

// Clear game config (game.clear-config, 9.8): the question names the game; Cancel keeps Kyty.ini
// byte for byte, Clear game config removes the game's custom settings.
KYTY_UI_CASE(content_clear_config_confirm) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const auto alpha = Entry(games, kAlphaTitle);
	if (!KytyUi::StoreSettings(ui, [&alpha](core::LauncherSettings& settings) {
		    auto config             = core::EnableCustomSettings(settings, alpha);
		    config.vblank_frequency = 120;
		    const auto saved        = core::SaveCustomSettings(settings, alpha, config);
		    Check(static_cast<bool>(saved), "store Alpha Game's custom settings");
	    })) {
		return;
	}
	auto&      context = ui.Context();
	GtkWindow* parent  = ui.ParentWindow();
	const auto before  = KytyUi::ReadText(ui.SettingsFile());
	int        changed = 0;
	const auto clear   = [&]() {
		kyty::gtk::ClearGameConfig(context, parent, alpha, nullptr, [&changed]() { changed++; });
	};
	clear();
	if (GtkWindow* question = KytyUi::FindAlert("Clear the game config?"); question != nullptr) {
		Check(HasText(GTK_WIDGET(question), "Alpha Game (PPSA01001) will use the global settings."),
		      "the question names the game: " + KytyUi::AllText(GTK_WIDGET(question)));
		Check(KytyUi::Capture(question, "question"), "the question renders");
		KytyUi::CloseAlert(question, "Cancel");
	}
	KytyUi::Pump(200);
	Check(KytyUi::ReadText(ui.SettingsFile()) == before && changed == 0,
	      "Cancel leaves Kyty.ini byte for byte");
	clear();
	if (GtkWindow* question = KytyUi::FindAlert("Clear the game config?"); question != nullptr) {
		KytyUi::CloseAlert(question, "Clear game config");
	}
	KytyUi::WaitFor([&changed]() { return changed == 1; }, 3000, "the clear to be saved");
	Check(!core::HasCustomSettings(KytyUi::StoredSettings(ui), alpha),
	      "Kyty.ini has no custom settings for Alpha Game");
	Check(!core::HasCustomSettings(context.settings.loaded.settings, alpha),
	      "the launcher's settings have none either");
}

// Remove save data (11.13): the question names the game and lists every directory; Cancel removes
// nothing; with failures injected, every path that could not be deleted is listed with its reason;
// another title's save data stays.
KYTY_UI_CASE(content_remove_save_data_confirm) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path saves = ui.AppDirectory() / "_SaveData" / "PPSA01001";
	const fs::path other = ui.AppDirectory() / "_SaveData" / "PPSA01002";
	for (int i = 1; i <= 3; i++) {
		KytyUi::WriteText(saves / "1" / ("slot" + std::to_string(i) + ".bin"), "save");
	}
	KytyUi::WriteText(saves / "1" / "sce_sys" / "param.sfo", "param");
	KytyUi::WriteText(other / "1" / "data.bin", "other save");
	auto&            context = ui.Context();
	GtkWindow*       parent  = ui.ParentWindow();
	const auto       alpha   = Entry(games, kAlphaTitle);
	FailingSaveFiles remover;
	context.path_remover = &remover;

	kyty::gtk::RemoveGameSaveData(context, parent, alpha, nullptr);
	if (GtkWindow* question = KytyUi::FindAlert("Remove the save data of Alpha Game?");
	    question != nullptr) {
		Check(HasText(GTK_WIDGET(question), saves.string()) &&
		          HasText(GTK_WIDGET(question), "This cannot be undone."),
		      "the question lists the save data directory: " +
		          KytyUi::AllText(GTK_WIDGET(question)));
		KytyUi::CloseAlert(question, "Cancel");
	}
	KytyUi::Pump(200);
	Check(KytyUi::Exists(saves / "1" / "slot1.bin") && KytyUi::Exists(saves / "1" / "sce_sys"),
	      "Cancel removes nothing");

	kyty::gtk::RemoveGameSaveData(context, parent, alpha, nullptr);
	if (GtkWindow* question = KytyUi::FindAlert("Remove the save data of Alpha Game?");
	    question != nullptr) {
		KytyUi::CloseAlert(question, "Remove save data");
	}
	if (GtkWindow* failures =
	        KytyUi::FindAlert("Some save data of Alpha Game could not be removed");
	    failures != nullptr) {
		const std::string text = KytyUi::AllText(GTK_WIDGET(failures));
		for (int i = 1; i <= 3; i++) {
			const auto path = saves / "1" / ("slot" + std::to_string(i) + ".bin");
			Check(text.find(path.string() + ": Permission denied") != std::string::npos,
			      "the failure list names " + path.string() + " and its reason");
		}
		Check(text.find("Could not remove:") != std::string::npos, "the list's body");
		Check(KytyUi::Capture(failures, "failures"), "the failure list renders");
		KytyUi::CloseAlert(failures, "_Close");
	}
	Check(!KytyUi::Exists(saves / "1" / "sce_sys") && KytyUi::Exists(saves / "1" / "slot1.bin"),
	      "what could be deleted is gone, the failed files stay");
	Check(KytyUi::ReadText(other / "1" / "data.bin") == "other save",
	      "Beta Quest's save data is unchanged");
	context.path_remover = &core::HostPathRemover();
}

// Import to Local over an existing local file (11.5): "Replace local cheats?"; Cancel keeps the
// file; Replace writes the remote file with every cheat off and shows the Local view.
KYTY_UI_CASE(content_cheats_import_confirm) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	ServeAlphaFile(ui, gen::CheatDocumentText(gen::RemoteCheatSpec(
	                       "PPSA01001", "01.000", "eboot.bin", {"Speed Run", "Low Gravity"})));
	const fs::path    data   = ui.AppDirectory();
	const fs::path    local  = core::CheatFilePath(data, "PPSA01001");
	const std::string before = KytyUi::ReadText(local);
	GtkWindow*        cheats = OpenCheats(ui, Entry(games, kAlphaTitle));
	if (cheats == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(cheats);
	adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(Widget(root, "stack")), "remote");
	if (!WaitForRemoteList(root)) {
		return;
	}
	GtkWidget* row = FileRow(root, ", PPSA01001_01.000_alpha.json");
	gtk_list_box_select_row(GTK_LIST_BOX(Widget(root, "files_list")), GTK_LIST_BOX_ROW(row));
	GtkWidget* import = Widget(root, "import_button");
	KytyUi::WaitFor([import]() { return gtk_widget_get_sensitive(import) != FALSE; }, 5000,
	                "the preview to enable Import to Local");
	Check(FirstNamed(Widget(root, "preview_list"), ADW_TYPE_ACTION_ROW, "Speed Run") != nullptr,
	      "the preview lists the remote cheats");
	const auto import_once = [&](const char* answer) {
		Check(KytyUi::Activate(import), "a click on Import to Local");
		if (GtkWindow* question = KytyUi::FindAlert("Replace local cheats?"); question != nullptr) {
			Check(HasText(GTK_WIDGET(question), "Replace the local cheat file and selections for "
			                                    "PPSA01001? Imported cheats will start unchecked."),
			      "the question names the title ID");
			KytyUi::CloseAlert(question, answer);
		}
	};
	import_once("Cancel");
	KytyUi::Pump(200);
	Check(KytyUi::ReadText(local) == before, "Cancel leaves the local file byte for byte");
	Check(KytyUi::VisiblePage(root) == "remote", "the window stays on the Remote view");
	import_once("Replace");
	KytyUi::WaitFor([root]() { return KytyUi::VisiblePage(root) == "local"; }, 3000,
	                "the Local view after the import");
	using States = std::vector<std::pair<std::string, bool>>;
	Check(StoredCheats(data) == States {{"Speed Run", false}, {"Low Gravity", false}},
	      "the local file has the remote cheats, every one off");
	Check(LabelText(root, "local_status") == "Imported 2 cheats. Select cheats and apply your "
	                                         "selection for the next game launch.",
	      "the status line counts the imported cheats: " + LabelText(root, "local_status"));
	Check(KytyUi::Capture(cheats, "imported"), "the cheats window renders");
	KytyUi::CloseWithEscape(cheats, "the cheats window");
}

// Compression finished (11.11): the question's default button is Keep folder, which keeps the
// folder; the archive is listed after the rescan next to the kept folder; Delete folder removes
// the original.
KYTY_UI_CASE(content_compress_delete_original) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&games](KytyApplication* application, KytyMainWindow* window) {
		auto&          context    = KytyUi::ContextOf(application);
		auto&          controller = kyty::gtk::MainWindowLibrary(window);
		const fs::path beta       = games / "Beta" / "Beta Quest";
		KytyUi::SelectTitle(context.library, kBetaTitle);
		auto scans = controller.Stats().scans_applied;
		KytyUi::ActivateGameAction(window, "compress");
		GtkWindow* dialog =
		    KytyUi::WaitForWindow(KYTY_TYPE_COMPRESSION_DIALOG, "the compression dialog");
		if (GtkWindow* question = KytyUi::FindAlert("Delete the original folder?", 20000);
		    question != nullptr && dialog != nullptr) {
			Check(HasText(GTK_WIDGET(question), "Delete the original folder " + beta.string()),
			      "the question names the folder");
			GtkWidget* keep = KytyUi::ButtonLabelled(GTK_WIDGET(question), {"Keep folder"});
			Check(keep != nullptr && gtk_window_get_default_widget(question) == keep,
			      "Keep folder is the default button");
			Check(HasText(GTK_WIDGET(dialog), "Compression finished"),
			      "the dialog shows that the compression finished");
			Check(KytyUi::Capture(dialog, "finished"), "the finished dialog renders");
			KytyUi::CloseAlert(question, "Keep folder");
		}
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > scans; }, 8000,
		                "the rescan after the compression");
		Check(KytyUi::Exists(beta) && KytyUi::Exists(games / "Beta" / "Beta Quest.zar"),
		      "Keep folder keeps the folder next to the archive");
		int listed = 0;
		for (const auto& title: KytyUi::ShownTitles(context.library)) {
			listed += title == kBetaTitle ? 1 : 0;
		}
		Check(listed == 2, "the library lists the folder and the archive of Beta Quest (" +
		                       std::to_string(listed) + ")");
		// Delete folder (Alpha Game).
		KytyUi::SelectTitle(context.library, kAlphaTitle);
		scans = controller.Stats().scans_applied;
		KytyUi::ActivateGameAction(window, "compress");
		if (GtkWindow* question = KytyUi::FindAlert("Delete the original folder?", 20000);
		    question != nullptr) {
			KytyUi::CloseAlert(question, "Delete folder");
		}
		KytyUi::WaitFor([&]() { return controller.Stats().scans_applied > scans; }, 8000,
		                "the rescan after the second compression");
		Check(!KytyUi::Exists(games / "Alpha Game") && KytyUi::Exists(games / "Alpha Game.zar"),
		      "Delete folder removes the original folder");
		KytyGame* alpha = KytyUi::GameByTitle(context.library, kAlphaTitle);
		Check(alpha != nullptr && kyty_game_get_entry(alpha).kind == core::GameEntry::Kind::Archive,
		      "the library lists Alpha Game as its archive");
	});
}

// Import game config from a file exported for another title (9.9): the question names both; Cancel
// changes nothing, Import gives the game that configuration as its custom settings.
KYTY_UI_CASE(content_import_settings_other_title) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const auto alpha = Entry(games, kAlphaTitle);
	const auto beta  = Entry(games, kBetaTitle);
	if (!KytyUi::StoreSettings(ui, [&beta](core::LauncherSettings& settings) {
		    auto config             = core::EnableCustomSettings(settings, beta);
		    config.vblank_frequency = 144;
		    Check(static_cast<bool>(core::SaveCustomSettings(settings, beta, config)),
		          "store Beta Quest's custom settings");
	    })) {
		return;
	}
	auto&          context  = ui.Context();
	GtkWindow*     parent   = ui.ParentWindow();
	auto&          chooser  = ui.Services().file_chooser;
	const fs::path exported = ui.Root() / "PPSA01002.json";
	chooser.file_answers.push_back(exported);
	kyty::gtk::ExportGameConfig(context, parent, beta, nullptr);
	KytyUi::WaitFor([&exported]() { return KytyUi::Exists(exported); }, 3000,
	                "Beta Quest's game config to be exported");
	const auto before  = KytyUi::ReadText(ui.SettingsFile());
	int        changed = 0;
	const auto import  = [&](const char* answer) {
		chooser.file_answers.push_back(exported);
		kyty::gtk::ImportGameConfig(context, parent, alpha, nullptr, [&changed]() { changed++; });
		if (GtkWindow* question = KytyUi::FindAlert("Import the config of another game?");
		    question != nullptr) {
			Check(HasText(GTK_WIDGET(question), "PPSA01002") &&
			          HasText(GTK_WIDGET(question), "Alpha Game (PPSA01001)"),
			      "the question names both games: " + KytyUi::AllText(GTK_WIDGET(question)));
			KytyUi::CloseAlert(question, answer);
		}
	};
	import("Cancel");
	KytyUi::Pump(200);
	Check(KytyUi::ReadText(ui.SettingsFile()) == before && changed == 0,
	      "Cancel leaves Kyty.ini byte for byte");
	import("Import");
	KytyUi::WaitFor([&changed]() { return changed == 1; }, 3000, "the import to be saved");
	const auto  stored = KytyUi::StoredSettings(ui);
	const auto* game   = core::FindGameSettings(stored, alpha);
	Check(game != nullptr && game->configuration.vblank_frequency == 144,
	      "Alpha Game has Beta Quest's configuration as its custom settings");
}

// ---- Trophies

// The trophy window (11.1): every trophy of the readable package with its name, description,
// grade, and state; the hidden locked trophy masked until Reveal; the unreadable package named.
KYTY_UI_CASE(content_trophy_viewer) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty() ||
	    !KytyUi::WriteText(games / "Alpha Game" / "sce_sys" / "trophy2" / "trophy01.ucp",
	                       "not a trophy package")) {
		return;
	}
	auto* window =
	    kyty_trophy_window_new(ui.Context(), ui.ParentWindow(), Entry(games, kAlphaTitle));
	// Tall enough for every row (p6-u9-3: the masked and revealed rows were cropped).
	gtk_window_set_default_size(GTK_WINDOW(window), 900, 860);
	auto* trophies = KYTY_TROPHY_WINDOW(Present(GTK_WINDOW(window), "the trophy window"));
	if (trophies == nullptr ||
	    !KytyUi::WaitFor([trophies]() { return kyty_trophy_window_is_loaded(trophies); }, 5000,
	                     "the trophies to be read")) {
		return;
	}
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(trophies));
	GtkWidget*        root = GTK_WIDGET(trophies);
	const std::string text = KytyUi::AllText(root);
	Check(text.find("Could not read trophy package trophy01.ucp.") != std::string::npos,
	      "the unreadable package is named");
	for (const char* shown:
	     {"Alpha Game Trophies", "Gold Rush", "Collect 1,000 coins.", "Silver Lining",
	      "Survive the storm.", "Gold", "Silver", "Earned", "Hidden trophy"}) {
		Check(text.find(shown) != std::string::npos, std::string("the window shows ") + shown);
	}
	Check(text.find("Secret Ending") == std::string::npos &&
	          text.find("Reach the secret ending.") == std::string::npos,
	      "the hidden locked trophy's name and description are masked");
	Check(FirstNamed(root, GTK_TYPE_LIST_BOX_ROW, "Gold Rush, Gold, earned") != nullptr &&
	          FirstNamed(root, GTK_TYPE_LIST_BOX_ROW, "Silver Lining, Silver, not earned") !=
	              nullptr &&
	          FirstNamed(root, GTK_TYPE_LIST_BOX_ROW, "Hidden trophy, not earned") != nullptr,
	      "each row names its trophy, grade, and state");
	Check(KytyUi::Capture(GTK_WINDOW(trophies), "masked"), "the masked state renders");
	GtkWidget* reveal = KytyUi::ButtonLabelled(root, {"_Reveal", "Reveal"});
	Check(KytyUi::Activate(reveal), "a click on Reveal");
	KytyUi::WaitFor([root]() { return HasText(root, "Reach the secret ending."); }, 2000,
	                "Reveal to show the hidden trophy");
	Check(HasText(root, "Secret Ending") && HasText(root, "Bronze"),
	      "the revealed trophy shows its name and grade");
	KytyUi::WaitForPaintedFrame(root);
	Check(KytyUi::Capture(GTK_WINDOW(trophies), "revealed"), "the revealed state renders");
	KytyUi::CloseWithEscape(GTK_WINDOW(trophies), "the trophy window");
}

// The trophy details dialog (LFI-25): Enter on a row; the earned trophy's state and details, and
// the masked trophy's Reveal, which reveals the row too.
KYTY_UI_CASE(content_trophy_details) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto* trophies =
	    KYTY_TROPHY_WINDOW(Present(GTK_WINDOW(kyty_trophy_window_new(
	                                   ui.Context(), ui.ParentWindow(), Entry(games, kAlphaTitle))),
	                               "the trophy window"));
	if (trophies == nullptr ||
	    !KytyUi::WaitFor([trophies]() { return kyty_trophy_window_is_loaded(trophies); }, 5000,
	                     "the trophies to be read")) {
		return;
	}
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(trophies));
	GtkWidget* root    = GTK_WIDGET(trophies);
	const auto details = [&](std::string_view row_name) -> GtkWindow* {
		GtkWidget* row = FirstNamed(root, GTK_TYPE_LIST_BOX_ROW, row_name);
		if (!Check(row != nullptr, "the row " + std::string(row_name))) {
			return nullptr;
		}
		gtk_widget_grab_focus(row);
		KytyUi::KeyPress(root, GDK_KEY_Return);
		return KytyUi::WaitForWindow(KYTY_TYPE_TROPHY_DETAILS_DIALOG, "the trophy details");
	};
	if (GtkWindow* dialog = details("Gold Rush, "); dialog != nullptr) {
		const std::string text = KytyUi::AllText(GTK_WIDGET(dialog));
		Check(text.find("Gold Rush") != std::string::npos &&
		          text.find("Gold") != std::string::npos &&
		          text.find("Earned") != std::string::npos &&
		          text.find("Collect 1,000 coins.") != std::string::npos,
		      "the details of the earned trophy: " + text);
		Check(KytyUi::Capture(dialog, "earned"), "the details render");
		KytyUi::CloseWithEscape(dialog, "the trophy details");
	}
	if (GtkWindow* dialog = details("Hidden trophy, "); dialog != nullptr) {
		const std::string text = KytyUi::AllText(GTK_WIDGET(dialog));
		Check(text.find("Not earned") != std::string::npos &&
		          text.find("Secret Ending") == std::string::npos,
		      "the masked trophy's details hide it: " + text);
		Check(KytyUi::Capture(dialog, "masked"), "the masked details render");
		GtkWidget* reveal = Widget(GTK_WIDGET(dialog), "reveal_button");
		Check(KytyUi::Activate(reveal), "a click on the details' Reveal");
		KytyUi::WaitFor([dialog]() { return HasText(GTK_WIDGET(dialog), "Secret Ending"); }, 2000,
		                "the details' Reveal to show the name");
		KytyUi::WaitFor([root]() { return HasText(root, "Secret Ending"); }, 2000,
		                "the row to be revealed too");
		if (KytyUi::IsToplevel(dialog)) {
			KytyUi::CloseWithEscape(dialog, "the trophy details");
		}
	}
	KytyUi::CloseWithEscape(GTK_WINDOW(trophies), "the trophy window");
}

// The trophy overview (11.2): one row per game with trophy data (its name, or its title ID when
// the name is empty, title ID, earned and total, integer percentage) and none for the others; the
// totals over every game.
KYTY_UI_CASE(content_trophy_overview) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path data     = ui.AppDirectory();
	const fs::path nameless = games / "Nameless";
	if (!WriteTrophies(games / "Beta" / "Beta Quest", "PPSA01002", data, "Beta Trophies",
	                   {Trophy(0, 'P', "Beta Platinum"), Trophy(1, 'G', "Beta Gold"),
	                    Trophy(2, 'B', "Beta Bronze")},
	                   {1, 2}) ||
	    !KytyUi::WriteGameWithParam(nameless,
	                                KytyUi::ParamJson("Nameless", "PPSA01009", "01.000")) ||
	    !WriteTrophies(nameless, "PPSA01009", data, "Nameless Trophies",
	                   {Trophy(1, 'S', "One"), Trophy(2, 'S', "Two")}, {})) {
		return;
	}
	auto&           context = ui.Context();
	auto            empty   = Entry(games, kAlphaTitle);
	core::GameEntry no_name;
	no_name.path     = nameless;
	no_name.title_id = "PPSA01009";
	core::GameEntry gamma;
	gamma.path       = games / "Gamma Archive.zar";
	gamma.kind       = core::GameEntry::Kind::Archive;
	gamma.title_id   = "PPSA01004";
	gamma.title_name = "Gamma Archive";
	FillLibrary(context, {Entry(games, kAlphaTitle), Entry(games, kBetaTitle), no_name, gamma});
	static_cast<void>(empty);
	auto* overview = KYTY_TROPHY_OVERVIEW_WINDOW(
	    Present(GTK_WINDOW(kyty_trophy_overview_window_new(context, ui.ParentWindow())),
	            "the trophy overview"));
	if (overview == nullptr ||
	    !KytyUi::WaitFor([overview]() { return kyty_trophy_overview_window_is_loaded(overview); },
	                     5000, "the overview to be read")) {
		return;
	}
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(overview));
	GtkWidget*               root = GTK_WIDGET(overview);
	const auto               rows = Named(root, GTK_TYPE_LIST_BOX_ROW, "");
	std::vector<std::string> names;
	for (GtkWidget* row: rows) {
		names.push_back(KytyUi::ControlName(row));
	}
	Check(rows.size() == 3, "three games have trophy data (" + std::to_string(rows.size()) + ")");
	const auto row_text = [&](std::size_t i) {
		return i < rows.size() ? KytyUi::AllText(rows[i]) : std::string();
	};
	Check(names.size() == 3 && names[0].starts_with("Alpha Game, ") &&
	          names[1].starts_with("Beta Quest, ") && names[2].starts_with("PPSA01009, "),
	      "the rows in library order; a game without a name shows its title ID");
	Check(row_text(0).find("PPSA01001") != std::string::npos &&
	          row_text(0).find("Earned 1/3") != std::string::npos,
	      "Alpha Game: its title ID and Earned 1/3: " + row_text(0));
	Check(row_text(1).find("PPSA01002") != std::string::npos &&
	          row_text(1).find("Earned 2/3") != std::string::npos,
	      "Beta Quest: Earned 2/3: " + row_text(1));
	Check(row_text(2).find("Earned 0/2") != std::string::npos &&
	          row_text(2).find("0%") != std::string::npos,
	      "the nameless game: Earned 0/2, 0%: " + row_text(2));
	for (const auto& name: names) {
		Check(name.find("Gamma Archive") == std::string::npos,
		      "no row for a game without trophies");
	}
	Check(LabelText(root, "total_label") == "Total 3",
	      "the total over every game: " + LabelText(root, "total_label"));
	Check(KytyUi::Capture(GTK_WINDOW(overview), "games"), "the overview renders");
	KytyUi::CloseWithEscape(GTK_WINDOW(overview), "the trophy overview");

	// No game with trophy data: the empty state.
	FillLibrary(context, {gamma});
	auto* none = KYTY_TROPHY_OVERVIEW_WINDOW(
	    Present(GTK_WINDOW(kyty_trophy_overview_window_new(context, ui.ParentWindow())),
	            "the empty trophy overview"));
	if (none != nullptr &&
	    KytyUi::WaitFor([none]() { return kyty_trophy_overview_window_is_loaded(none); }, 5000,
	                    "the empty overview to be read")) {
		Check(HasText(GTK_WIDGET(none), "No games with trophies found in the game folders."),
		      "the overview says that no game has trophies");
		Check(KytyUi::Capture(GTK_WINDOW(none), "empty"), "the empty overview renders");
		KytyUi::CloseWithEscape(GTK_WINDOW(none), "the empty trophy overview");
	}
}

// W4-20 (p6-u9-5): the progress block and every grade's count start at the same x in each row,
// whether or not the game has a Platinum trophy.
KYTY_UI_CASE(content_trophy_overview_alignment) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty() ||
	    !WriteTrophies(games / "Beta" / "Beta Quest", "PPSA01002", ui.AppDirectory(),
	                   "Beta Trophies",
	                   {Trophy(0, 'P', "Beta Platinum"), Trophy(1, 'G', "Beta Gold"),
	                    Trophy(2, 'S', "Beta Silver"), Trophy(3, 'B', "Beta Bronze")},
	                   {0, 1, 2, 3})) {
		return;
	}
	auto& context = ui.Context();
	FillLibrary(context, {Entry(games, kAlphaTitle), Entry(games, kBetaTitle)});
	auto* overview = KYTY_TROPHY_OVERVIEW_WINDOW(
	    Present(GTK_WINDOW(kyty_trophy_overview_window_new(context, ui.ParentWindow())),
	            "the trophy overview"));
	if (overview == nullptr ||
	    !KytyUi::WaitFor([overview]() { return kyty_trophy_overview_window_is_loaded(overview); },
	                     5000, "the overview to be read")) {
		return;
	}
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(overview));
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(overview));
	GtkWidget* root = GTK_WIDGET(overview);
	const auto rows = Named(root, GTK_TYPE_LIST_BOX_ROW, "");
	if (!Check(rows.size() == 2, "two rows")) {
		return;
	}
	// Each row's box: the art, the names, the progress block, then four grade columns.
	std::vector<std::vector<float>> columns;
	std::vector<std::vector<int>>   widths;
	for (GtkWidget* row: rows) {
		GtkWidget*         box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
		std::vector<float> x;
		std::vector<int>   w;
		int                index = 0;
		for (GtkWidget* child = gtk_widget_get_first_child(box); child != nullptr;
		     child            = gtk_widget_get_next_sibling(child), index++) {
			if (index >= 2) {
				x.push_back(PositionIn(child, root).x);
				w.push_back(gtk_widget_get_width(child));
			}
		}
		columns.push_back(x);
		widths.push_back(w);
	}
	for (std::size_t r = 0; r < columns.size(); r++) {
		std::string line;
		for (std::size_t c = 0; c < columns[r].size(); c++) {
			line += " " + std::to_string(static_cast<int>(columns[r][c])) + "+" +
			        std::to_string(widths[r][c]);
		}
		std::printf("%s: row %zu columns (x+width):%s\n", KytyUi::CaseName().c_str(), r,
		            line.c_str());
	}
	Check(columns[0].size() == 5 && columns[1].size() == 5,
	      "each row has the progress block and four grade columns");
	Check(columns[0] == columns[1], "the columns start at the same x in both rows");
	Check(widths[0] == widths[1], "the columns are equally wide in both rows");
	Check(HasText(rows[1], "Beta Platinum") || KytyUi::ControlName(rows[1]).starts_with("Beta"),
	      "the second row is Beta Quest's");
	Check(KytyUi::Capture(GTK_WINDOW(overview), "aligned"), "the overview renders");
	KytyUi::CloseWithEscape(GTK_WINDOW(overview), "the trophy overview");
}

// W4-16 (p6-u9-1): closing the overview while it reads stops the read before the next game (the
// read's end is logged with the number of games read).
KYTY_UI_CASE(content_trophy_overview_cancel) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	constexpr int                kGames = 300;
	const fs::path               many   = ui.Root() / "many";
	std::vector<core::GameEntry> entries;
	for (int n = 1; n <= kGames; n++) {
		char title_id[16];
		std::snprintf(title_id, sizeof(title_id), "PPSA3%04d", n);
		core::GameEntry entry;
		entry.path       = many / ("Trophy Game " + std::to_string(n));
		entry.title_id   = title_id;
		entry.title_name = "Trophy Game " + std::to_string(n);
		if (!WriteTrophies(entry.path, title_id, ui.AppDirectory(), entry.title_name,
		                   {Trophy(1, 'G', "First"), Trophy(2, 'S', "Second")}, {1})) {
			return;
		}
		entries.push_back(entry);
	}
	auto& context = ui.Context();
	FillLibrary(context, entries);
	KytyUi::WatchLog("kyty-launcher", "trophy overview: read ");
	// Reading every game to the end, for comparison.
	auto* full = KYTY_TROPHY_OVERVIEW_WINDOW(
	    Present(GTK_WINDOW(kyty_trophy_overview_window_new(context, ui.ParentWindow())),
	            "the trophy overview"));
	if (full == nullptr ||
	    !KytyUi::WaitFor([full]() { return kyty_trophy_overview_window_is_loaded(full); }, 20000,
	                     "the overview of 300 games to be read")) {
		return;
	}
	Check(Named(GTK_WIDGET(full), GTK_TYPE_LIST_BOX_ROW, "Trophy Game ").size() > 0,
	      "the overview lists the games");
	KytyUi::CloseWithEscape(GTK_WINDOW(full), "the trophy overview");
	// Closed at once: the read stops early.
	GtkWindow* closed = GTK_WINDOW(kyty_trophy_overview_window_new(context, ui.ParentWindow()));
	gtk_window_destroy(closed);
	KytyUi::WaitFor([]() { return KytyUi::WatchedLogs("trophy overview: read ").size() == 2; },
	                20000, "the second read to end");
	const auto logs = KytyUi::WatchedLogs("trophy overview: read ");
	for (const auto& log: logs) {
		std::printf("%s: %s\n", KytyUi::CaseName().c_str(), log.c_str());
	}
	if (Check(logs.size() == 2, "two reads were logged")) {
		Check(logs[0] == "trophy overview: read 300 of 300 game(s)",
		      "the open overview read every game: " + logs[0]);
		Check(logs[1].ends_with(" of 300 game(s), cancelled") &&
		          logs[1] != "trophy overview: read 300 of 300 game(s), cancelled",
		      "the closed overview's read stopped before the last game: " + logs[1]);
	}
}

// ---- Add-ons

// The additional content window (11.3): the add-ons whose title ID is the game's (case and
// surrounding white space ignored), each with its title or, without one, its entitlement label;
// the count; then the folder not set, missing, and without add-ons of the game.
KYTY_UI_CASE(content_addons_list) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path addons = ui.Root() / "addons";
	if (!KytyUi::WriteText(
	        addons / "alpha-bonus" / "sce_sys" / "param.json",
	        gen::AddOnParamJson(gen::AddOnContentId("PPSA01001", "ALPHABONUSPACK01"), "")) ||
	    !KytyUi::WriteText(addons / "beta-pack" / "sce_sys" / "param.json",
	                       gen::AddOnParamJson(gen::AddOnContentId("PPSA01002", "BETAEXPANSION001"),
	                                           "Beta Pack"))) {
		return;
	}
	auto& context  = ui.Context();
	auto  alpha    = Entry(games, kAlphaTitle);
	alpha.title_id = " ppsa01001 ";
	auto* window   = KYTY_ADDITIONAL_CONTENT_WINDOW(
	    Present(GTK_WINDOW(kyty_additional_content_window_new(context, ui.ParentWindow(), alpha)),
	            "the additional content window"));
	if (window == nullptr) {
		return;
	}
	GtkWidget* root   = GTK_WIDGET(window);
	const auto loaded = [window]() { return kyty_additional_content_window_is_loaded(window); };
	KytyUi::WaitFor(loaded, 5000, "the add-ons to be listed");
	KytyUi::WaitForPaintedFrame(root);
	const auto               rows = Named(root, ADW_TYPE_ACTION_ROW, "");
	std::vector<std::string> titles;
	for (GtkWidget* row: rows) {
		titles.push_back(KytyUi::WidgetText(row));
	}
	std::sort(titles.begin(), titles.end());
	Check(titles == std::vector<std::string> {"ALPHABONUSPACK01", "Alpha Soundtrack"},
	      "Alpha Game's two add-ons, one by its entitlement label, none of Beta Quest");
	Check(LabelText(root, "status_label") == "2 installed add-ons",
	      "the status counts them: " + LabelText(root, "status_label"));
	Check(KytyUi::Capture(GTK_WINDOW(window), "listed"), "the listed add-ons render");
	struct State {
		std::string folder;
		std::string status;
		const char* capture;
	};
	const fs::path  empty_folder = ui.Root() / "empty-addons";
	std::error_code ec;
	fs::create_directories(empty_folder, ec);
	for (const auto& state: std::vector<State> {
	         {"",
	          "No add-ons folder is set. Open Global settings and set the Add-ons folder to the "
	          "location of your extracted add-on packages.",
	          "not-set"},
	         {(ui.Root() / "missing").string(),
	          "The add-ons folder does not exist:\n" + (ui.Root() / "missing").string() +
	              "\nCheck the path in Global settings.",
	          "missing"},
	         {empty_folder.string(), "No add-ons found for this game in:\n" + empty_folder.string(),
	          "none"}}) {
		context.settings.loaded.settings.addons_dir = state.folder;
		Check(KytyUi::Activate(Widget(root, "refresh_button")), "a click on Refresh");
		KytyUi::WaitFor(loaded, 5000, "Refresh to list again");
		Check(LabelText(root, "status_label") == state.status, std::string("the status of ") +
		                                                           state.capture + ": " +
		                                                           LabelText(root, "status_label"));
		Check(Named(root, ADW_TYPE_ACTION_ROW, "").empty(), "no add-on is listed");
		Check(gtk_widget_get_sensitive(Widget(root, "open_button")) ==
		          (state.capture == std::string_view("none") ? TRUE : FALSE),
		      "Open folder only while the folder exists");
		Check(KytyUi::Capture(GTK_WINDOW(window), state.capture), "the state renders");
	}
	KytyUi::CloseWithEscape(GTK_WINDOW(window), "the additional content window");
}

// W4-27 (FEAT-010 (5), FEAT-012 (11)): Refresh while an add-on row has the focus rebuilds the list
// without a critical (the focus leaves the list before its rows go) and leaves the focus on
// Refresh.
KYTY_UI_CASE(content_addons_refresh_focused) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto* window = KYTY_ADDITIONAL_CONTENT_WINDOW(
	    Present(GTK_WINDOW(kyty_additional_content_window_new(ui.Context(), ui.ParentWindow(),
	                                                          Entry(games, kAlphaTitle))),
	            "the additional content window"));
	if (window == nullptr) {
		return;
	}
	GtkWidget* root   = GTK_WIDGET(window);
	const auto loaded = [window]() { return kyty_additional_content_window_is_loaded(window); };
	for (int round = 1; round <= 3; round++) {
		KytyUi::WaitFor(loaded, 5000, "the add-ons to be listed");
		KytyUi::WaitForPaintedFrame(root);
		GtkWidget* row = FirstNamed(root, ADW_TYPE_ACTION_ROW, "Alpha Soundtrack");
		if (!Check(row != nullptr && gtk_widget_grab_focus(row) != FALSE &&
		               gtk_root_get_focus(GTK_ROOT(window)) == row,
		           "the add-on row has the focus")) {
			return;
		}
		gtk_window_set_focus_visible(GTK_WINDOW(window), TRUE);
		// The mnemonic's effect: Refresh is clicked while the row keeps the focus.
		g_signal_emit_by_name(Widget(root, "refresh_button"), "clicked");
		KytyUi::WaitFor(loaded, 5000, "Refresh to list the add-on again");
		for (int frame = 0; frame < 3; frame++) {
			KytyUi::WaitForPaintedFrame(root);
		}
		GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window));
		std::printf("%s: round %d, the focus after Refresh: %s\n", KytyUi::CaseName().c_str(),
		            round, focus != nullptr ? KytyUi::WidgetPath(focus).c_str() : "(none)");
		Check(focus == Widget(root, "refresh_button"),
		      "the focus is on Refresh after the listing, round " + std::to_string(round));
	}
	Check(FirstNamed(root, ADW_TYPE_ACTION_ROW, "Alpha Soundtrack") != nullptr,
	      "the add-on is listed after the refreshes");
	KytyUi::CloseWithEscape(GTK_WINDOW(window), "the additional content window");
}

// ---- Compression

// The progress (11.8: a percentage that never decreases, Cancel offered while it runs), Cancel and
// Escape (11.10: the partial archive is deleted, the folder is unchanged, nothing is shown); the
// job is registered while its thread runs.
KYTY_UI_CASE(content_compress_progress_cancel) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto&          context = ui.Context();
	const auto     beta    = Entry(games, kBetaTitle);
	const fs::path archive = games / "Beta" / "Beta Quest.zar";
	const auto     source  = gen::SnapshotTree(beta.path);
	for (const bool escape: {false, true}) {
		const std::string how    = escape ? "Escape" : "Cancel";
		auto              calls  = std::make_shared<std::atomic<int>>(0);
		auto*             dialog = Present(
		    GTK_WINDOW(kyty_compression_dialog_new(context, ui.ParentWindow(), beta,
		                                           SlowPacker(400, 15, PackEnd::Succeed, calls))),
		    "the compression dialog");
		if (dialog == nullptr) {
			return;
		}
		GtkWidget* root   = GTK_WIDGET(dialog);
		GtkWidget* cancel = Widget(root, "cancel_button");
		Check(context.jobs.Count() == 1, "the compression is registered");
		Check(gtk_widget_get_visible(cancel) != FALSE && gtk_widget_get_sensitive(cancel) != FALSE,
		      "Cancel is offered while it runs");
		std::vector<double> seen;
		KytyUi::WaitFor(
		    [&]() {
			    seen.push_back(Fraction(dialog));
			    return seen.back() >= 0.1;
		    },
		    10000, "the progress to reach 10 %");
		Check(std::is_sorted(seen.begin(), seen.end()), "the progress never decreases");
		std::printf("%s: %s at %.0f %% after %zu samples\n", KytyUi::CaseName().c_str(),
		            how.c_str(), seen.back() * 100, seen.size());
		Check(HasText(root, "Compressing " + beta.path.string()), "the dialog names the folder");
		if (!escape) {
			Check(KytyUi::Capture(dialog, "progress"), "the progress renders");
			Check(KytyUi::Activate(cancel), "a click on Cancel");
		} else {
			KytyUi::KeyPressOn(root, GDK_KEY_Escape);
		}
		KytyUi::WaitClosed(dialog, how + " to close the dialog once the job stopped", 5000);
		KytyUi::Pump(200);
		Check(!KytyUi::Exists(archive), how + " deletes the partial archive");
		Check(gen::SnapshotTree(beta.path) == source, how + " leaves the folder unchanged");
		Check(calls->load() < 400, how + " stops the packer");
		Check(context.jobs.Count() == 0, "the job is unregistered once it stopped");
		Check(KytyUi::ToplevelCount() == 1, how + " shows no message");
	}
}

// 11.16: a packer that fails and an archive the verifier cannot open both show "Could not
// compress <folder>." and leave neither an archive nor a changed folder.
KYTY_UI_CASE(content_compress_verify_failed) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto&          context = ui.Context();
	const auto     beta    = Entry(games, kBetaTitle);
	const fs::path archive = games / "Beta" / "Beta Quest.zar";
	const auto     source  = gen::SnapshotTree(beta.path);
	for (const auto end: {PackEnd::NotAnArchive, PackEnd::Fail}) {
		const std::string name  = end == PackEnd::Fail ? "failed" : "verify-failed";
		auto              calls = std::make_shared<std::atomic<int>>(0);
		auto*             dialog =
		    Present(GTK_WINDOW(kyty_compression_dialog_new(context, ui.ParentWindow(), beta,
		                                                   SlowPacker(3, 10, end, calls))),
		            "the compression dialog");
		if (dialog == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(dialog);
		KytyUi::WaitFor([root]() { return HasText(root, "Compression failed"); }, 5000,
		                "the failure page (" + name + ")");
		Check(HasText(root, "Could not compress " + beta.path.string() + "."),
		      "the failure names the folder (" + name + ")");
		Check(kyty_compression_dialog_get_outcome(KYTY_COMPRESSION_DIALOG(dialog)) ==
		          (end == PackEnd::Fail ? core::ArchiveOutcome::Failed
		                                : core::ArchiveOutcome::VerifyFailed),
		      "the job's outcome (" + name + ")");
		Check(!KytyUi::Exists(archive), "no archive is left (" + name + ")");
		Check(gen::SnapshotTree(beta.path) == source, "the folder is unchanged (" + name + ")");
		Check(gtk_widget_get_visible(Widget(root, "close_button")) != FALSE &&
		          gtk_widget_get_visible(Widget(root, "cancel_button")) == FALSE,
		      "Close instead of Cancel");
		Check(KytyUi::Capture(dialog, name), "the failure renders");
		KytyUi::CloseWithEscape(dialog, "the compression dialog");
	}
}

// W4-17 (p6-u9-2): app.quit during a compression cancels it and waits until the job stopped, so
// neither the partial archive nor another file remains.
KYTY_UI_CASE(content_compress_quit_midway) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path big = games / "Big Game";
	if (!KytyUi::WriteGameWithParam(big, KytyUi::ParamJson("Big Game", "PPSA01020", "01.000")) ||
	    !WriteRandomFile(big / "data" / "payload.bin", std::size_t {64} << 20u)) {
		return;
	}
	const auto listing = Listing(games);
	const auto source  = gen::SnapshotTree(big);
	KytyUi::WatchLog("kyty-launcher", "background job");
	gint64    quit_us = 0;
	gint64    done_us = 0;
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		if (!KytyUi::WaitForLibrary(window)) {
			KytyUi::QuitApplication(application);
			return;
		}
		auto& context = KytyUi::ContextOf(application);
		KytyUi::SelectTitle(context.library, "Big Game");
		KytyUi::ActivateGameAction(window, "compress");
		GtkWindow* dialog =
		    KytyUi::WaitForWindow(KYTY_TYPE_COMPRESSION_DIALOG, "the compression dialog");
		if (dialog != nullptr) {
			KytyUi::WaitFor(
			    [dialog]() {
				    const double fraction = Fraction(dialog);
				    return fraction > 0.0 && fraction < 0.5;
			    },
			    10000, "the compression to be under way");
			std::printf("%s: app.quit at %.0f %% of the compression\n", KytyUi::CaseName().c_str(),
			            Fraction(dialog) * 100);
			Check(Fraction(dialog) < 0.5, "the compression is under way when the case quits");
			Check(context.jobs.Count() == 1, "the compression is registered");
		}
		quit_us = g_get_monotonic_time();
		KytyUi::QuitApplication(application);
		done_us = g_get_monotonic_time();
	});
	Check(status == 0, "the application ends with status 0");
	std::printf("%s: app.quit returned after %lld ms\n", KytyUi::CaseName().c_str(),
	            static_cast<long long>((done_us - quit_us) / 1000));
	for (const auto& log: KytyUi::WatchedLogs("background job")) {
		std::printf("%s: %s\n", KytyUi::CaseName().c_str(), log.c_str());
	}
	Check(done_us - quit_us < 10 * G_USEC_PER_SEC,
	      "app.quit waited " + std::to_string((done_us - quit_us) / 1000) + " ms (at most 10 s)");
	Check(!KytyUi::Exists(games / "Big Game.zar"), "no partial archive is left");
	Check(Listing(games) == listing, "no other file is left next to the game");
	Check(gen::SnapshotTree(big) == source, "the game folder is unchanged");
	const auto logs = KytyUi::WatchedLogs("background job");
	Check(std::find(logs.begin(), logs.end(),
	                "background job cancelled: compression of " + big.string()) != logs.end() &&
	          std::any_of(logs.begin(), logs.end(),
	                      [](const std::string& log) {
		                      return log.starts_with("background jobs stopped after ");
	                      }),
	      "the quit cancelled the compression and waited for it");
}

// ---- Cheats

// The Remote view (11.4): the credit link, Alpha Game's files with the installed version first,
// MC4 and SHN disabled, the count; W4-28: the Version column keeps its width while the list loads;
// W4-24: the window opens at 900x700 with room for the preview.
KYTY_UI_CASE(content_cheats_remote_list) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	// The JSON index answers after 1.5 s, so the list is seen while it loads.
	for (auto [url, body]: KytyUi::CheatIndexes()) {
		KytyTests::LoopbackResponse response;
		response.body         = std::move(body);
		response.content_type = "text/plain";
		response.delay_ms     = url == core::CheatIndexUrl("json") ? 1500 : 0;
		ui.Server().Script(url, std::move(response));
	}
	ServeAlphaFile(ui, gen::CheatDocumentText(gen::RemoteCheatSpec(
	                       "PPSA01001", "01.000", "eboot.bin", {"One", "Two", "Three", "Four"})));
	GtkWindow* cheats = OpenCheats(ui, Entry(games, kAlphaTitle));
	if (cheats == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(cheats);
	adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(Widget(root, "stack")), "remote");
	KytyUi::WaitForPaintedFrame(root);
	int width  = 0;
	int height = 0;
	gtk_window_get_default_size(cheats, &width, &height);
	Check(width == 900 && height == 700, "the window opens at 900x700");
	GtkWidget* version = Widget(root, "version_header");
	GtkWidget* format  = Widget(root, "format_header");
	Check(HasText(root, "Loading remote cheats…"), "the list is loading");
	const int loading_version = gtk_widget_get_width(version);
	const int loading_format  = gtk_widget_get_width(format);
	Check(KytyUi::Capture(cheats, "remote-loading"), "the loading list renders");
	if (!WaitForRemoteList(root)) {
		return;
	}
	KytyUi::WaitForPaintedFrame(root);
	Check(gtk_widget_get_width(version) == loading_version &&
	          gtk_widget_get_width(format) == loading_format,
	      "the Version and Format columns keep their widths (" + std::to_string(loading_version) +
	          " -> " + std::to_string(gtk_widget_get_width(version)) + ", " +
	          std::to_string(loading_format) + " -> " +
	          std::to_string(gtk_widget_get_width(format)) + ")");
	const auto rows = KytyUi::FindAll(
	    Widget(root, "files_list"), [](GtkWidget* widget) { return GTK_IS_LIST_BOX_ROW(widget); });
	std::vector<std::string> names;
	for (GtkWidget* row: rows) {
		names.push_back(KytyUi::ControlName(row));
	}
	Check(names.size() == 4 && names[0] == "01.000 (installed), JSON, PPSA01001_01.000_alpha.json",
	      "four files, the installed version's JSON file first: " +
	          (names.empty() ? std::string("none") : names[0]));
	int disabled = 0;
	for (GtkWidget* row: rows) {
		if (gtk_widget_get_sensitive(row) == FALSE) {
			disabled++;
			const char* tooltip = gtk_widget_get_tooltip_text(row);
			Check(tooltip != nullptr && std::string_view(tooltip).ends_with(" is not supported."),
			      "a disabled file says why");
		}
	}
	Check(disabled == 2, "the MC4 and SHN files are disabled");
	Check(LabelText(root, "remote_status") == "4 remote files for PPSA01001.",
	      "the status line: " + LabelText(root, "remote_status"));
	// The preview of the installed version's file (W4-24: every cheat visible).
	gtk_list_box_select_row(GTK_LIST_BOX(Widget(root, "files_list")), GTK_LIST_BOX_ROW(rows[0]));
	KytyUi::WaitFor(
	    [root]() { return gtk_widget_get_sensitive(Widget(root, "import_button")) != FALSE; }, 5000,
	    "the preview to load");
	KytyUi::WaitForPaintedFrame(root);
	const auto preview = Named(Widget(root, "preview_list"), ADW_TYPE_ACTION_ROW, "");
	Check(preview.size() == 4, "the preview lists the four cheats");
	GtkWidget* scrolled =
	    gtk_widget_get_ancestor(Widget(root, "preview_list"), GTK_TYPE_SCROLLED_WINDOW);
	Check(scrolled != nullptr && gtk_widget_get_height(scrolled) >= 170,
	      "the preview keeps its minimum height (" +
	          std::to_string(scrolled != nullptr ? gtk_widget_get_height(scrolled) : 0) + " px)");
	Check(KytyUi::Capture(cheats, "remote-preview"), "the preview renders");
	// The credit link opens the collection's page through the desktop launcher.
	gboolean handled = FALSE;
	g_signal_emit_by_name(Widget(root, "credit_label"), "activate-link", core::kHenCheatsRepository,
	                      &handled);
	const auto& calls = ui.Services().desktop_launcher.calls;
	KytyUi::WaitFor([&calls]() { return !calls.empty(); }, 3000, "the credit link to open");
	Check(handled != FALSE && !calls.empty() &&
	          calls.back() == std::string("OpenUri ") + core::kHenCheatsRepository,
	      "the credit link opens the HEN Cheats Collection");
	KytyUi::CloseWithEscape(cheats, "the cheats window");
}

// W4-25 (p6-u10-4, 11.6): a remote file that is not JSON: the error names the file, no cheat is
// listed, Import stays disabled, and the local file is unchanged.
KYTY_UI_CASE(content_cheats_remote_not_json) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	ServeAlphaFile(ui, "this is not JSON at all");
	const fs::path    local  = core::CheatFilePath(ui.AppDirectory(), "PPSA01001");
	const std::string before = KytyUi::ReadText(local);
	GtkWindow*        cheats = OpenCheats(ui, Entry(games, kAlphaTitle));
	if (cheats == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(cheats);
	adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(Widget(root, "stack")), "remote");
	if (!WaitForRemoteList(root)) {
		return;
	}
	gtk_list_box_select_row(GTK_LIST_BOX(Widget(root, "files_list")),
	                        GTK_LIST_BOX_ROW(FileRow(root, ", PPSA01001_01.000_alpha.json")));
	KytyUi::WaitFor(
	    [root]() {
		    return LabelText(root, "preview_status").starts_with("Could not preview this file: ");
	    },
	    5000, "the parse error");
	const std::string status = LabelText(root, "preview_status");
	Check(status.find("PPSA01001_01.000_alpha.json") != std::string::npos,
	      "the error names the file: " + status);
	Check(Named(Widget(root, "preview_list"), ADW_TYPE_ACTION_ROW, "").empty(),
	      "no cheat is listed");
	Check(gtk_widget_get_sensitive(Widget(root, "import_button")) == FALSE,
	      "Import to Local stays disabled");
	Check(KytyUi::ReadText(local) == before, "the local file is unchanged");
	Check(KytyUi::Capture(cheats, "not-json"), "the error renders");
	KytyUi::CloseWithEscape(cheats, "the cheats window");
}

// W4-22, W4-23 (p6-u10-1, p6-u10-2): the Local view without cheats names the file's state: no
// file, a file that cannot be read (a directory in its place; the reason names the path once), a
// file that is not valid, and a valid file without cheats.
KYTY_UI_CASE(content_cheats_local_states) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path local = core::CheatFilePath(ui.AppDirectory(), "PPSA01001");
	struct State {
		const char*           name;
		std::function<void()> prepare;
		std::string           title;
		std::string           status_start;
	};
	const auto remove = [local]() {
		std::error_code ec;
		fs::remove_all(local, ec);
	};
	const std::vector<State> states = {
	    {"no-file", remove, "No local cheats", "No local cheat file: " + local.string()},
	    {"unreadable",
	     [&]() {
		     remove();
		     std::error_code ec;
		     fs::create_directories(local, ec);
	     },
	     "The local cheat file cannot be read", "Could not read the local cheat file:\n"},
	    {"invalid",
	     [&]() {
		     remove();
		     KytyUi::WriteText(local, "{ not valid");
	     },
	     "The local cheat file is not valid", "The local cheat file is not valid:\n"},
	    {"no-cheats",
	     [&]() {
		     remove();
		     KytyUi::WriteText(local, gen::CheatDocumentText(gen::RemoteCheatSpec(
		                                  "PPSA01001", "01.000", "eboot.bin", {})));
	     },
	     "The local cheat file has no cheats", "Loaded 0 cheats from " + local.string()},
	};
	for (const auto& state: states) {
		state.prepare();
		GtkWindow* cheats = OpenCheats(ui, Entry(games, kAlphaTitle));
		if (cheats == nullptr) {
			return;
		}
		GtkWidget* root = GTK_WIDGET(cheats);
		adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(Widget(root, "stack")), "local");
		KytyUi::WaitForPaintedFrame(root);
		auto*             page   = ADW_STATUS_PAGE(Widget(root, "local_empty_page"));
		const std::string title  = adw_status_page_get_title(page);
		const std::string status = LabelText(root, "local_status");
		Check(title == state.title,
		      std::string(state.name) + ": the page's title is \"" + title + "\"");
		Check(gtk_widget_get_mapped(GTK_WIDGET(page)) != FALSE,
		      std::string(state.name) + ": the page is shown");
		Check(status.starts_with(state.status_start),
		      std::string(state.name) + ": the status line: " + status);
		const char* description = adw_status_page_get_description(page);
		Check(description != nullptr &&
		          (std::string_view(state.name) == "unreadable") ==
		              (std::string_view(description).find("Nothing can be imported") !=
		               std::string_view::npos),
		      std::string(state.name) + ": the description fits the state");
		if (std::string_view(state.name) == "unreadable") {
			std::size_t count = 0;
			for (auto at = status.find(local.string()); at != std::string::npos;
			     at      = status.find(local.string(), at + 1)) {
				count++;
			}
			Check(count == 1, "the unreadable file's status names its path once: " + status);
		}
		Check(gtk_widget_get_sensitive(Widget(root, "apply_button")) == FALSE,
		      std::string(state.name) + ": nothing to apply");
		Check(KytyUi::Capture(cheats, state.name), "the state renders");
		KytyUi::CloseWithEscape(cheats, "the cheats window");
	}
	std::error_code ec;
	fs::remove_all(local, ec);
}

// Apply selection, Enable all, Disable all, and a file that changed on disk (LFI-27, HC-33): the
// selection is written only over the bytes that were read; Reload reads the change.
KYTY_UI_CASE(content_cheats_apply_enable_disable) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const fs::path data   = ui.AppDirectory();
	const fs::path local  = core::CheatFilePath(data, "PPSA01001");
	GtkWindow*     cheats = OpenCheats(ui, Entry(games, kAlphaTitle));
	if (cheats == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(cheats);
	adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(Widget(root, "stack")), "local");
	KytyUi::WaitForPaintedFrame(root);
	using States      = std::vector<std::pair<std::string, bool>>;
	const auto checks = [root]() {
		std::vector<GtkCheckButton*> found;
		for (GtkWidget* widget: Named(root, GTK_TYPE_CHECK_BUTTON, "")) {
			found.push_back(GTK_CHECK_BUTTON(widget));
		}
		return found;
	};
	if (!Check(checks().size() == 2, "two local cheats")) {
		return;
	}
	Check(KytyUi::Activate(Widget(root, "enable_all_button")), "a click on Enable all");
	Check(KytyUi::Activate(Widget(root, "apply_button")), "a click on Apply selection");
	Check(StoredCheats(data) == States {{"Infinite Health", true}, {"Max Coins", true}},
	      "Apply writes both on");
	Check(LabelText(root, "local_status") ==
	          "Cheat selection saved. Changes take effect on the next game launch.",
	      "the status says it is saved");
	// The file changes on disk; Apply refuses, Reload reads the change.
	const std::string changed = gen::CheatDocumentText(
	    gen::RemoteCheatSpec("PPSA01001", "01.000", "eboot.bin", {"Edited Elsewhere"}));
	KytyUi::WriteText(local, changed);
	Check(KytyUi::Activate(Widget(root, "disable_all_button")), "a click on Disable all");
	Check(KytyUi::Activate(Widget(root, "apply_button")), "Apply after the change");
	Check(LabelText(root, "local_status") == "Cheat file changed; reload the dialog.",
	      "Apply refuses a changed file: " + LabelText(root, "local_status"));
	Check(KytyUi::ReadText(local) == changed, "the changed file stays as it is");
	Check(KytyUi::Activate(Widget(root, "reload_button")), "a click on Reload");
	KytyUi::WaitForPaintedFrame(root);
	Check(FirstNamed(root, ADW_TYPE_ACTION_ROW, "Edited Elsewhere") != nullptr &&
	          checks().size() == 1,
	      "Reload shows the changed file's cheat");
	Check(KytyUi::Capture(cheats, "local"), "the Local view renders");
	KytyUi::CloseWithEscape(cheats, "the cheats window");
}

// ---- Input mapping

// The global editor (11.14, LFI-24): a key, a mouse button, and a pad source are captured into
// three bindings; nothing is written before Save, and Save writes every change.
KYTY_UI_CASE(content_input_mapping_capture) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto* editor = OpenEditor(ui, nullptr);
	if (editor == nullptr) {
		return;
	}
	GtkWidget* root   = GTK_WIDGET(editor);
	const auto before = KytyUi::ReadText(ui.SettingsFile());
	kyty_input_mapping_window_show_tab(editor, "keyboard");
	KytyUi::WaitForPaintedFrame(root);
	Capture(BindingOf(root, "Cross"), [](KytyBindingCaptureDialog* capture) {
		Check(kyty_binding_capture_dialog_handle_key(capture, GDK_KEY_k, GdkModifierType(0)) !=
		          FALSE,
		      "the capture takes K");
	});
	Check(Bound(editor, core::InputDevice::KeyboardMouse, "Cross") == "K",
	      "Cross is bound to K (" + Bound(editor, core::InputDevice::KeyboardMouse, "Cross") + ")");
	Capture(BindingOf(root, "Circle"), [](KytyBindingCaptureDialog* capture) {
		kyty_binding_capture_dialog_handle_button(capture, 3);
	});
	const std::string mouse = Bound(editor, core::InputDevice::KeyboardMouse, "Circle");
	Check(mouse.find("Mouse") != std::string::npos,
	      "Circle is bound to a mouse button (" + mouse + ")");
	kyty_input_mapping_window_show_tab(editor, "gamepad");
	KytyUi::WaitForPaintedFrame(root);
	auto& controller = ui.Services().controller;
	// The pad capture's prompts without a usable controller (p6-u8-4); Escape keeps the binding.
	struct Prompt {
		const char*                capture;
		kyty::gtk::ControllerState state;
		std::string                text;
	};
	kyty::gtk::ControllerState paused;
	paused.paused = true;
	kyty::gtk::ControllerState broken;
	broken.error = "no gamepad subsystem";
	for (const auto& [name, state, text]: std::vector<Prompt> {
	         {"pad-none", {}, "No controller connected.\nConnect one, or press Esc to cancel."},
	         {"pad-paused", paused,
	          "Controller input is paused while a game is running.\nPress Esc to cancel."},
	         {"pad-error", broken,
	          "The controller cannot be read: no gamepad subsystem\nPress Esc to cancel."}}) {
		controller.Set(state);
		const std::string before = Bound(editor, core::InputDevice::Pad, "Square");
		Capture(BindingOf(root, "Square"), [&](KytyBindingCaptureDialog* capture) {
			Check(HasText(GTK_WIDGET(capture), text),
			      std::string(name) + ": the prompt: " + KytyUi::AllText(GTK_WIDGET(capture)));
			Check(KytyUi::Capture(GTK_WINDOW(capture), name), "the prompt renders");
			KytyUi::KeyPressOn(GTK_WIDGET(capture), GDK_KEY_Escape);
		});
		Check(Bound(editor, core::InputDevice::Pad, "Square") == before,
		      std::string(name) + ": Escape keeps the binding");
	}
	kyty::gtk::ControllerState connected;
	connected.device_name = "UI Test Pad";
	controller.Set(connected);
	Capture(BindingOf(root, "Square"), [&controller,
	                                    &connected](KytyBindingCaptureDialog* capture) {
		Check(HasText(GTK_WIDGET(capture), "UI Test Pad"), "the pad capture names the controller");
		Check(KytyUi::Capture(GTK_WINDOW(capture), "pad-capture"), "the pad capture renders");
		auto pressed        = connected;
		pressed.raw_pressed = {"North"};
		controller.Set(pressed);
	});
	controller.Set(connected);
	Check(Bound(editor, core::InputDevice::Pad, "Square") == "North",
	      "Square is bound to the pad's North button (" +
	          Bound(editor, core::InputDevice::Pad, "Square") + ")");
	Check(KytyUi::ReadText(ui.SettingsFile()) == before, "nothing is written before Save");
	const auto expected = core::SerializeMapping(kyty_input_mapping_window_get_model(editor));
	Check(KytyUi::Capture(GTK_WINDOW(editor), "gamepad"), "the editor renders");
	// A setting that is not valid refuses the save and names it (p6-u8-4's missing capture).
	auto&             loaded = ui.Context().settings.loaded.settings;
	const std::string name   = loaded.global.user_name;
	loaded.global.user_name  = std::string(40, 'x');
	Check(KytyUi::Activate(Widget(root, "save_button")), "a click on Save");
	if (GtkWindow* refused = KytyUi::FindAlert("The input mapping could not be saved");
	    refused != nullptr) {
		Check(HasText(GTK_WIDGET(refused), "The setting user_name is not valid."),
		      "the alert names the setting: " + KytyUi::AllText(GTK_WIDGET(refused)));
		Check(KytyUi::Capture(refused, "save-refused"), "the alert renders");
		KytyUi::CloseError(refused);
	}
	Check(KytyUi::IsToplevel(GTK_WINDOW(editor)) && KytyUi::ReadText(ui.SettingsFile()) == before,
	      "the refused save keeps the editor open and writes nothing");
	loaded.global.user_name = name;
	SaveEditor(editor);
	Check(KytyUi::StoredSettings(ui).global.host_input_mapping == expected,
	      "Save writes every captured binding");
}

// The per-game editor (11.14): Save gives the game its own mapping (the global one unchanged);
// reopened, the editor says so, and Use global mapping makes the game use the global one again.
KYTY_UI_CASE(content_input_mapping_per_game) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const auto alpha  = Entry(games, kAlphaTitle);
	const auto global = KytyUi::StoredSettings(ui).global.host_input_mapping;
	auto*      editor = OpenEditor(ui, &alpha);
	if (editor == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(editor);
	kyty_input_mapping_window_show_tab(editor, "keyboard");
	KytyUi::WaitForPaintedFrame(root);
	Capture(BindingOf(root, "Triangle"), [](KytyBindingCaptureDialog* capture) {
		kyty_binding_capture_dialog_handle_key(capture, GDK_KEY_u, GdkModifierType(0));
	});
	SaveEditor(editor);
	const auto  stored = KytyUi::StoredSettings(ui);
	const auto* game   = core::FindGameSettings(stored, alpha);
	Check(game != nullptr && game->configuration.input_mapping_custom &&
	          std::find(game->configuration.host_input_mapping.begin(),
	                    game->configuration.host_input_mapping.end(),
	                    "Triangle=U") != game->configuration.host_input_mapping.end(),
	      "Alpha Game has its own mapping with Triangle=U");
	Check(stored.global.host_input_mapping == global, "the global mapping is unchanged");
	editor = OpenEditor(ui, &alpha);
	if (editor == nullptr) {
		return;
	}
	root = GTK_WIDGET(editor);
	Check(LabelText(root, "game_note_label") == "This game has its own input mapping.",
	      "the reopened editor says the game has its own mapping");
	GtkWidget* use_global = Widget(root, "global_button");
	Check(gtk_widget_get_sensitive(use_global) != FALSE, "Use global mapping is enabled");
	Check(KytyUi::Capture(GTK_WINDOW(editor), "own-mapping"), "the editor renders");
	GtkWindow* closing = GTK_WINDOW(editor);
	Check(KytyUi::Activate(use_global), "a click on Use global mapping");
	KytyUi::WaitClosed(closing, "Use global mapping to close the editor");
	const auto  after = KytyUi::StoredSettings(ui);
	const auto* again = core::FindGameSettings(after, alpha);
	Check(again != nullptr && !again->configuration.input_mapping_custom,
	      "Alpha Game uses the global mapping again");
	const auto effective = core::ResolveEffectiveConfiguration(after, alpha, std::nullopt);
	Check(effective.configuration.host_input_mapping == after.global.host_input_mapping,
	      "the game runs with the global mapping");
}

// W4-14 (p6-u8-3): while the game has no custom settings, the per-game editor says that saving
// gives it its own settings; once it has them, it does not.
KYTY_UI_CASE(content_input_mapping_per_game_note) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const auto alpha = Entry(games, kAlphaTitle);
	const auto beta  = Entry(games, kBetaTitle);
	if (!KytyUi::StoreSettings(ui, [&beta](core::LauncherSettings& settings) {
		    Check(static_cast<bool>(core::SaveCustomSettings(
		              settings, beta, core::EnableCustomSettings(settings, beta))),
		          "store Beta Quest's custom settings");
	    })) {
		return;
	}
	for (const auto& [entry, note]:
	     std::vector<std::pair<core::GameEntry, bool>> {{alpha, true}, {beta, false}}) {
		auto* editor = OpenEditor(ui, &entry);
		if (editor == nullptr) {
			return;
		}
		GtkWidget* root  = GTK_WIDGET(editor);
		GtkWidget* label = Widget(root, "game_settings_note_label");
		const auto name  = entry.title_name;
		Check(label != nullptr && (gtk_widget_get_mapped(label) != FALSE) == note &&
		          KytyUi::WidgetText(label) == "Saving gives this game its own settings.",
		      name + (note ? ": the note is shown" : ": no note"));
		Check(LabelText(root, "game_note_label") ==
		          "This game uses the global input mapping. Saving gives it its own mapping.",
		      name + ": the mapping note");
		Check(KytyUi::Capture(GTK_WINDOW(editor), note ? "global-settings" : "own-settings"),
		      "the editor renders");
		KytyUi::CloseWithEscape(GTK_WINDOW(editor), "the input mapping editor");
	}
}

// W4-13 (p6-u8-2): a translated section title with "<" and "&" is shown as it is (the group title
// is markup: it is escaped), without a Pango warning.
KYTY_UI_CASE(content_input_mapping_markup) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	const std::string         translated = "Face <buttons> & co";
	const KytyUi::TestCatalog catalog(
	    ui.Root() / "locale",
	    {{"Face buttons", translated}, {"Input mapping", "Input mapping (test catalog)"}});
	Check(std::string_view(g_dgettext(GETTEXT_PACKAGE, "Face buttons")) == translated,
	      "the case's catalog translates the section title");
	auto* editor = OpenEditor(ui, nullptr);
	if (editor == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(editor);
	kyty_input_mapping_window_show_tab(editor, "keyboard");
	KytyUi::WaitForPaintedFrame(root);
	const auto titles = KytyUi::FindAll(root, [&translated](GtkWidget* widget) {
		return GTK_IS_LABEL(widget) && gtk_widget_get_mapped(widget) != FALSE &&
		       gtk_label_get_text(GTK_LABEL(widget)) == translated;
	});
	Check(!titles.empty(), "the group title shows \"" + translated + "\" as it is");
	Check(std::string_view(gtk_window_get_title(GTK_WINDOW(editor))) ==
	          "Input mapping (test catalog)",
	      "the window title comes from the catalog too");
	Check(KytyUi::Capture(GTK_WINDOW(editor), "markup"), "the editor renders");
	KytyUi::CloseWithEscape(GTK_WINDOW(editor), "the input mapping editor");
}

// The controller preview of the input mapping editor (LFI-24): the connected controller's name,
// the binding of a pressed pad source highlighted on the Gamepad tab and its control lit in the
// diagram, both cleared on release; paused while a game runs.
KYTY_UI_CASE(content_controller_preview) {
	KytyUi::UiContext ui;
	const auto        games = ContentLibrary(ui);
	if (games.empty()) {
		return;
	}
	auto& controller = ui.Services().controller;
	auto* editor     = OpenEditor(ui, nullptr);
	if (editor == nullptr) {
		return;
	}
	GtkWidget* root = GTK_WIDGET(editor);
	kyty_input_mapping_window_show_tab(editor, "gamepad");
	KytyUi::WaitForPaintedFrame(root);
	Check(controller.Listeners() == 1, "the editor listens to the controller");
	Check(LabelText(root, "controller_status") == "No controller connected",
	      "no controller at first");
	kyty::gtk::ControllerState state;
	state.device_name = "UI Test Pad";
	controller.Set(state);
	Check(LabelText(root, "controller_status") == "Connected: UI Test Pad",
	      "the connected controller is named");
	GtkWidget* cross   = BindingOf(root, "Cross");
	state.raw_pressed  = {"South"};
	state.pressed      = {"Cross"};
	const gint64 start = g_get_monotonic_time();
	controller.Set(state);
	const gint64 shown = g_get_monotonic_time();
	Check(cross != nullptr && gtk_widget_has_css_class(cross, "binding-pressed") != FALSE,
	      "Cross's binding is highlighted while South is pressed");
	Check(shown - start < 100000, "the highlight follows within 100 ms");
	auto* diagram = KytyUi::FindByType(root, KYTY_TYPE_DUALSENSE_WIDGET);
	Check(diagram != nullptr, "the editor has the DualSense diagram");
	KytyUi::WaitForPaintedFrame(root);
	Check(KytyUi::Capture(GTK_WINDOW(editor), "pressed"), "the pressed state renders");
	state.raw_pressed.clear();
	state.pressed.clear();
	controller.Set(state);
	Check(gtk_widget_has_css_class(cross, "binding-pressed") == FALSE,
	      "the highlight goes on release");
	state.paused = true;
	controller.Set(state);
	Check(LabelText(root, "controller_status") == "Preview is paused while a game is running.",
	      "the preview is paused while a game runs");
	KytyUi::WaitForPaintedFrame(root);
	Check(KytyUi::Capture(GTK_WINDOW(editor), "paused"), "the paused state renders");
	state.paused = false;
	state.error  = "no gamepad subsystem";
	controller.Set(state);
	Check(LabelText(root, "controller_status") ==
	          "Controller preview unavailable: no gamepad subsystem",
	      "an SDL failure is shown");
	KytyUi::WaitForPaintedFrame(root);
	Check(KytyUi::Capture(GTK_WINDOW(editor), "unavailable"), "the unavailable state renders");
	GtkWindow* closing = GTK_WINDOW(editor);
	KytyUi::CloseWithEscape(closing, "the input mapping editor");
	Check(controller.Listeners() == 0, "the editor stops listening when it closes");
}
