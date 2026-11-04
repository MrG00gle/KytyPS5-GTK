#include "ui/support/uiCatalog.h"

#include "launcher/gtk/i18n.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"

#include <clocale>
#include <cstdint>
#include <system_error>
#include <utility>

namespace KytyUi {

namespace {

// glibc 2.35 and later install the C.UTF-8 locale as this directory.
constexpr const char* kSystemUtf8Locale = "/usr/lib/locale/C.utf8";

void AppendLe32(std::string& out, uint32_t value) {
	for (int i = 0; i < 4; i++) {
		out += static_cast<char>((value >> (8 * i)) & 0xffu);
	}
}

} // namespace

std::string MoFile(std::map<std::string, std::string> translations) {
	translations[""] = "Content-Type: text/plain; charset=UTF-8\n"
	                   "Plural-Forms: nplurals=2; plural=(n != 1);\n";
	const auto count = static_cast<uint32_t>(translations.size());
	// The header (28 bytes), the table of msgids, the table of texts, then the strings.
	const uint32_t originals = 28;
	const uint32_t texts     = originals + count * 8;
	uint32_t       offset    = texts + count * 8;
	std::string    original_table;
	std::string    text_table;
	std::string    strings;
	for (const auto& [msgid, text]: translations) {
		AppendLe32(original_table, static_cast<uint32_t>(msgid.size()));
		AppendLe32(original_table, offset);
		strings += msgid + '\0';
		offset += static_cast<uint32_t>(msgid.size() + 1);
	}
	for (const auto& [msgid, text]: translations) {
		AppendLe32(text_table, static_cast<uint32_t>(text.size()));
		AppendLe32(text_table, offset);
		strings += text + '\0';
		offset += static_cast<uint32_t>(text.size() + 1);
	}
	std::string file;
	AppendLe32(file, 0x950412deu); // the magic number, little-endian
	AppendLe32(file, 0);           // revision
	AppendLe32(file, count);
	AppendLe32(file, originals);
	AppendLe32(file, texts);
	AppendLe32(file, 0); // no hash table
	AppendLe32(file, offset);
	return file + original_table + text_table + strings;
}

TestCatalog::TestCatalog(const std::filesystem::path&       root,
                         std::map<std::string, std::string> translations)
    : m_root(root) {
	namespace fs = std::filesystem;
	WriteText(root / "kytytest" / "LC_MESSAGES" / (GETTEXT_PACKAGE ".mo"),
	          MoFile(std::move(translations)));
	// glibc's own C.UTF-8 locale under another name, so that LC_MESSAGES is not "C".
	std::error_code ec;
	fs::create_directories(root / "locales", ec);
	fs::copy(kSystemUtf8Locale, root / "locales" / "kytytest.utf8", fs::copy_options::recursive,
	         ec);
	Check(!ec, "copy " + std::string(kSystemUtf8Locale) + ": " + ec.message());
	g_setenv("LOCPATH", (root / "locales").c_str(), TRUE);
	Check(std::setlocale(LC_MESSAGES, "kytytest.utf8") != nullptr,
	      "LC_MESSAGES kytytest.utf8 (a copy of " + std::string(kSystemUtf8Locale) + ")");
	bindtextdomain(GETTEXT_PACKAGE, root.c_str());
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
}

TestCatalog::~TestCatalog() {
	std::setlocale(LC_MESSAGES, "");
	g_unsetenv("LOCPATH");
	bindtextdomain(GETTEXT_PACKAGE, (m_root / "none").c_str());
}

} // namespace KytyUi
