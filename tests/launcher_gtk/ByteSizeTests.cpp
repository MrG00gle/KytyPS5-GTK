// Example tests of FormatByteSize (src/launcher/gtk/byteSize.h; phase 7 plan D21, W2): the
// launcher's byte sizes use 1024-based IEC units, in GLib's own wording (g_format_size_full with
// G_FORMAT_SIZE_IEC_UNITS: the number with one decimal, a no-break space, and the unit; whole
// bytes below 1 KiB). The expected texts are what GLib prints (checked on the host's GLib 2.88.4);
// the process has no locale (no setlocale call), so GLib's untranslated texts and the C decimal
// point are used. No display, no gtk_init.

#include "launcher/gtk/byteSize.h"
#include "support/noDisplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace {

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ByteSizeTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// GLib puts a no-break space (U+00A0) between the number and the unit.
const std::string kSpace = "\xc2\xa0";

void CheckSize(uint64_t bytes, const std::string& expected) {
	const std::string text = kyty::gtk::FormatByteSize(bytes);
	Check(text == expected,
	      std::to_string(bytes) + " bytes are \"" + expected + "\", not \"" + text + "\"");
}

void TestIecUnits() {
	CheckSize(0, "0 bytes");
	CheckSize(1, "1 byte");
	CheckSize(1023, "1023 bytes");
	CheckSize(1024, "1.0" + kSpace + "KiB");
	CheckSize(131072, "128.0" + kSpace + "KiB");
	CheckSize(262144, "256.0" + kSpace + "KiB");
	// GLib rounds 1 MiB - 1 within the KiB range.
	CheckSize((uint64_t {1} << 20u) - 1, "1024.0" + kSpace + "KiB");
	CheckSize(uint64_t {1} << 20u, "1.0" + kSpace + "MiB");
	CheckSize(uint64_t {48} << 20u, "48.0" + kSpace + "MiB");
	CheckSize(uint64_t {5} << 30u, "5.0" + kSpace + "GiB");
	CheckSize(std::numeric_limits<uint64_t>::max(), "16.0" + kSpace + "EiB");
}

// The update download progress (updateProgressWindow.cpp) shows "<received> of <total>": 128 KiB
// of 256 KiB, where the SI units of phase 6 showed "131.1 kB of 262.1 kB" (p6-u11-3).
void TestNoSiUnits() {
	for (const uint64_t bytes:
	     {uint64_t {1000}, uint64_t {131072}, uint64_t {1000000}, uint64_t {5000000000}}) {
		const std::string text = kyty::gtk::FormatByteSize(bytes);
		Check(text.find("kB") == std::string::npos && text.find("MB") == std::string::npos &&
		          text.find("GB") == std::string::npos,
		      std::to_string(bytes) + " bytes are not shown in SI units (\"" + text + "\")");
	}
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ByteSizeTests");
	TestIecUnits();
	TestNoSiUnits();
	std::printf("ByteSizeTests: all tests passed\n");
	return 0;
}
