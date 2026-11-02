#include "ui/support/uiFixtures.h"

#include "launcher/core/archiveJob.h"
#include "launcher/core/cancelToken.h"
#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/trophyModel.h"
#include "launcher/gtk/gobjectPtr.h"
#include "support/contentGenerators.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiHarness.h"

#include <adwaita.h>
#include <cstdio>
#include <fstream>
#include <gtk/gtk.h>
#include <system_error>
#include <vector>

namespace KytyUi {

namespace fs  = std::filesystem;
namespace gen = KytyTests::ContentGen;
using kyty::gtk::GObjectPtr;

namespace {

bool WriteFile(const fs::path& path, std::string_view bytes) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return Check(static_cast<bool>(out), "write " + path.string());
}

bool WriteGame(const fs::path& directory, std::string_view title, std::string_view title_id,
               std::string_view version, int hue) {
	return WriteFile(directory / "sce_sys" / "param.json", ParamJson(title, title_id, version)) &&
	       WriteFile(directory / "eboot.bin", "\x7f"
	                                          "ELF fake eboot of the UI tests\n") &&
	       WritePng(directory / "sce_sys" / "icon0.png", 512, 512, hue) &&
	       WritePng(directory / "sce_sys" / "pic0.png", 960, 540, hue + 1);
}

// "Ünïcöde" in UTF-8.
std::string Unicode() {
	return std::string("\xc3\x9c") + "n" + "\xc3\xaf" + "c" + "\xc3\xb6" + "de";
}

gen::TrophySpec Trophy(int id, char grade, const char* name, const char* detail, bool hidden) {
	gen::TrophySpec spec;
	spec.id     = id;
	spec.grade  = grade;
	spec.hidden = hidden;
	spec.name   = name;
	spec.detail = detail;
	return spec;
}

// A control of type whose ControlName starts with prefix and ends with suffix.
std::function<bool(GtkWidget*)> NamedControl(GType type, std::string prefix,
                                             std::string suffix = "") {
	return [type, prefix = std::move(prefix), suffix = std::move(suffix)](GtkWidget* widget) {
		if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type) == FALSE) {
			return false;
		}
		const std::string name = ControlName(widget);
		return name.starts_with(prefix) && name.ends_with(suffix);
	};
}

} // namespace

std::string ParamJson(std::string_view title, std::string_view title_id, std::string_view version) {
	std::string json =
	    "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"";
	for (const char c: title) {
		if (c == '"' || c == '\\') {
			json += '\\';
		}
		json += c;
	}
	json += "\"}},\"titleId\":\"" + std::string(title_id) + "\",\"appVersion\":\"" +
	        std::string(version) + "\",\"requiredSystemSoftwareVersion\":\"0x0150000000000000\"}";
	return json;
}

bool WritePng(const fs::path& path, int width, int height, int hue) {
	static const int    kBase[6][3] = {{220, 70, 60},  {60, 160, 90},  {60, 110, 210},
	                                   {200, 160, 40}, {150, 80, 190}, {40, 170, 180}};
	const auto*         base        = kBase[hue % 6];
	std::vector<guint8> pixels(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const double t     = static_cast<double>(x + y) / (width + height);
			guint8*      pixel = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
			for (int channel = 0; channel < 3; channel++) {
				pixel[channel] = static_cast<guint8>(base[channel] * (0.45 + 0.55 * t));
			}
			pixel[3]         = 255;
			const bool frame = x < width / 16 || y < height / 16 || x >= width - width / 16 ||
			                   y >= height - height / 16;
			if (frame) {
				pixel[0] = pixel[1] = pixel[2] = 245;
			}
		}
	}
	GBytes* bytes   = g_bytes_new(pixels.data(), pixels.size());
	auto    texture = GObjectPtr<GdkTexture>::Adopt(gdk_memory_texture_new(
	    width, height, GDK_MEMORY_R8G8B8A8, bytes, static_cast<gsize>(width) * 4));
	g_bytes_unref(bytes);
	GBytes*     png  = gdk_texture_save_to_png_bytes(texture.get());
	gsize       size = 0;
	const auto* data = static_cast<const char*>(g_bytes_get_data(png, &size));
	const bool  ok   = WriteFile(path, std::string_view(data, size));
	g_bytes_unref(png);
	return ok;
}

fs::path WriteFixtures(const fs::path& root) {
	const fs::path games = root / "games";
	bool ok = WriteGame(games / "Alpha Game", "Alpha Game", "PPSA01001", "01.000", 0) &&
	          WriteGame(games / "Beta" / "Beta Quest", "Beta Quest", "PPSA01002", "01.020", 2) &&
	          WriteGame(games / ("Odd \"Quotes\" back\\slash " + Unicode()),
	                    Unicode() + " \"Odd\" Game", "PPSA01003", "02.000", 4);
	// An unusable param.json: the title falls back to the directory name (Requirement 7.9).
	ok = ok && WriteFile(games / "Broken Metadata" / "sce_sys" / "param.json", "{\"titleName\":") &&
	     WriteFile(games / "Broken Metadata" / "eboot.bin", "fake eboot\n");
	// A .zar game: the staging directory is packed next to itself, then removed.
	const fs::path staging = games / "Gamma Archive";
	ok                     = ok && WriteGame(staging, "Gamma Archive", "PPSA01004", "01.000", 3);
	if (!ok) {
		return {};
	}
	auto job = kyty::core::ArchiveJob::Prepare(staging);
	if (!job) {
		Fail("cannot prepare the archive of " + staging.string() + ": " + job.error());
		return {};
	}
	kyty::core::CancelToken cancel;
	job->Run([](int) {}, cancel);
	if (job->Outcome() != kyty::core::ArchiveOutcome::Completed) {
		Fail("packing " + staging.string() + " failed: " + job->Error());
		return {};
	}
	std::error_code ec;
	fs::remove_all(staging, ec);
	return games;
}

int WriteSyntheticGames(const fs::path& folder, int count, bool with_icons) {
	for (int n = 1; n <= count; n++) {
		char title_id[16];
		std::snprintf(title_id, sizeof(title_id), "PPSA2%04d", n);
		const std::string title     = "Synthetic Game " + std::to_string(n);
		const fs::path    directory = folder / title;
		const bool        ok =
		    WriteFile(directory / "sce_sys" / "param.json", ParamJson(title, title_id, "01.000")) &&
		    WriteFile(directory / "eboot.bin", "fake eboot\n") &&
		    (!with_icons || WritePng(directory / "sce_sys" / "icon0.png", 64, 64, n));
		if (!ok) {
			return n - 1;
		}
	}
	return count;
}

bool WriteGameContent(const fs::path& games, const fs::path& data_dir, const fs::path& addons,
                      int user_id) {
	gen::TrophyPackageSpec package;
	package.title    = "Alpha Game Trophies";
	package.trophies = {Trophy(1, 'G', "Gold Rush", "Collect 1,000 coins.", false),
	                    Trophy(2, 'S', "Silver Lining", "Survive the storm.", false),
	                    Trophy(3, 'B', "Secret Ending", "Reach the secret ending.", true)};
	kyty::core::TrophyUnlocks unlocks;
	unlocks.unlocked = {1};
	auto cheats =
	    gen::RemoteCheatSpec("PPSA01001", "01.000", "eboot.bin", {"Infinite Health", "Max Coins"});
	cheats.mods[1].push_back({"enabled", "false"});
	const fs::path add_on = addons / "alpha-soundtrack" / "sce_sys";
	return WriteFile(games / "Alpha Game" / "sce_sys" / "trophy2" / "trophy00.ucp",
	                 gen::TrophyPackageBytes(package)) &&
	       Check(gen::WriteUnlocks(data_dir, "PPSA01001", user_id, 0, unlocks),
	             "write the unlock file of Alpha Game's trophies") &&
	       WriteFile(add_on / "param.json",
	                 gen::AddOnParamJson(gen::AddOnContentId("PPSA01001", "ALPHASOUNDTRACK1"),
	                                     "Alpha Soundtrack")) &&
	       WritePng(add_on / "icon0.png", 128, 128, 5) &&
	       WriteFile(kyty::core::CheatFilePath(data_dir, "PPSA01001"),
	                 gen::CheatDocumentText(cheats));
}

std::vector<std::pair<std::string, std::string>> CheatIndexes() {
	const auto line = [](std::string_view title_id, std::string_view version, std::string_view rest,
	                     std::string_view format, std::string_view title) {
		return gen::CheatIndexLine(gen::CheatFileName(title_id, version, rest, format), title) +
		       "\n";
	};
	return {{kyty::core::CheatIndexUrl("json"),
	         line("PPSA01001", "01.000", "alpha", "json", "Alpha Game") +
	             line("PPSA01001", "01.020", "alpha", "json", "Alpha Game") +
	             line("PPSA01002", "01.020", "beta", "json", "Beta Quest")},
	        {kyty::core::CheatIndexUrl("mc4"),
	         line("PPSA01001", "01.000", "alpha", "mc4", "Alpha Game")},
	        {kyty::core::CheatIndexUrl("shn"),
	         line("PPSA01001", "01.000", "alpha", "shn", "Alpha Game")}};
}

std::vector<ContentControl> GameContentControls(std::string_view id) {
	if (id == "trophy-window") {
		// One readable package: one page.
		const std::string page = "package-0";
		return {{page, "the row of the earned trophy",
		         NamedControl(GTK_TYPE_LIST_BOX_ROW, "Gold Rush, ", ", earned")},
		        {page, "the row of a trophy not earned",
		         NamedControl(GTK_TYPE_LIST_BOX_ROW, "Silver Lining, ", ", not earned")},
		        {page, "the row of the hidden trophy",
		         NamedControl(GTK_TYPE_LIST_BOX_ROW, "Hidden trophy, ")},
		        {page, "the Reveal button of the hidden trophy", [](GtkWidget* widget) {
			         return GTK_IS_BUTTON(widget) &&
			                gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW) != nullptr;
		         }}};
	}
	if (id == "trophy-overview-window") {
		return {{"", "the row of Alpha Game", NamedControl(GTK_TYPE_LIST_BOX_ROW, "Alpha Game, ")}};
	}
	if (id == "additional-content-window") {
		return {
		    {"", "the row of the add-on", NamedControl(ADW_TYPE_ACTION_ROW, "Alpha Soundtrack")}};
	}
	if (id == "cheats-window") {
		return {
		    {"local", "the row of a cheat", NamedControl(ADW_TYPE_ACTION_ROW, "Infinite Health")},
		    {"local", "the check box of a cheat",
		     NamedControl(GTK_TYPE_CHECK_BUTTON, "Infinite Health")},
		    {"remote", "the row of a remote cheat file",
		     NamedControl(GTK_TYPE_LIST_BOX_ROW, "", ", PPSA01001_01.000_alpha.json")}};
	}
	return {};
}

} // namespace KytyUi
