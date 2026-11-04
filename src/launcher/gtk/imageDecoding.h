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

// texture (transfer none) made to fit a max_size x max_size square (transfer full): a new reference
// to texture when it fits already (or max_size is not positive), else a copy scaled down once by
// averaging the source pixels each target pixel covers, with the aspect ratio kept, so the renderer
// does not scale a large image at every frame (Requirement 7.13). nullptr for nullptr.
GdkTexture* FitTexture(GdkTexture* texture, int max_size);

} // namespace kyty::gtk
