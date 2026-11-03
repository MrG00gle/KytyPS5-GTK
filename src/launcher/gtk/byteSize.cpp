#include "launcher/gtk/byteSize.h"

#include "launcher/gtk/gobjectPtr.h"

#include <glib.h>

namespace kyty::gtk {

std::string FormatByteSize(uint64_t bytes) {
	const GCharPtr text(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
	return std::string(text.View());
}

} // namespace kyty::gtk
