// The documentation screenshot (task 26.2; Requirements 14.8, 14.14; phase 8 plan D21), on Wayland
// and on X11. docs_main_window_screenshot runs the real KytyApplication of a Flatpak build (no
// update control) over 10 synthetic games with 512x512 icons (uiFixtures.h WriteSyntheticGames and
// WritePng) in a 1200x720 main window in the grid view, on a desktop that prefers the light style.
// The footer would name the case's temporary tree and the fake Emulator's banner, so once the
// version probe reported, it gets the texts of a Flatpak installation instead (ShowFlatpakFooter).
// The case renders the window once its rendering has settled (uiDriver.h RenderSettled) and checks
// it: the light style, the stored size 1200x720, a tile with its icon for each game, no footer
// update link, and no shown text that names the temporary tree.
// With --screenshot-out <file> (uiHarness.h) it also writes the rendering there as a PNG: the
// update-docs-screenshot target (tests/CMakeLists.txt) writes
// docs/screenshots/kyty-launcher-main.png that way, the screenshot that
// packaging/linux/io.github.MrG00gle.KytyPS5_GTK.metainfo.xml names.

#include "launcher/core/settingsStore.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/gameCell.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/mainWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"

#include <adwaita.h>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using KytyUi::Check;

constexpr int kGames  = 10;
constexpr int kWidth  = 1200;
constexpr int kHeight = 720;

// The desktop prefers the light style (2) before the launcher starts, as in theme_prefer_light.
const KytyUi::InitialColorScheme kPreferLight("docs_main_window_screenshot", 2);

// What the footer of a Flatpak installation shows for a user named "user": the Settings_Store
// below the sandbox's XDG_CONFIG_HOME, and the Emulator next to the launcher in /app/bin.
constexpr const char* kFlatpakSettingsFile =
    "Settings file: /home/user/.var/app/io.github.MrG00gle.KytyPS5_GTK/config/Kyty/Kyty.ini";
constexpr const char* kFlatpakEmulator = "Emulator: /app/bin/kyty_emulator";

// "Synthetic Game <n>" (uiFixtures.h WriteSyntheticGames).
std::string Title(int n) {
	return "Synthetic Game " + std::to_string(n);
}

// Every game has a mapped tile that draws its icon.
bool TilesWithIcons(GtkGridView* grid) {
	for (int n = 1; n <= kGames; n++) {
		GtkWidget* tile = KytyUi::GridTile(grid, Title(n));
		if (tile == nullptr || kyty_game_cell_get_texture(KYTY_GAME_CELL(tile)) == nullptr) {
			return false;
		}
	}
	return true;
}

// The footer names the case's temporary tree (the Settings_Store and the Emulator) and the fake
// Emulator's version banner. For the screenshot it shows the Flatpak paths instead, and no version
// line, as when the version probe reports none. Call it after the probe reported, so that the
// window does not update the footer again.
bool ShowFlatpakFooter(KytyMainWindow* window) {
	GtkWidget* root     = GTK_WIDGET(window);
	GtkWidget* settings = KytyUi::FindByBuildableId(root, "settings_label");
	GtkWidget* emulator = KytyUi::FindByBuildableId(root, "emulator_label");
	GtkWidget* version  = KytyUi::FindByBuildableId(root, "version_label");
	if (!Check(GTK_IS_LABEL(settings) && GTK_IS_LABEL(emulator) && GTK_IS_LABEL(version),
	           "the footer has its settings, emulator, and version labels")) {
		return false;
	}
	gtk_label_set_text(GTK_LABEL(settings), kFlatpakSettingsFile);
	gtk_label_set_text(GTK_LABEL(emulator), kFlatpakEmulator);
	gtk_widget_set_visible(version, FALSE);
	return true;
}

// A Flatpak build's footer has neither "Check for updates" nor "Releases page" (mainWindow.cpp).
void CheckNoUpdateLinks(KytyMainWindow* window) {
	for (const char* id: {"check_updates_link", "releases_link"}) {
		GtkWidget* link = KytyUi::FindByBuildableId(GTK_WIDGET(window), id);
		Check(link != nullptr && gtk_widget_get_mapped(link) == FALSE,
		      std::string("the footer does not show ") + id);
	}
}

// The mapped widgets below root whose visible text contains text (uiDriver.h WidgetText).
std::vector<GtkWidget*> ShownTextsWith(GtkWidget* root, const std::string& text) {
	return KytyUi::FindAll(root, [&text](GtkWidget* widget) {
		return gtk_widget_get_mapped(widget) != FALSE &&
		       KytyUi::WidgetText(widget).find(text) != std::string::npos;
	});
}

} // namespace

KYTY_UI_CASE(docs_main_window_screenshot) {
	KytyUi::UiContext ui;
	const fs::path    games = ui.Root() / "games";
	if (!Check(KytyUi::WriteSyntheticGames(games, kGames) == kGames,
	           "10 synthetic games are written")) {
		return;
	}
	// The size of a PS5 game's icon0.png, so that the tiles draw them downscaled.
	for (int n = 1; n <= kGames; n++) {
		if (!Check(KytyUi::WritePng(games / Title(n) / "sce_sys" / "icon0.png", 512, 512, n),
		           Title(n) + " gets a 512x512 icon")) {
			return;
		}
	}
	if (!ui.StoreGameFolders({games}) ||
	    !KytyUi::StoreSettings(ui, [](kyty::core::LauncherSettings& settings) {
		    settings.grid_view        = true;
		    settings.window.width     = kWidth;
		    settings.window.height    = kHeight;
		    settings.window.maximized = false;
	    })) {
		return;
	}
	ui.ServeCompatibility();
	KytyUi::RunWithLibrary(
	    ui,
	    [&ui](KytyApplication* application, KytyMainWindow* window) {
		    if (!KytyUi::ShowGridView(application, window, true)) {
			    return;
		    }
		    GtkGridView* grid = KytyUi::GridViewOf(window);
		    if (!KytyUi::WaitFor([grid]() { return TilesWithIcons(grid); }, 5000,
		                         "a tile with its icon for each of the 10 games")) {
			    return;
		    }
		    auto& context = KytyUi::ContextOf(application);
		    if (!KytyUi::WaitFor([&context]() { return !context.emulator_version.empty(); }, 5000,
		                         "the Emulator's version probe") ||
		        !ShowFlatpakFooter(window) || !KytyUi::WaitForPaintedFrame(GTK_WIDGET(window))) {
			    return;
		    }
		    Check(adw_style_manager_get_dark(adw_style_manager_get_default()) == FALSE,
		          "the light style");
		    int width  = 0;
		    int height = 0;
		    gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
		    Check(width == kWidth && height == kHeight,
		          "the main window's size is the stored 1200x720 (" + std::to_string(width) + "x" +
		              std::to_string(height) + ")");
		    CheckNoUpdateLinks(window);
		    for (GtkWidget* widget: ShownTextsWith(GTK_WIDGET(window), ui.Root().string())) {
			    KytyUi::Fail("the shown text \"" + KytyUi::WidgetText(widget) + "\" of " +
			                 KytyUi::WidgetPath(widget) + " names the case's temporary tree");
		    }

		    const auto texture = KytyUi::RenderSettled(GTK_WINDOW(window), "the main window");
		    if (!texture) {
			    return;
		    }
		    width  = gdk_texture_get_width(texture.get());
		    height = gdk_texture_get_height(texture.get());
		    Check(width == kWidth && height == kHeight, "the rendering is 1200x720 (" +
		                                                    std::to_string(width) + "x" +
		                                                    std::to_string(height) + ")");
		    // A rendering that failed a check never replaces the screenshot.
		    const fs::path& output = KytyUi::ScreenshotOutput();
		    if (output.empty() || KytyUi::Failures() > 0) {
			    return;
		    }
		    if (Check(gdk_texture_save_to_png(texture.get(), output.c_str()) != FALSE,
		              "write " + output.string())) {
			    std::printf("%s: wrote %s (%dx%d)\n", KytyUi::CaseName().c_str(), output.c_str(),
			                width, height);
		    }
	    },
	    kyty::updater::UpdateUiMode::Flatpak);
}
