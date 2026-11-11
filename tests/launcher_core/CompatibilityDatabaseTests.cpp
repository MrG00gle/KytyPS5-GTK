// Example tests of the Compatibility_Database parser, lookup, model, and the --local maintainer
// file (src/launcher/core/compatibility.h, task 14.1; Requirements 8.5, 8.6): every status alias
// of the Qt launcher, trimmed and upper-cased keys, unknown statuses, non-object members, invalid
// JSON and non-object roots with a message describing the error, the model keeping its statuses
// when a load has no database, and the --local load, edit, and atomic save in the Qt format.

#include "launcher/core/atomicFile.h"
#include "launcher/core/compatibility.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::AtomicFileStep;
using kyty::core::CompatibilityDatabase;
using kyty::core::CompatibilityEntry;
using kyty::core::CompatibilityLoadResult;
using kyty::core::CompatibilityModel;
using kyty::core::CompatibilityStatus;
using kyty::core::Lookup;
using kyty::core::ParseCompatibility;
using kyty::core::ParseCompatibilityStatus;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CompatibilityDatabaseTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool Contains(const std::string& text, std::string_view part) {
	return text.find(part) != std::string::npos;
}

std::string ReadBytes(const fs::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "cannot read " + path.string());
	return *bytes;
}

CompatibilityDatabase ParseOk(std::string_view json) {
	auto database = ParseCompatibility(json);
	Check(database.has_value(), "parses: " + std::string(json) +
	                                (database ? std::string() : " (" + database.error() + ")"));
	return *database;
}

// A JSON string literal for text (valid UTF-8).
std::string JsonString(std::string_view text) {
	std::string out = "\"";
	for (const char c: text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
			out += escaped;
		} else {
			out += c;
		}
	}
	return out + "\"";
}

CompatibilityEntry Entry(CompatibilityStatus status, std::string comment = {}) {
	return CompatibilityEntry {status, std::move(comment)};
}

void TestStatusAliases() {
	const std::vector<std::pair<std::string, CompatibilityStatus>> cases = {
	    {"InGame", CompatibilityStatus::InGame},
	    {"In game", CompatibilityStatus::InGame},
	    {"MainMenu", CompatibilityStatus::MainMenu},
	    {"Main menu", CompatibilityStatus::MainMenu},
	    {"Logo", CompatibilityStatus::Logo},
	    {"DoesntBoot", CompatibilityStatus::DoesntBoot},
	    {"Doesn't boot", CompatibilityStatus::DoesntBoot},
	    // Trimmed as the Qt launcher did.
	    {"  InGame\t", CompatibilityStatus::InGame},
	    {"\nMain menu ", CompatibilityStatus::MainMenu},
	    {"\xE3\x80\x80Logo\xC2\xA0", CompatibilityStatus::Logo},
	    // Letter case and spelling count.
	    {"ingame", CompatibilityStatus::Unknown},
	    {"INGAME", CompatibilityStatus::Unknown},
	    {"In Game", CompatibilityStatus::Unknown},
	    {"Main Menu", CompatibilityStatus::Unknown},
	    {"logo", CompatibilityStatus::Unknown},
	    {"Doesnt boot", CompatibilityStatus::Unknown},
	    {"Doesn\xE2\x80\x99t boot", CompatibilityStatus::Unknown},
	    {"In  game", CompatibilityStatus::Unknown},
	    {"Playable", CompatibilityStatus::Unknown},
	    {"Unknown", CompatibilityStatus::Unknown},
	    {"", CompatibilityStatus::Unknown},
	    {" ", CompatibilityStatus::Unknown},
	};
	for (const auto& [text, status]: cases) {
		Check(ParseCompatibilityStatus(text) == status, "status alias '" + text + "'");
		const auto database = ParseOk(R"({"PPSA01234": {"status": )" + JsonString(text) + "}}");
		Check(Lookup(database, "PPSA01234").status == status, "parsed status '" + text + "'");
	}

	Check(kyty::core::CompatibilityStatusKey(CompatibilityStatus::InGame) == "InGame" &&
	          kyty::core::CompatibilityStatusKey(CompatibilityStatus::MainMenu) == "MainMenu" &&
	          kyty::core::CompatibilityStatusKey(CompatibilityStatus::Logo) == "Logo" &&
	          kyty::core::CompatibilityStatusKey(CompatibilityStatus::DoesntBoot) == "DoesntBoot" &&
	          kyty::core::CompatibilityStatusKey(CompatibilityStatus::Unknown) == "Unknown",
	      "status keys");
	Check(kyty::core::CompatibilityStatusKey(static_cast<CompatibilityStatus>(42)) == "Unknown",
	      "an out-of-range status is written as Unknown");
	for (const auto status: {CompatibilityStatus::InGame, CompatibilityStatus::MainMenu,
	                         CompatibilityStatus::Logo, CompatibilityStatus::DoesntBoot}) {
		Check(ParseCompatibilityStatus(kyty::core::CompatibilityStatusKey(status)) == status,
		      "a status key reads back");
	}
}

void TestParse() {
	const auto                                                    database = ParseOk(R"({
		" ppsa01234 ": {"status": "InGame", "comment": "Runs well"},
		"cusa00001": {"status": "Main menu"},
		"\u3000PPSA00002\u00a0": {"status": "Logo", "comment": ""},
		"PPSA00003": {"status": "Doesn't boot", "comment": 7},
		"PPSA00004": {"status": "Playable", "comment": "unknown status"},
		"PPSA00005": {"comment": "no status"},
		"PPSA00006": {"status": 1},
		"PPSA00007": {"status": null},
		"PPSA00008": {"status": {"value": "InGame"}},
		"PPSA00009": {"status": ["InGame"]},
		"PPSA00010": "InGame",
		"PPSA00011": 3,
		"PPSA00012": null,
		"PPSA00013": ["InGame"],
		"PPSA00014": {},
		"PPSA00015": {"status": "InGame", "extra": true},
		"": {"status": "InGame"},
		"  ": {"status": "InGame"},
		"\t\n": {"status": "Logo"}
	})");
	const std::vector<std::pair<std::string, CompatibilityEntry>> expected = {
	    {"CUSA00001", Entry(CompatibilityStatus::MainMenu)},
	    {"PPSA00002", Entry(CompatibilityStatus::Logo)},
	    {"PPSA00003", Entry(CompatibilityStatus::DoesntBoot)},
	    {"PPSA00004", Entry(CompatibilityStatus::Unknown, "unknown status")},
	    {"PPSA00005", Entry(CompatibilityStatus::Unknown, "no status")},
	    {"PPSA00006", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00007", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00008", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00009", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00010", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00011", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00012", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00013", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00014", Entry(CompatibilityStatus::Unknown)},
	    {"PPSA00015", Entry(CompatibilityStatus::InGame)},
	    {"PPSA01234", Entry(CompatibilityStatus::InGame, "Runs well")},
	};
	Check(database.entries.size() == expected.size(),
	      "every member with a non-empty key is an entry, got " +
	          std::to_string(database.entries.size()));
	for (const auto& [key, entry]: expected) {
		const auto found = database.entries.find(key);
		Check(found != database.entries.end(), "entry " + key);
		Check(found->second == entry, "entry " + key + " status and comment");
	}

	// Two keys with the same normalized ID: the later one in byte order wins ("PPSA1" < "ppsa1").
	auto collision = ParseOk(R"({"ppsa1": {"status": "Logo"}, "PPSA1": {"status": "InGame"}})");
	Check(collision.entries.size() == 1 &&
	          Lookup(collision, "PPSA1").status == CompatibilityStatus::Logo,
	      "colliding keys: the later key in byte order wins");
	collision = ParseOk(R"({"PPSA1 ": {"status": "Logo"}, " PPSA1": {"status": "InGame"}})");
	Check(Lookup(collision, "ppsa1").status == CompatibilityStatus::Logo,
	      "colliding keys with whitespace");
	// A key repeated in the text: the last value counts.
	const auto repeated =
	    ParseOk(R"({"PPSA1": {"status": "Logo"}, "PPSA1": {"status": "InGame"}})");
	Check(Lookup(repeated, "PPSA1").status == CompatibilityStatus::InGame, "a repeated key");

	Check(ParseOk("{}").entries.empty(), "an empty object is an empty database");
	Check(ParseOk(" \n{ }\n ").entries.empty(), "whitespace around the object");
	// Non-ASCII letters keep their case: title IDs are ASCII.
	const auto non_ascii = ParseOk(R"({"ppsa\u00e9": {"status": "Logo"}})");
	Check(non_ascii.entries.count("PPSA\xC3\xA9") == 1, "only ASCII letters are upper-cased");
}

void TestParseErrors() {
	for (const std::string_view json:
	     {std::string_view(""), std::string_view(" "), std::string_view("{"),
	      std::string_view("not json"), std::string_view(R"({"PPSA1": })"),
	      std::string_view(R"({"PPSA1": {"status": "InGame"}} trailing)"),
	      std::string_view(R"({"PPSA1": {"status": "InGame",}})"), std::string_view("{'a': 1}"),
	      std::string_view("{\"PPSA1\": {\"status\": \"\xFF\"}}"),
	      std::string_view("{\"PPSA1\": {\"status\": \"a\nb\"}}"), std::string_view("\xEF\xBB")}) {
		const auto result = ParseCompatibility(json);
		Check(!result.has_value(), "invalid JSON is rejected: " + std::string(json));
		Check(Contains(result.error(), "not valid JSON") && Contains(result.error(), "parse error"),
		      "the message describes the parse error: " + result.error());
		// Phase 8 plan D9: without nlohmann's "[json.exception...]" prefix.
		Check(Contains(result.error(), "the content is not valid JSON: parse error at line ") &&
		          !Contains(result.error(), "[json.exception"),
		      "the parser's reason has no exception prefix: " + result.error());
	}
	for (const std::string_view json: {"[]", "[{\"PPSA1\": {\"status\": \"InGame\"}}]", "1",
	                                   "\"PPSA1\"", "null", "true", "1.5"}) {
		const auto result = ParseCompatibility(json);
		Check(!result.has_value() && Contains(result.error(), "not an object"),
		      "a non-object root is rejected: " + std::string(json));
	}
}

void TestLookup() {
	const auto database =
	    ParseOk(R"({"PPSA01234": {"status": "InGame", "comment": "c"}, "CUSA1": {"status": "x"}})");
	for (const std::string_view id: {"PPSA01234", "ppsa01234", "PpSa01234", " PPSA01234",
	                                 "PPSA01234\t", "\xE3\x80\x80ppsa01234\xE2\x80\xA8"}) {
		Check(Lookup(database, id) == Entry(CompatibilityStatus::InGame, "c"),
		      "lookup '" + std::string(id) + "'");
	}
	Check(Lookup(database, "CUSA1") == Entry(CompatibilityStatus::Unknown),
	      "an unrecognized status is Unknown");
	for (const std::string_view id: {"", " ", "\t\n", "PPSA0123", "PPSA012345", "PPSA 01234"}) {
		Check(Lookup(database, id) == CompatibilityEntry {},
		      "no entry for '" + std::string(id) + "'");
	}
	Check(Lookup(CompatibilityDatabase {}, "PPSA01234") == CompatibilityEntry {},
	      "an empty database");
}

void TestSerialize() {
	CompatibilityDatabase database;
	database.entries["PPSA01234"] = Entry(CompatibilityStatus::InGame, "Runs \"well\"");
	database.entries["CUSA00001"] = Entry(CompatibilityStatus::Logo);
	database.entries["PPSA00002"] = Entry(CompatibilityStatus::Unknown, "no status yet");
	const std::string expected    = "{\n"
	                                "    \"CUSA00001\": {\n"
	                                "        \"comment\": \"\",\n"
	                                "        \"status\": \"Logo\"\n"
	                                "    },\n"
	                                "    \"PPSA00002\": {\n"
	                                "        \"comment\": \"no status yet\",\n"
	                                "        \"status\": \"Unknown\"\n"
	                                "    },\n"
	                                "    \"PPSA01234\": {\n"
	                                "        \"comment\": \"Runs \\\"well\\\"\",\n"
	                                "        \"status\": \"InGame\"\n"
	                                "    }\n"
	                                "}\n";
	const auto        text        = kyty::core::SerializeCompatibility(database);
	Check(text == expected, "the Qt format:\n" + text);
	Check(ParseOk(text) == database, "the serialized database parses back");
	Check(kyty::core::SerializeCompatibility({}) == "{}\n", "an empty database");

	CompatibilityDatabase ill_formed;
	ill_formed.entries["PPSA1"] = Entry(CompatibilityStatus::Logo, "a\xFF"
	                                                               "b");
	const auto replaced         = ParseOk(kyty::core::SerializeCompatibility(ill_formed));
	Check(Lookup(replaced, "PPSA1").comment == "a\xEF\xBF\xBD"
	                                           "b",
	      "an ill-formed comment is written with U+FFFD");
}

void TestModel() {
	CompatibilityModel model;
	Check(model.Database().entries.empty() &&
	          model.StatusFor("PPSA01234").status == CompatibilityStatus::Unknown,
	      "a new model shows Unknown");

	const auto first =
	    ParseOk(R"({"PPSA01234": {"status": "InGame"}, "PPSA2": {"status": "Logo"}})");
	model.Apply(first);
	Check(model.StatusFor(" ppsa01234 ").status == CompatibilityStatus::InGame &&
	          model.StatusFor("PPSA2").status == CompatibilityStatus::Logo &&
	          model.StatusFor("PPSA3").status == CompatibilityStatus::Unknown &&
	          model.StatusFor("").status == CompatibilityStatus::Unknown,
	      "applied statuses");

	CompatibilityLoadResult failed;
	failed.source       = CompatibilityLoadResult::Source::None;
	failed.attempts     = 4;
	failed.last_failure = "the downloaded content does not parse";
	Check(!model.ApplyLoadResult(failed) && model.Database() == first,
	      "a load without a database keeps the earlier statuses (8.6)");

	CompatibilityLoadResult cached;
	cached.source   = CompatibilityLoadResult::Source::Cache;
	cached.database = ParseOk(R"({"PPSA2": {"status": "DoesntBoot"}})");
	Check(model.ApplyLoadResult(cached) && model.Database() == *cached.database,
	      "a load with a database replaces the statuses");
	Check(model.StatusFor("PPSA01234").status == CompatibilityStatus::Unknown,
	      "an entry the new database lacks is Unknown");

	// --local edits.
	Check(model.SetStatus(" ppsa9 ", CompatibilityStatus::MainMenu), "set a new entry's status");
	Check(model.StatusFor("PPSA9") == Entry(CompatibilityStatus::MainMenu), "the new entry");
	Check(model.SetComment("PPSA9", "Menus work"), "set its comment");
	Check(model.StatusFor("ppsa9") == Entry(CompatibilityStatus::MainMenu, "Menus work"),
	      "status and comment");
	Check(model.SetComment("cusa5", "comment only"), "set a comment of a new entry");
	Check(model.StatusFor("CUSA5") == Entry(CompatibilityStatus::Unknown, "comment only"),
	      "a comment-only entry");
	Check(model.SetStatus("PPSA2", CompatibilityStatus::InGame) &&
	          model.StatusFor("PPSA2").status == CompatibilityStatus::InGame,
	      "change an existing status");
	const auto before = model.Database();
	Check(!model.SetStatus("", CompatibilityStatus::InGame) &&
	          !model.SetStatus(" \t", CompatibilityStatus::InGame) && !model.SetComment("", "x") &&
	          !model.SetComment("\xE3\x80\x80", "x") && model.Database() == before,
	      "an empty title ID changes nothing");
}

void TestLocalMode() {
	Check(std::string(kyty::core::kLocalCompatibilityFileName) == "compatibility_db.json",
	      "the --local file name");
	const TempDir dir("kyty_compat_local");
	const auto    path = dir.Path() / kyty::core::kLocalCompatibilityFileName;

	auto missing = kyty::core::LoadLocalCompatibility(path);
	Check(missing.has_value() && missing->entries.empty(), "a missing file is an empty database");

	// A file as the Qt launcher's Save() wrote it.
	dir.WriteFile(path.filename(), "{\n"
	                               "    \"PPSA01234\": {\n"
	                               "        \"comment\": \"Runs well\",\n"
	                               "        \"status\": \"InGame\"\n"
	                               "    }\n"
	                               "}\n");
	auto loaded = kyty::core::LoadLocalCompatibility(path);
	Check(loaded.has_value() &&
	          Lookup(*loaded, "PPSA01234") == Entry(CompatibilityStatus::InGame, "Runs well"),
	      "the Qt file loads");

	CompatibilityModel model;
	model.Apply(*loaded);
	Check(model.SetStatus("ppsa01234", CompatibilityStatus::Logo) &&
	          model.SetComment("PPSA00002", "new"),
	      "edits");
	Check(kyty::core::SaveLocalCompatibility(path, model.Database()).has_value(), "save");
	const std::string saved = ReadBytes(path);
	Check(saved == kyty::core::SerializeCompatibility(model.Database()),
	      "the file holds the serialized database");
	Check(Contains(saved, "\"status\": \"Logo\"") && Contains(saved, "\"PPSA00002\""),
	      "the edits are in the file");
	auto reloaded = kyty::core::LoadLocalCompatibility(path);
	Check(reloaded.has_value() && *reloaded == model.Database(), "the saved file loads equal");

	// A failure at each step of the atomic write leaves the file unchanged.
	CompatibilityModel edited = model;
	edited.SetStatus("PPSA01234", CompatibilityStatus::DoesntBoot);
	for (const auto step: {AtomicFileStep::CreateTemp, AtomicFileStep::Write, AtomicFileStep::Sync,
	                       AtomicFileStep::Close, AtomicFileStep::Rename}) {
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](AtomicFileStep current) { return current == step ? EIO : 0; });
		const auto result = kyty::core::SaveLocalCompatibility(path, edited.Database());
		kyty::core::SetAtomicFileFaultForTesting({});
		Check(!result.has_value() && result.error().error_number == EIO,
		      "a write fault is an error");
		Check(ReadBytes(path) == saved, "a write fault leaves the file byte-identical");
		std::error_code error;
		size_t          files = 0;
		for (const auto& entry: fs::directory_iterator(dir.Path(), error)) {
			(void)entry;
			files++;
		}
		Check(!error && files == 1, "no temporary file is left behind");
	}

	// Unusable files are errors naming the path or the reason.
	dir.WriteFile("invalid.json", "{\"PPSA1\": ");
	using kyty::core::CompatibilityFileErrorKind;
	auto invalid = kyty::core::LoadLocalCompatibility(dir.Path() / "invalid.json");
	Check(!invalid && Contains(invalid.error().message, "invalid.json") &&
	          Contains(invalid.error().message, "does not parse") &&
	          Contains(invalid.error().message, "parse error"),
	      "invalid JSON: " + (invalid ? std::string() : invalid.error().message));
	// Phase 9 V1: the kind and the parts of each message.
	Check(invalid.error().kind == CompatibilityFileErrorKind::InvalidJson &&
	          invalid.error().path == dir.Path() / "invalid.json" &&
	          invalid.error().message ==
	              "The compatibility database " + (dir.Path() / "invalid.json").string() +
	                  " does not parse: the content is not valid JSON: " + invalid.error().detail &&
	          invalid.error().detail.starts_with("parse error"),
	      "invalid JSON's kind, path, and parser text");
	dir.WriteFile("array.json", "[]");
	auto array = kyty::core::LoadLocalCompatibility(dir.Path() / "array.json");
	Check(!array && Contains(array.error().message, "not an object") &&
	          array.error().kind == CompatibilityFileErrorKind::NotAnObject,
	      "a non-object root");
	dir.MakeDirectories("directory.json");
	auto directory = kyty::core::LoadLocalCompatibility(dir.Path() / "directory.json");
	Check(!directory && Contains(directory.error().message, "Cannot read") &&
	          Contains(directory.error().message, "directory.json"),
	      "a directory: " + (directory ? std::string() : directory.error().message));
	Check(directory.error().kind == CompatibilityFileErrorKind::Unreadable &&
	          directory.error().io.error_number == EISDIR &&
	          directory.error().message ==
	              "Cannot read the compatibility database: " + directory.error().io.message,
	      "a directory: the read error is kept");
	dir.WriteFile("large.json", std::string((16u << 20u) + 1u, ' '));
	auto large = kyty::core::LoadLocalCompatibility(dir.Path() / "large.json");
	Check(!large && Contains(large.error().message, "Cannot read") &&
	          large.error().io.kind == kyty::core::IoErrorKind::TooLarge &&
	          large.error().io.limit == (16u << 20u),
	      "a file over 16 MiB");
	Check(!kyty::core::SaveLocalCompatibility(dir.Path() / "missing" / "db.json", {}).has_value(),
	      "saving into a missing directory fails");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CompatibilityDatabaseTests");
	TestStatusAliases();
	TestParse();
	TestParseErrors();
	TestLookup();
	TestSerialize();
	TestModel();
	TestLocalMode();
	std::printf("CompatibilityDatabaseTests: all passed\n");
	return 0;
}
