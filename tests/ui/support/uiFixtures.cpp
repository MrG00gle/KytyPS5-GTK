#include "ui/support/uiFixtures.h"

#include "launcher/core/archiveJob.h"
#include "launcher/core/cancelToken.h"
#include "launcher/gtk/gobjectPtr.h"
#include "ui/support/uiHarness.h"

#include <cstdio>
#include <fstream>
#include <gtk/gtk.h>
#include <system_error>
#include <vector>

namespace KytyUi {

namespace fs = std::filesystem;
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

} // namespace KytyUi
