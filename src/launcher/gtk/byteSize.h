#pragma once

// Byte sizes as the launcher shows them (phase 7 plan D21, W2): 1024-based IEC units everywhere
// (the list view's Size column, as the Qt launcher's QLocale::DataSizeTraditionalFormat, and the
// update download progress).

#include <cstdint>
#include <string>

namespace kyty::gtk {

// g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS): GLib's own wording, for example "0 bytes",
// "1023 bytes", "128.0 KiB", "5.0 GiB" (a no-break space before the unit; GLib's translations when
// the process has a locale).
std::string FormatByteSize(uint64_t bytes);

} // namespace kyty::gtk
