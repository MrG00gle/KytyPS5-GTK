#pragma once

// The UI test harness (task 22.1; Requirements 17.10, 17.11; phase 7 plan D6, D11, D33):
// launcher_ui_tests holds every case of tests/ui/*Tests.cpp, declared with
//   KYTY_UI_CASE(<name>) { ... }
// at the start of a line (kyty_add_ui_tests in cmake/KytyTesting.cmake finds the names there and
// registers one CTest test per case and backend). main() (uiHarness.cpp):
//   launcher_ui_tests --list                prints the case names, sorted, one per line; no display
//   launcher_ui_tests --case <name> [--capture-dir <dir>]
//                                           runs one case inside tests/ui/with-display.sh
// A case runs on the main thread after gtk_init, adw_init, and the launcher's GResource bundle. A
// GLib log writer, installed before anything else, prints every warning, critical, and error of
// any domain to stderr at once and fails the case, unless the case declared it with ExpectLog(); a
// declared message must then arrive exactly once. Lower levels go to GLib's default writer. The
// exit status is 0 when the case passed and 1 otherwise; the last line of the output says which.
// --capture-dir (or KYTY_UI_CAPTURE_DIR) makes Capture() (uiDriver.h) write PNG files there.

#include <filesystem>
#include <glib.h>
#include <string>
#include <string_view>
#include <vector>

namespace KytyUi {

using CaseFn = void (*)();

// Registers a case (KYTY_UI_CASE does it during static initialization).
class CaseRegistrar {
public:
	CaseRegistrar(const char* name, CaseFn run);
};

// Sets the fake portal's color-scheme (0 no preference, 1 prefer dark, 2 prefer light) through
// TestPortal.SetColorScheme before the case's gtk_init and adw_init, so that the case starts in a
// desktop with that preference (a static object next to the case:
//   static const KytyUi::InitialColorScheme kDark("theme_initial_dark", 1);).
class InitialColorScheme {
public:
	InitialColorScheme(const char* case_name, guint32 value);
};

// Fails the running case: prints "<case>: FAILED: <what>" to stderr. The case goes on.
void Fail(std::string_view what);

// value; Fail(what) first when it is false.
bool Check(bool value, std::string_view what);

// The number of failures of the running case so far.
[[nodiscard]] int Failures();

// Declares one expected log message of the running case: level (G_LOG_LEVEL_WARNING,
// G_LOG_LEVEL_CRITICAL) in domain, with a message that starts with prefix. It does not fail the
// case, and the case fails unless it arrives exactly once.
void ExpectLog(std::string_view domain, GLogLevelFlags level, std::string_view prefix);

// From now on, records every message below the warning level (message, info, debug) of domain
// whose text starts with prefix, from any thread. GLib's default writer still prints them (debug
// only with G_MESSAGES_DEBUG).
void WatchLog(std::string_view domain, std::string_view prefix);
// The messages the watch with this prefix recorded so far, in order.
[[nodiscard]] std::vector<std::string> WatchedLogs(std::string_view prefix);

// The running case's name.
[[nodiscard]] const std::string& CaseName();

// Where Capture() writes; empty without --capture-dir and KYTY_UI_CAPTURE_DIR.
[[nodiscard]] const std::filesystem::path& CaptureDirectory();

// The names of every registered case, sorted (what --list prints).
[[nodiscard]] std::vector<std::string> CaseNames();

} // namespace KytyUi

#define KYTY_UI_CASE(name)                                                                         \
	static void                          KytyUiCase_##name();                                      \
	static const ::KytyUi::CaseRegistrar kytyUiCaseRegistrar_##name(#name, &KytyUiCase_##name);    \
	static void                          KytyUiCase_##name()
