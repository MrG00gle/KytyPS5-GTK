#include "launcher/gtk/imageDecoding.h"

#include "launcher/gtk/gobjectPtr.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kyty::gtk {

namespace {

constexpr std::string_view kPngSignature("\x89PNG\r\n\x1a\n", 8);
// The largest decoded image (see DecodePng).
constexpr uint64_t kMaxDecodedImageBytes = uint64_t {256} << 20u;

uint32_t BigEndian32(std::string_view bytes, std::size_t offset) {
	uint32_t value = 0;
	for (std::size_t i = 0; i < 4; i++) {
		value = (value << 8u) | static_cast<uint8_t>(bytes[offset + i]);
	}
	return value;
}

// Whether the PNG's IHDR chunk (the first one: length 13, "IHDR", width, height, bit depth) gives
// an image that fits kMaxDecodedImageBytes, estimated as the decoder's RGBA layout: 4 bytes per
// pixel, 8 for a bit depth of 16. A PNG without a readable IHDR does not decode anyway.
bool FitsDecodeLimit(std::string_view png, const std::filesystem::path& source, const char* file) {
	// Signature (8), length (4), type (4), width (4), height (4), bit depth (1).
	constexpr std::size_t kIhdrEnd = 25;
	if (png.size() < kIhdrEnd || BigEndian32(png, 8) != 13 || png.substr(12, 4) != "IHDR") {
		return false;
	}
	const uint64_t width  = BigEndian32(png, 16);
	const uint64_t height = BigEndian32(png, 20);
	const uint64_t pixel  = static_cast<uint8_t>(png[24]) == 16 ? 8 : 4;
	if (width == 0 || height == 0) {
		return false;
	}
	if (width * height > kMaxDecodedImageBytes / pixel) {
		g_message("not decoding the %s of %s: %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT
		          " pixels need more than %" G_GUINT64_FORMAT " MiB",
		          file, source.c_str(), width, height, kMaxDecodedImageBytes >> 20u);
		return false;
	}
	return true;
}

} // namespace

GdkTexture* DecodePng(std::string_view png, const std::filesystem::path& source, const char* file) {
	if (png.substr(0, kPngSignature.size()) != kPngSignature ||
	    !FitsDecodeLimit(png, source, file)) {
		return nullptr;
	}
	GBytes*     data = g_bytes_new(png.data(), png.size());
	GErrorPtr   error;
	GdkTexture* texture = gdk_texture_new_from_bytes(data, error.Out());
	g_bytes_unref(data);
	return texture;
}

GdkTexture* FitTexture(GdkTexture* texture, int max_size) {
	if (texture == nullptr) {
		return nullptr;
	}
	const int width  = gdk_texture_get_width(texture);
	const int height = gdk_texture_get_height(texture);
	if (max_size <= 0 || (width <= max_size && height <= max_size)) {
		return GDK_TEXTURE(g_object_ref(texture));
	}
	const double scale    = static_cast<double>(max_size) / std::max(width, height);
	const int    target_w = std::clamp(static_cast<int>(std::lround(width * scale)), 1, max_size);
	const int    target_h = std::clamp(static_cast<int>(std::lround(height * scale)), 1, max_size);

	// Premultiplied, so that averaging does not darken the edges of transparent areas.
	GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED);
	gsize   source_stride = 0;
	GBytes* source_bytes  = gdk_texture_downloader_download_bytes(downloader, &source_stride);
	gdk_texture_downloader_free(downloader);
	const auto* source = static_cast<const guint8*>(g_bytes_get_data(source_bytes, nullptr));

	const std::size_t   target_stride = static_cast<std::size_t>(target_w) * 4;
	std::vector<guint8> target(target_stride * static_cast<std::size_t>(target_h));
	for (int y = 0; y < target_h; y++) {
		const int y0 = static_cast<int>(static_cast<int64_t>(y) * height / target_h);
		const int y1 =
		    std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / target_h));
		for (int x = 0; x < target_w; x++) {
			const int x0 = static_cast<int>(static_cast<int64_t>(x) * width / target_w);
			const int x1 =
			    std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / target_w));
			uint32_t sum[4] = {};
			for (int sy = y0; sy < y1; sy++) {
				const guint8* row = source + static_cast<std::size_t>(sy) * source_stride;
				for (int sx = x0; sx < x1; sx++) {
					for (int channel = 0; channel < 4; channel++) {
						sum[channel] += row[static_cast<std::size_t>(sx) * 4 + channel];
					}
				}
			}
			const auto count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
			guint8*    pixel = &target[static_cast<std::size_t>(y) * target_stride +
			                           static_cast<std::size_t>(x) * 4];
			for (int channel = 0; channel < 4; channel++) {
				pixel[channel] = static_cast<guint8>((sum[channel] + count / 2) / count);
			}
		}
	}
	g_bytes_unref(source_bytes);
	GBytes*     bytes  = g_bytes_new(target.data(), target.size());
	GdkTexture* scaled = gdk_memory_texture_new(
	    target_w, target_h, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, target_stride);
	g_bytes_unref(bytes);
	return scaled;
}

} // namespace kyty::gtk
