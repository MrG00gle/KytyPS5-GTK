#pragma once

// PNG decoding for the game images of the GTK launcher (Requirement 7.16; HC-23): the icons and the
// background art of the library (libraryController.cpp), trophy icons (trophyWindow.cpp), and
// add-on icons (additionalContentWindow.cpp). Only PNG data is decoded (Requirement 7.16 names
// PNG; other formats show the caller's placeholder). Thread-safe: the decoders run in GTask
// threads.

#include <filesystem>
#include <gdk/gdk.h>
#include <string_view>

namespace kyty::gtk {

// A texture of the PNG bytes (transfer full), or nullptr for empty or other data, an image whose
// IHDR size would need more than 256 MiB decoded (4 bytes per pixel, 8 for a bit depth of 16: Qt
// 6's default QImageReader allocation limit, which the Qt launcher had; 8192 x 8192 RGBA fits), or
// a decode failure. source and file name the image in the message logged for a refused size.
GdkTexture* DecodePng(std::string_view png, const std::filesystem::path& source, const char* file);

} // namespace kyty::gtk
