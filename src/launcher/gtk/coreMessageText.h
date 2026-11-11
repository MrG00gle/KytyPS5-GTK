#pragma once

// The GTK launcher's own texts for the errors and messages of Launcher_Core and the Updater (plan
// V1, phase 9 D4, D5; design.md Error Handling "Core returns errors as values"; Requirement 6.12).
// The core says what failed with a kind and its parts (paths, errno values, numbers, a URL or a
// tag, the parser's or a library's text); its English message is for the log. Every function here
// picks the launcher's msgid for the kind and inserts only those parts, so that a translation can
// say every sentence. In the C locale the texts equal the core's English messages, so nothing
// changes there. An errno is shown as g_strerror() says it, which glibc translates.
//
// The content_check_core_text CTest (tools/checks/content_checks.py --check core_text) keeps every
// other GTK source from showing a core message, reason, or error member as it is.

#include "launcher/core/archiveJob.h"
#include "launcher/core/atomicFile.h"
#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/emulatorProcess.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameScanner.h"
#include "launcher/core/remoteCheats.h"
#include "updater/core/processRunner.h"
#include "updater/core/updateSession.h"

#include <string>
#include <vector>

namespace kyty::gtk {

// A text before it is shown. msgid is a literal of coreMessageText.cpp in the kyty-launcher
// domain; nullptr for a text that is dynamic data only (the system's or a library's words, shown
// as arguments[0]). plural_msgid is set when count picks the form (NGettext). arguments replace
// the conversions of the translated text (%s, %llu, %d, ..., and the positional %1$s forms) in
// order, as they are shown; %% is a percent sign.
struct CoreText {
	const char*              msgid        = nullptr;
	const char*              plural_msgid = nullptr;
	unsigned long            count        = 0;
	std::vector<std::string> arguments;
};

// The translated text with its arguments.
std::string Render(const CoreText& text);

// The system's text for an errno value (g_strerror).
std::string OsErrorText(int error_number);

// atomicFile.h: "cannot open <path>: <reason>" and the other steps, the reason from error_number.
CoreText IoErrorText(const core::IoError& error);

// The rule a field's value breaks (Requirement 9.4), as the settings editor says it: by
// error.rule, then by the field key (error.field); error.message is not used.
CoreText ValidationRuleText(const core::ValidationError& error);

// cheats.h: why a cheat document is not valid ("<file>: <reason>" when problem.file is set) or must
// not be imported.
CoreText CheatProblemText(const core::CheatProblem& problem);

// cheats.h: why saving or importing a cheat file failed.
CoreText CheatSaveErrorText(const core::CheatSaveError& error);

// cheatCatalog.h: why a collection download failed.
CoreText CheatFetchErrorText(const core::CheatFetchError& error);

// cheatCatalog.h: "<FORMAT>: <reason>" for an index that failed or had invalid lines.
CoreText CheatIndexFailureText(const core::CheatIndexFailure& failure);

// CheatIndexFailureText of every failure of result, one per line; "" when there is none.
std::string CheatCatalogErrorText(const core::CheatCatalogResult& result);

// remoteCheats.h: why a downloaded file cannot be previewed or imported (the download failure,
// else the parse error or the reason it does not match the game); an empty text when there is
// none.
CoreText RemoteCheatErrorText(const core::RemoteCheatResult& result);

// dataPaths.h: "<dir>: cannot <step>: <reason>".
CoreText WritableDirectoryErrorText(const core::WritableDirectoryError& error);

// emulatorProcess.h: the reason part of "Cannot start <executable>: <reason>" (Requirement 10.9).
CoreText SpawnReasonText(const core::SpawnError& error);

// compatibility.h: why the --local compatibility file cannot be used.
CoreText CompatibilityFileErrorText(const core::CompatibilityFileError& error);

// archiveJob.h: why Prepare or Run failed ("Could not compress <source>: <reason>" for Run's
// errors other than CompressFailed); an empty text for kind None.
CoreText ArchiveErrorText(const core::ArchiveError& error);

// gameScanner.h: why the path could not be read (the system's text, or the canonical path).
CoreText ScanWarningReasonText(const core::ScanWarning& warning);

// processRunner.h: "Cannot start <program>: <reason>" and "Cannot wait for <program>: <reason>";
// a runner's text without a kind as it is.
CoreText ProcessErrorText(const updater::ProcessError& error);

// updateSession.h: the session's message (outcome.detail), with the unverified installed release's
// frame; dynamic parts have invalid UTF-8 replaced. An empty text without a message.
CoreText SessionMessageText(const updater::SessionOutcome& outcome);

} // namespace kyty::gtk
