#pragma once

// A message catalog of a UI case's own (phase 7 plan section 6 U7: a translated section title of
// the input mapping editor, a translated update message). gettext translates nothing in the
// sessions' C.UTF-8 locale (glibc treats it as "C" and ignores LANGUAGE), and the test hosts need
// no other locale: TestCatalog copies glibc's C.UTF-8 locale directory (/usr/lib/locale/C.utf8,
// glibc 2.35 and later) as "kytytest.utf8" below the case's tree, points LOCPATH there, sets
// LC_MESSAGES to it, and binds the kyty-launcher domain to a GNU .mo file for the language
// "kytytest" while it exists. Main thread only.

#include <filesystem>
#include <map>
#include <string>

namespace KytyUi {

// A GNU .mo file of translations (msgid -> text, sorted by msgid as the format needs), with a
// header entry that declares UTF-8 and two plural forms (n != 1). A plural entry's msgid is
// "<singular>\0<plural>" and its text "<form 0>\0<form 1>".
std::string MoFile(std::map<std::string, std::string> translations);

class TestCatalog {
public:
	// Writes <root>/kytytest/LC_MESSAGES/kyty-launcher.mo and <root>/locales/kytytest.utf8, sets
	// LOCPATH and LC_MESSAGES, and binds the domain to root (UTF-8). Fails the case when the locale
	// cannot be set.
	TestCatalog(const std::filesystem::path& root, std::map<std::string, std::string> translations);
	// LC_MESSAGES from the environment again, LOCPATH unset, and the domain bound to a directory
	// without catalogs.
	~TestCatalog();

	TestCatalog(const TestCatalog&)            = delete;
	TestCatalog& operator=(const TestCatalog&) = delete;

private:
	std::filesystem::path m_root;
};

} // namespace KytyUi
