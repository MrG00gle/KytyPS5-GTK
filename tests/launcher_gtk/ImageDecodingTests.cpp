// Example tests of FitTexture (src/launcher/gtk/imageDecoding.h; phase 7 plan D24 step 4, W1): the
// library's details worker scales each game icon down once to the largest size the views show
// (kLargestGameIconSize), keeping the aspect ratio, by averaging the source pixels in premultiplied
// alpha; a texture that fits is returned as it is. GdkMemoryTexture and GdkTextureDownloader need
// no display: no gtk_init.

#include "launcher/gtk/gameViewSupport.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/imageDecoding.h"
#include "support/noDisplay.h"

#include <cstdio>
#include <cstdlib>
#include <gdk/gdk.h>
#include <string>
#include <vector>

namespace {

using kyty::gtk::FitTexture;
using kyty::gtk::GObjectPtr;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ImageDecodingTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// A width x height straight-alpha RGBA texture whose pixel (x, y) is pixel(x, y).
template <typename Pixel>
GObjectPtr<GdkTexture> MakeTexture(int width, int height, Pixel pixel) {
	std::vector<guint8> bytes(static_cast<std::size_t>(width) * height * 4);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const std::vector<guint8> value = pixel(x, y);
			for (int channel = 0; channel < 4; channel++) {
				bytes[(static_cast<std::size_t>(y) * width + x) * 4 + channel] = value[channel];
			}
		}
	}
	GBytes* data    = g_bytes_new(bytes.data(), bytes.size());
	auto    texture = GObjectPtr<GdkTexture>::Adopt(gdk_memory_texture_new(
	    width, height, GDK_MEMORY_R8G8B8A8, data, static_cast<gsize>(width) * 4));
	g_bytes_unref(data);
	return texture;
}

// The texture's pixels as straight-alpha RGBA bytes.
std::vector<guint8> Pixels(GdkTexture* texture) {
	const int             width  = gdk_texture_get_width(texture);
	const int             height = gdk_texture_get_height(texture);
	std::vector<guint8>   bytes(static_cast<std::size_t>(width) * height * 4);
	GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	gdk_texture_downloader_download_into(downloader, bytes.data(), static_cast<gsize>(width) * 4);
	gdk_texture_downloader_free(downloader);
	return bytes;
}

std::string Size(GdkTexture* texture) {
	return std::to_string(gdk_texture_get_width(texture)) + "x" +
	       std::to_string(gdk_texture_get_height(texture));
}

void TestFitsAlready() {
	Check(FitTexture(nullptr, 180) == nullptr, "nullptr gives nullptr");
	const auto small =
	    MakeTexture(48, 32, [](int, int) { return std::vector<guint8> {10, 20, 30, 255}; });
	for (const int max_size: {180, 48, 0, -1}) {
		auto fitted = GObjectPtr<GdkTexture>::Adopt(FitTexture(small.get(), max_size));
		Check(fitted.get() == small.get(),
		      "a texture that fits " + std::to_string(max_size) + " is returned as it is");
	}
}

void TestScalesDown() {
	const auto icon = MakeTexture(512, 512, [](int x, int y) {
		return std::vector<guint8> {static_cast<guint8>(x / 2), static_cast<guint8>(y / 2), 128,
		                            255};
	});
	auto       fitted =
	    GObjectPtr<GdkTexture>::Adopt(FitTexture(icon.get(), kyty::gtk::kLargestGameIconSize));
	Check(fitted && fitted.get() != icon.get() && gdk_texture_get_width(fitted.get()) == 180 &&
	          gdk_texture_get_height(fitted.get()) == 180,
	      "a 512x512 icon becomes 180x180 (" + (fitted ? Size(fitted.get()) : "none") + ")");
	const auto art =
	    MakeTexture(960, 540, [](int, int) { return std::vector<guint8> {0, 0, 0, 255}; });
	fitted = GObjectPtr<GdkTexture>::Adopt(FitTexture(art.get(), 180));
	Check(gdk_texture_get_width(fitted.get()) == 180 && gdk_texture_get_height(fitted.get()) == 101,
	      "a 960x540 image keeps its aspect ratio (" + Size(fitted.get()) + ")");
	const auto tall =
	    MakeTexture(10, 400, [](int, int) { return std::vector<guint8> {0, 0, 0, 255}; });
	fitted = GObjectPtr<GdkTexture>::Adopt(FitTexture(tall.get(), 100));
	Check(gdk_texture_get_width(fitted.get()) == 3 && gdk_texture_get_height(fitted.get()) == 100,
	      "a 10x400 image becomes 3x100 (" + Size(fitted.get()) + ")");
}

// Each target pixel is the average of the source pixels it covers.
void TestAverages() {
	// Four 2x2 quadrants of different colors, scaled to 2x2.
	const auto quadrants = MakeTexture(4, 4, [](int x, int y) {
		const int    quadrant     = (y / 2) * 2 + x / 2;
		const guint8 colors[4][4] = {
		    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
		return std::vector<guint8>(colors[quadrant], colors[quadrant] + 4);
	});
	auto       fitted    = GObjectPtr<GdkTexture>::Adopt(FitTexture(quadrants.get(), 2));
	const auto pixels    = Pixels(fitted.get());
	Check(pixels == std::vector<guint8> {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255,
	                                     255, 255},
	      "every quadrant keeps its color");

	// Opaque red next to transparent black, scaled to one pixel: half-transparent red (in
	// premultiplied alpha the transparent pixel does not darken the color).
	const auto edge = MakeTexture(2, 1, [](int x, int) {
		return x == 0 ? std::vector<guint8> {255, 0, 0, 255} : std::vector<guint8> {0, 0, 0, 0};
	});
	fitted          = GObjectPtr<GdkTexture>::Adopt(FitTexture(edge.get(), 1));
	const auto one  = Pixels(fitted.get());
	Check(gdk_texture_get_width(fitted.get()) == 1 && gdk_texture_get_height(fitted.get()) == 1,
	      "2x1 to fit 1 is 1x1");
	Check(one[0] >= 250 && one[1] == 0 && one[2] == 0 && one[3] >= 127 && one[3] <= 128,
	      "red over transparent averages to half-transparent red (" + std::to_string(one[0]) +
	          ", " + std::to_string(one[1]) + ", " + std::to_string(one[2]) + ", " +
	          std::to_string(one[3]) + ")");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ImageDecodingTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));
	TestFitsAlready();
	TestScalesDown();
	TestAverages();
	std::printf("ImageDecodingTests: all tests passed\n");
	return 0;
}
