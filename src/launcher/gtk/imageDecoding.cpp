#include "launcher/gtk/imageDecoding.h"

#include "launcher/gtk/gobjectPtr.h"

#include <cstddef>
#include <cstdint>

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

} // namespace kyty::gtk
