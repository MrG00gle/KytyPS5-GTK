// The GTK launcher's own texts for the errors of Launcher_Core (src/launcher/gtk/coreMessageText.h;
// phase 9 plan V1, D4, D5, D8; Requirement 6.12). Real core errors are made for every kind that a
// test can reach (fault hooks, scratch files, the POSIX process runner, fake HTTP clients), and for
// each one the test asserts that its text's msgid (and plural msgid) is a literal of
// coreMessageText.cpp's N_(), _(), NGettext(), and NGettextNoop() calls, read from the source,
// and that in the C locale (no catalog: gettext returns the msgid) the text is the core's English
// message again, so nothing changes there. Every enumerator of every converted kind is also
// mapped on an error made here. Render() is checked on its own: conversions in order and by
// position, %%, and plural forms. The updater's session messages are covered by
// launcher_gtk_update_message_text. No display, no gtk_init.

#include "launcher/core/archiveJob.h"
#include "launcher/core/atomicFile.h"
#include "launcher/core/cheatCatalog.h"
#include "launcher/core/cheats.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/configurationFields.h"
#include "launcher/core/dataPaths.h"
#include "launcher/core/emulatorProcess.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameScanner.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/remoteCheats.h"
#include "launcher/core/settingsStore.h"
#include "launcher/gtk/coreMessageText.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/sourceLiterals.h"
#include "support/helperPaths.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"
#include "updater/core/processRunner.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kyty::gtk::CoreText;
using kyty::gtk::Render;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CoreMessageTextTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

const std::set<std::string>& MappingLiterals() {
	static const std::set<std::string> literals = KytyTests::TranslatableLiterals(
	    fs::path(KYTY_TEST_SOURCE_DIR) / "src" / "launcher" / "gtk" / "coreMessageText.cpp");
	return literals;
}

// text is the launcher's: a msgid of coreMessageText.cpp (and a plural one when it has two forms).
void CheckKey(const CoreText& text, const std::string& name) {
	Check(text.msgid != nullptr, name + ": a msgid");
	Check(MappingLiterals().count(text.msgid) == 1,
	      name + ": the msgid \"" + text.msgid + "\" is a literal of coreMessageText.cpp");
	Check(text.plural_msgid == nullptr || MappingLiterals().count(text.plural_msgid) == 1,
	      name + ": the plural msgid is a literal of coreMessageText.cpp");
}

// CheckKey, and in the C locale the text is expected (the core's English message).
void CheckText(const CoreText& text, const std::string& expected, const std::string& name) {
	CheckKey(text, name);
	const std::string shown = Render(text);
	Check(shown == expected, name + ": \"" + shown + "\", expected \"" + expected + "\"");
}

// ---- Render

void TestRender() {
	Check(Render(CoreText {}).empty(), "an empty text");
	Check(Render(CoreText {nullptr, nullptr, 0, {"as it is: 100%"}}) == "as it is: 100%",
	      "dynamic data only is shown as it is");
	Check(Render(CoreText {"%s and %s", nullptr, 0, {"a", "b"}}) == "a and b", "in order");
	Check(Render(CoreText {"%2$s before %1$s", nullptr, 0, {"a", "b"}}) == "b before a",
	      "by position");
	Check(Render(CoreText {"%llu%% of %lld, %d, %x", nullptr, 0, {"5", "-3", "7", "ff"}}) ==
	          "5% of -3, 7, ff",
	      "numbers are inserted as their text; %% is a percent sign");
	Check(Render(CoreText {"%s, %s.", nullptr, 0, {"one"}}) == "one, .",
	      "a missing argument is empty");
	Check(Render(CoreText {"ends with %", nullptr, 0, {}}) == "ends with %", "a lone % stays");
	const CoreText one {"%s file", "%s files", 1, {"1"}};
	const CoreText two {"%s file", "%s files", 2, {"2"}};
	Check(Render(one) == "1 file" && Render(two) == "2 files", "the count picks the form");
	Check(kyty::gtk::OsErrorText(ENOENT) == ErrorText(ENOENT),
	      "the system's text in the C locale is the core's");
}

// ---- atomicFile.h

void TestIoErrors() {
	using kyty::core::AtomicFileStep;
	using kyty::core::IoErrorKind;
	const TempDir         temp("kyty_core_text");
	std::set<IoErrorKind> seen;
	const auto check = [&seen](const kyty::core::IoError& error, const std::string& name) {
		CheckText(kyty::gtk::IoErrorText(error), error.message, name);
		seen.insert(error.kind);
	};

	const auto missing = kyty::core::ReadFileLimited(temp.Path() / "missing", 10);
	check(missing.error(), "a missing file");
	const auto directory = kyty::core::ReadFileLimited(temp.Path(), 10);
	check(directory.error(), "a directory");
	const auto ten = temp.WriteFile("ten", "0123456789");
	check(kyty::core::ReadFileLimited(ten, 9).error(), "a file over the limit");
	const auto plural_one = kyty::core::ReadFileLimited(ten, 1).error();
	CheckText(kyty::gtk::IoErrorText(plural_one),
	          "cannot read " + ten.string() + " (limit 1 byte): " + ErrorText(EFBIG),
	          "a limit of 1 byte (the singular form; the core says \"1 bytes\")");

	for (const auto step: {AtomicFileStep::CreateTemp, AtomicFileStep::Write, AtomicFileStep::Sync,
	                       AtomicFileStep::Close, AtomicFileStep::Rename}) {
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](AtomicFileStep current) { return current == step ? ENOSPC : 0; });
		const auto written = kyty::core::WriteFileAtomically(ten, "x");
		kyty::core::SetAtomicFileFaultForTesting({});
		Check(!written, "the injected write fault");
		check(written.error(), "write step " + std::to_string(static_cast<int>(step)));
	}
	check(kyty::core::WriteFileAtomically(temp.Path() / "dir" / "", "x").error(),
	      "a target without a file name");
	const auto loop = temp.MakeSymlink("loop", "loop");
	check(kyty::core::WriteFileAtomically(loop, "x").error(), "a symbolic link loop");

	// SaveSettingsStore: the refusal and a directory that cannot be created.
	temp.MakeDirectories("ini-dir/Kyty.ini");
	auto refused = kyty::core::OpenSettingsStore(temp.Path() / "ini-dir" / "Kyty.ini");
	Check(refused.load_error.has_value(), "a Kyty.ini that is a directory cannot be loaded");
	check(*refused.load_error, "the load error");
	check(kyty::core::SaveSettingsStore(refused, {}).error(), "the refusal to replace it");
	kyty::core::SettingsStoreFile under_file;
	under_file.path = ten / "Kyty.ini";
	check(kyty::core::SaveSettingsStore(under_file, {}).error(), "a directory below a file");
	check(kyty::core::WriteGameSettingsFile(temp.Path() / "export.json", "", {}).error(),
	      "an export without a title ID");

	// A symbolic link that cannot be read after it was seen is a race; its text is checked here.
	kyty::core::IoError link;
	link.kind         = IoErrorKind::ReadLink;
	link.path         = "/t/a";
	link.detail       = "/t/b";
	link.error_number = EACCES;
	CheckText(kyty::gtk::IoErrorText(link),
	          "cannot read the symbolic link /t/b: " + ErrorText(EACCES), "an unreadable link");
	seen.insert(IoErrorKind::ReadLink);
	for (int kind = 0; kind <= static_cast<int>(IoErrorKind::NoTitleId); kind++) {
		Check(seen.count(static_cast<IoErrorKind>(kind)) == 1,
		      "IoErrorKind " + std::to_string(kind) + " was shown");
	}
}

// ---- gameConfiguration.h: the rule of every field

void TestValidationRules() {
	using kyty::core::ValidationRule;
	// Every field has a rule text; only a field whose rule no value of its kind breaks gets the
	// general one.
	for (const auto& field: kyty::core::GameConfigurationFields()) {
		const std::string key(field.name);
		const auto text = kyty::gtk::ValidationRuleText({key, {}, ValidationRule::Field, {}});
		CheckKey(text, "the rule of " + key);
		const bool general = std::string(text.msgid) == "The value is not valid.";
		Check(general == (key == "audio_input_device"),
		      key + ": the general text only for a field without a rule of its own");
	}
	// The texts the settings editor showed before (phase 6 D46), on real Validate() errors.
	kyty::core::GameConfiguration configuration;
	configuration.user_id = 254;
	auto error            = kyty::core::Validate(configuration);
	CheckText(kyty::gtk::ValidationRuleText(*error),
	          "The user ID must be between 0 and 2147482879 and must not be 254 (everyone) or 255 "
	          "(system).",
	          "the user ID");
	configuration.user_id            = 1000;
	configuration.printf_direction   = kyty::core::LogDirection::File;
	configuration.printf_output_file = "";
	error                            = kyty::core::Validate(configuration);
	Check(error && error->rule == ValidationRule::MissingOutputPath, "a missing output path");
	CheckText(kyty::gtk::ValidationRuleText(*error),
	          "The printf output file must not be empty while printf output goes to a file.",
	          "the printf output file");
	configuration.printf_output_file = "out.txt";
	configuration.host_input_mapping = {"bad"};
	error                            = kyty::core::Validate(configuration);
	Check(error && error->rule == ValidationRule::InvalidMappingEntries, "an invalid mapping");
	CheckText(kyty::gtk::ValidationRuleText(*error),
	          "Each input mapping entry must have the form Name=Value with a non-empty name and "
	          "value.",
	          "the input mapping");
	CheckText(kyty::gtk::ValidationRuleText({"present_mode", {}, ValidationRule::Field, {}}),
	          "The value must be one of the listed choices.", "an enum field");
	CheckText(kyty::gtk::ValidationRuleText({"hdr_enabled", {}, ValidationRule::Field, {}}),
	          "The value must be true or false.", "a bool field");
}

// ---- cheats.h

std::string WithMod(const std::string& mod, const std::string& extra = "") {
	return R"({"id":"ppsa01234","version":"01.000","process":"eboot.bin",)" + extra +
	       R"("mods":[)" + mod + "]}";
}

const char kWrite[] = R"({"offset":"1","on":"90","off":"0F"})";

void TestCheatProblems() {
	using kyty::core::CheatProblemKind;
	std::set<CheatProblemKind> seen;
	const auto check = [&seen](const kyty::core::CheatProblem& problem, const std::string& name) {
		CheckText(kyty::gtk::CheatProblemText(problem), problem.message, name);
		seen.insert(problem.kind);
	};
	for (const std::string bytes: {"{", "[]", "{}", R"({"mods":[1]})", R"({"mods":[{}]})",
	                               R"({"mods":[{"name":"a","enabled":1}]})"}) {
		const auto parsed = kyty::core::ParseCheatDocument(bytes, "/d/_Patches/PPSA01234.json");
		check(parsed.error(), "the document " + bytes);
	}
	const auto refused = [&check](const std::string& text, const std::string& title,
	                              const std::string& version, const std::string& process,
	                              const std::string& name) {
		const auto document = kyty::core::ParseCheatDocument(text, "remote.json");
		Check(document.has_value(), name + ": parses");
		const auto problem = kyty::core::ValidateRemoteImport(*document, title, version, process);
		Check(problem.has_value(), name + ": refused");
		check(*problem, name);
	};
	const std::string mod = std::string(R"({"name":"m","memory":[)") + kWrite + "]}";
	refused(WithMod(mod), "PPSA05678", "01.000", "eboot.bin", "another title");
	refused(WithMod(mod), "PPSA01234", "01.001", "eboot.bin", "another version");
	refused(WithMod(mod), "PPSA01234", "01.000", "game.elf", "another executable");
	refused(WithMod(""), "PPSA01234", "01.000", "eboot.bin", "no cheats");
	refused(WithMod(mod, R"("master":{},)"), "PPSA01234", "01.000", "eboot.bin", "master codes");
	refused(WithMod(std::string(R"({"name":"m","module_name":"x","memory":[)") + kWrite + "]}"),
	        "PPSA01234", "01.000", "eboot.bin", "module-specific cheats");
	refused(WithMod(R"({"name":"m","memory":[{"offset":"1","on":"90","off":"0F","section":0}]})"),
	        "PPSA01234", "01.000", "eboot.bin", "addressing features");
	refused(WithMod(R"({"name":"m"})"), "PPSA01234", "01.000", "eboot.bin", "unsupported memory");
	check(*kyty::core::ValidateRemoteImport({}, "PPSA01234", "01.000", "eboot.bin"),
	      "a document without JSON");
	for (int kind = 0; kind <= static_cast<int>(CheatProblemKind::UnsupportedMemory); kind++) {
		Check(seen.count(static_cast<CheatProblemKind>(kind)) == 1,
		      "CheatProblemKind " + std::to_string(kind) + " was shown");
	}
}

void TestCheatSaveErrors() {
	using kyty::core::CheatSaveFailure;
	const TempDir temp("kyty_core_text");
	const auto    document = *kyty::core::ParseCheatDocument(
	    WithMod(std::string(R"({"name":"m","memory":[)") + kWrite + "]}"), "remote.json");
	std::set<CheatSaveFailure> failures;
	const auto                 check = [&failures](const kyty::core::CheatSaveError& error,
	                                               const std::string&                name) {
		CheckText(kyty::gtk::CheatSaveErrorText(error), error.message, name);
		failures.insert(error.failure);
	};
	const auto path = temp.Path() / "_Patches" / "PPSA01234.json";
	Check(kyty::core::SaveIfUnchanged(path, document, std::nullopt).has_value(), "a new file");
	check(kyty::core::SaveIfUnchanged(path, document, std::nullopt).error(), "confirmation");
	check(kyty::core::SaveIfUnchanged(path, document, std::string_view("x")).error(),
	      "changed on disk");
	check(kyty::core::SaveIfUnchanged({}, document, std::nullopt).error(), "an empty path");
	temp.MakeDirectories("dir.json");
	check(kyty::core::SaveIfUnchanged(temp.Path() / "dir.json", document, std::nullopt).error(),
	      "a directory at the path");
	const TempDir blocked("kyty_core_text_dir");
	blocked.MakeSymlink("/nonexistent/kyty-core-text", "_Patches");
	check(kyty::core::SaveIfUnchanged(blocked.Path() / "_Patches" / "PPSA01234.json", document,
	                                  std::nullopt)
	          .error(),
	      "a directory that cannot be created");
	const auto bytes = *kyty::core::ReadFileLimited(path, kyty::core::kMaxCheatFileBytes);
	kyty::core::SetAtomicFileFaultForTesting([](kyty::core::AtomicFileStep step) {
		return step == kyty::core::AtomicFileStep::Rename ? EIO : 0;
	});
	const auto write = kyty::core::SaveIfUnchanged(path, document, std::string_view(bytes));
	kyty::core::SetAtomicFileFaultForTesting({});
	check(write.error(), "a write that fails");
	check(kyty::core::ImportRemoteCheats(temp.Path(), "CUSA01234", document, std::nullopt).error(),
	      "an unsupported title");
	for (int failure = 1; failure <= static_cast<int>(CheatSaveFailure::Write); failure++) {
		Check(failures.count(static_cast<CheatSaveFailure>(failure)) == 1,
		      "CheatSaveFailure " + std::to_string(failure) + " was shown");
	}
}

// ---- cheatCatalog.h, remoteCheats.h

void TestCheatDownloads() {
	using kyty::core::HttpFailure;
	using kyty::core::HttpResult;
	constexpr uint64_t kLimit = 8u << 20u;
	for (const HttpResult& result:
	     {HttpResult {HttpFailure::Network, 0, "host not found"},
	      HttpResult {HttpFailure::Status, 503, ""}, HttpResult {HttpFailure::TooLarge, 200, ""},
	      HttpResult {HttpFailure::Stalled, 200, "no data"},
	      HttpResult {HttpFailure::TimedOut, 0, ""}, HttpResult {HttpFailure::Cancelled, 0, ""}}) {
		const auto error = kyty::core::CheatFetchErrorFor(result, kLimit);
		CheckText(kyty::gtk::CheatFetchErrorText(error), error.message,
		          "download failure " + std::to_string(static_cast<int>(result.failure)));
	}

	// The catalog: one line per index that failed, as the core joins them.
	KytyTests::FakeScheduler  scheduler;
	KytyTests::FakeHttpClient http(scheduler);
	KytyTests::FakeResponse   network;
	network.network_failure = true;
	network.failure_message = "connection refused";
	http.AddResponse(kyty::core::CheatIndexUrl("json"), network);
	KytyTests::FakeResponse status;
	status.status = 404;
	http.AddResponse(kyty::core::CheatIndexUrl("mc4"), status);
	KytyTests::FakeResponse skipped;
	skipped.body = "bad\nalso bad\nPPSA01234_1.0_a.shn=A\n";
	http.AddResponse(kyty::core::CheatIndexUrl("shn"), skipped);
	std::vector<kyty::core::CheatCatalogResult> results;
	kyty::core::CheatCatalog                    catalog(
	    http, [&](const kyty::core::CheatCatalogResult& result) { results.push_back(result); });
	catalog.Load("PPSA01234");
	scheduler.RunUntilIdle();
	Check(results.size() == 1 && results[0].failures.size() == 3, "three failed indexes");
	for (const auto& failure: results[0].failures) {
		CheckKey(kyty::gtk::CheatIndexFailureText(failure), "the index " + failure.format);
	}
	Check(kyty::gtk::CheatCatalogErrorText(results[0]) == results[0].error,
	      "the catalog's lines: \"" + kyty::gtk::CheatCatalogErrorText(results[0]) + "\"");
	kyty::core::CheatIndexFailure invalid {"mc4", {}, kyty::core::CheatIndexProblem::Invalid, 0};
	CheckText(kyty::gtk::CheatIndexFailureText(invalid), "MC4: Invalid cheat index.",
	          "an invalid index");
	kyty::core::CheatIndexFailure one {
	    "json", {}, kyty::core::CheatIndexProblem::SkippedEntries, 1};
	CheckText(kyty::gtk::CheatIndexFailureText(one), "JSON: Skipped 1 invalid cheat index entry.",
	          "one skipped line (the singular form; the core says \"entries\")");

	// A remote file: the download failure, the parse error, a mismatch.
	std::vector<kyty::core::RemoteCheatResult> fetched;
	kyty::core::RemoteCheats                   remote(
	    http, [&](const kyty::core::RemoteCheatResult& result) { fetched.push_back(result); });
	const kyty::core::RemoteCheatFile file {"PPSA01234_01.000_a.json", "", "01.000", "json"};
	const kyty::core::CheatGame       game {"PPSA01234", "01.000", "eboot.bin"};
	const auto                        url = kyty::core::RemoteCheatFileUrl(file);
	http.AddResponse(url, status);
	KytyTests::FakeResponse invalid_file;
	invalid_file.body = R"({"mods":[{"name":1}]})";
	http.AddResponse(url, invalid_file);
	KytyTests::FakeResponse other_title;
	other_title.body = WithMod(std::string(R"({"name":"m","memory":[)") + kWrite + "]}");
	http.AddResponse(url, other_title);
	for (const auto& name: {"a download failure", "a parse error", "a mismatch"}) {
		Check(remote
		          .Fetch(file, std::string(name) == "a mismatch"
		                           ? kyty::core::CheatGame {"PPSA05678", "01.000", "eboot.bin"}
		                           : game)
		          .has_value(),
		      std::string(name) + ": fetched");
		scheduler.RunUntilIdle();
		Check(!fetched.empty() && !fetched.back().importable, std::string(name) + ": refused");
		CheckText(kyty::gtk::RemoteCheatErrorText(fetched.back()), fetched.back().error, name);
	}
}

// ---- dataPaths.h, emulatorProcess.h

void TestWritableDirectory() {
	using kyty::core::WritableProbeStep;
	const TempDir temp("kyty_core_text");
	for (const auto step:
	     {WritableProbeStep::CreateDirectories, WritableProbeStep::Create, WritableProbeStep::Write,
	      WritableProbeStep::Close, WritableProbeStep::Remove}) {
		kyty::core::SetWritableProbeFaultForTesting(
		    [step](WritableProbeStep current) { return current == step ? EROFS : 0; });
		const auto result = kyty::core::EnsureWritableDirectory(temp.Path() / "data");
		kyty::core::SetWritableProbeFaultForTesting({});
		Check(!result, "the injected fault");
		CheckText(kyty::gtk::WritableDirectoryErrorText(result.error()), result.error().message,
		          "probe step " + std::to_string(static_cast<int>(step)));
	}
	const auto file   = temp.WriteFile("file", "x");
	const auto result = kyty::core::EnsureWritableDirectory(file);
	CheckText(kyty::gtk::WritableDirectoryErrorText(result.error()), result.error().message,
	          "a file in the way");
}

void TestSpawnReasons() {
	using kyty::core::SpawnStep;
	const TempDir temp("kyty_core_text");
	// The errno text alone has no msgid: it is the system's (OsErrorText).
	const auto check = [](const kyty::core::SpawnError& error, const std::string& name) {
		const auto text = kyty::gtk::SpawnReasonText(error);
		if (text.msgid != nullptr) {
			CheckKey(text, name);
		}
		Check(Render(text) == error.reason, name + ": \"" + Render(text) + "\"");
	};
	check(kyty::core::SpawnEmulator({temp.Path() / "missing", {}, {}}).error(), "missing");
	check(kyty::core::SpawnEmulator({temp.Path(), {}, {}}).error(), "a directory");
	const auto echo = KytyTests::HelperPath("argv_echo");
	check(kyty::core::SpawnEmulator({echo, {std::string("a\0b", 3)}, {}}).error(), "a NUL byte");
	for (const auto step:
	     {SpawnStep::Pipe, SpawnStep::FileActions, SpawnStep::Attributes, SpawnStep::Spawn}) {
		for (const auto& cwd: {fs::path(), temp.Path()}) {
			kyty::core::SetSpawnFaultForTesting(
			    [step](SpawnStep current) { return current == step ? EAGAIN : 0; });
			const auto spawned = kyty::core::SpawnEmulator({echo, {}, cwd});
			kyty::core::SetSpawnFaultForTesting({});
			Check(!spawned, "the injected spawn fault");
			check(spawned.error(), "spawn step " + std::to_string(static_cast<int>(step)) +
			                           (cwd.empty() ? "" : " with a working directory"));
		}
	}
	Check(std::string(
	          kyty::gtk::SpawnReasonText(kyty::core::SpawnEmulator({temp.Path(), {}, {}}).error())
	              .msgid) == "not a regular file",
	      "not a regular file is the launcher's text");
}

// ---- compatibility.h, archiveJob.h, gameScanner.h

void TestCompatibilityFile() {
	const TempDir temp("kyty_core_text");
	temp.WriteFile("invalid.json", "{\"PPSA1\": ");
	temp.WriteFile("array.json", "[]");
	temp.MakeDirectories("directory.json");
	for (const char* name: {"invalid.json", "array.json", "directory.json"}) {
		const auto loaded = kyty::core::LoadLocalCompatibility(temp.Path() / name);
		Check(!loaded, std::string(name) + ": refused");
		CheckText(kyty::gtk::CompatibilityFileErrorText(loaded.error()), loaded.error().message,
		          name);
	}
}

void TestArchiveErrors() {
	using kyty::core::ArchiveErrorKind;
	const TempDir temp("kyty_core_text");
	const auto    game = temp.MakeDirectories("Game");
	temp.WriteFile("Game/eboot.bin", "ELF");
	std::set<ArchiveErrorKind> seen;
	const auto check = [&seen](const kyty::core::ArchiveError& error, const std::string& name) {
		CheckText(kyty::gtk::ArchiveErrorText(error), error.message, name);
		seen.insert(error.kind);
	};
	temp.WriteFile("Game.zar", "existing");
	check(kyty::core::ArchiveJob::Prepare(game).error(), "Prepare: an existing archive");
	fs::remove(temp.Path() / "Game.zar");
	const std::string long_name(252, 'g');
	check(kyty::core::ArchiveJob::Prepare(temp.MakeDirectories(long_name)).error(),
	      "Prepare: a path that cannot be checked");

	const auto run = [&](const kyty::core::ArchivePacker& packer, const std::string& name,
	                     const std::function<void()>& before = {}) {
		auto job = kyty::core::ArchiveJob::Prepare(game, packer);
		Check(job.has_value(), name + ": prepared");
		if (before) {
			before();
		}
		kyty::core::CancelToken cancel;
		job->Run({}, cancel);
		Check(job->Outcome() == kyty::core::ArchiveOutcome::Failed, name + ": failed");
		Check(job->Failure().message == job->Error(), name + ": the failure is Error()");
		check(job->Failure(), name);
		fs::remove_all(temp.Path() / "Game.zar");
	};
	const kyty::core::ArchivePacker failing = [](const fs::path&, const fs::path&,
	                                             const std::function<bool(uint64_t, uint64_t)>&) {
		return false;
	};
	run(failing, "Run: the packer fails");
	run(failing, "Run: an archive made after Prepare",
	    [&temp] { temp.WriteFile("Game.zar", "someone else's"); });
	// A directory that is not empty cannot be removed with std::filesystem::remove.
	run(
	    [](const fs::path&, const fs::path& archive,
	       const std::function<bool(uint64_t, uint64_t)>&) {
		    std::error_code ec;
		    fs::create_directories(archive / "inner", ec);
		    return false;
	    },
	    "Run: a partial archive that cannot be removed");
	kyty::core::ArchiveError run_check;
	run_check.kind         = ArchiveErrorKind::CheckFailed;
	run_check.source       = "/g/Game";
	run_check.archive      = "/g/Game.zar";
	run_check.error_number = EACCES;
	CheckText(kyty::gtk::ArchiveErrorText(run_check),
	          "Could not compress /g/Game: /g/Game.zar: cannot check whether it exists: " +
	              ErrorText(EACCES) + ".",
	          "Run: a path that cannot be checked");
	Check(kyty::gtk::ArchiveErrorText({}).msgid == nullptr, "no error: no text");
	for (int kind = 1; kind <= static_cast<int>(ArchiveErrorKind::CompressFailed); kind++) {
		Check(seen.count(static_cast<ArchiveErrorKind>(kind)) == 1,
		      "ArchiveErrorKind " + std::to_string(kind) + " was shown");
	}
}

void TestScanWarnings() {
	kyty::core::ScanWarning unresolved {"/g/Game", "cannot resolve the canonical path",
	                                    kyty::core::ScanWarningKind::NoCanonicalPath};
	CheckText(kyty::gtk::ScanWarningReasonText(unresolved), unresolved.reason,
	          "an unresolvable path");
	kyty::core::ScanWarning unreadable {"/g", ErrorText(EACCES),
	                                    kyty::core::ScanWarningKind::Unreadable};
	const auto              text = kyty::gtk::ScanWarningReasonText(unreadable);
	Check(text.msgid == nullptr && Render(text) == ErrorText(EACCES),
	      "the system's text is shown as it is");
}

// ---- processRunner.h

void TestProcessErrors() {
	using kyty::updater::ProcessErrorKind;
	const TempDir              temp("kyty_core_text");
	const auto                 runner = kyty::updater::MakePosixProcessRunner();
	std::set<ProcessErrorKind> seen;
	const auto check = [&seen](const kyty::updater::ProcessError& error, const std::string& name) {
		CheckText(kyty::gtk::ProcessErrorText(error), error.message, name);
		seen.insert(error.kind);
	};
	check(runner->Run(temp.Path() / "missing", {}, {}).failure, "a missing program");
	check(runner->Run(temp.Path() / "missing", {}, temp.Path()).failure,
	      "a missing program with a working directory");
	check(runner->Run("/bin/true", {std::string("\0", 1)}, {}).failure, "a NUL byte");
	check(runner->StartDetached("/bin/true", {}, {}, temp.Path() / "no" / "log").error(),
	      "a log file that cannot be created");
	for (const auto& [kind, message]: std::vector<std::pair<ProcessErrorKind, std::string>> {
	         {ProcessErrorKind::Pipe,
	          "Cannot start /p: cannot create a pipe: " + ErrorText(EMFILE)},
	         {ProcessErrorKind::Prepare,
	          "Cannot start /p: cannot prepare the spawn: " + ErrorText(EMFILE)},
	         {ProcessErrorKind::Wait, "Cannot wait for /p: " + ErrorText(EMFILE)},
	     }) {
		kyty::updater::ProcessError error;
		error.kind         = kind;
		error.program      = "/p";
		error.error_number = EMFILE;
		error.message      = message;
		check(error, message);
	}
	for (int kind = 1; kind <= static_cast<int>(ProcessErrorKind::Wait); kind++) {
		Check(seen.count(static_cast<ProcessErrorKind>(kind)) == 1,
		      "ProcessErrorKind " + std::to_string(kind) + " was shown");
	}
	kyty::updater::ProcessError text_only;
	text_only.message = "a runner's own words";
	const auto text   = kyty::gtk::ProcessErrorText(text_only);
	Check(text.msgid == nullptr && Render(text) == text_only.message,
	      "a runner's text without a kind is shown as it is");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CoreMessageTextTests");
	Check(!MappingLiterals().empty(), "coreMessageText.cpp has translatable literals");
	TestRender();
	TestIoErrors();
	TestValidationRules();
	TestCheatProblems();
	TestCheatSaveErrors();
	TestCheatDownloads();
	TestWritableDirectory();
	TestSpawnReasons();
	TestCompatibilityFile();
	TestArchiveErrors();
	TestScanWarnings();
	TestProcessErrors();
	std::printf("CoreMessageTextTests: all tests passed (%zu literals of coreMessageText.cpp)\n",
	            MappingLiterals().size());
	return 0;
}
