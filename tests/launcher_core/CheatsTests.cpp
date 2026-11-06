// Example tests of the cheat file rules (src/launcher/core/cheats.h, task 16.2; Requirements
// 11.4-11.7): which title IDs have cheats, the path of their cheat file below the Data_Directory,
// parsing and serializing cheat documents (unknown members kept, enabled states written back),
// the remote import checks ported from Qt's Cheats::ValidateImport, the confirmation and
// changed-on-disk rules of SaveIfUnchanged with injected write faults, the import, and the Local
// view's loading of a real cheat file.

#include "launcher/core/atomicFile.h"
#include "launcher/core/cheats.h"
#include "launcher/core/dataPaths.h"
#include "support/contentGenerators.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace ContentGen = KytyTests::ContentGen;

using ContentGen::JsonString;
using kyty::core::CheatDocument;
using kyty::core::CheatFilePath;
using kyty::core::CheatSaveErrorKind;
using kyty::core::IsCheatTitleId;
using kyty::core::ParseCheatDocument;
using kyty::core::SerializeCheatDocument;
using kyty::core::ValidateRemoteImport;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "CheatsTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

CheatDocument Parse(std::string_view bytes, const std::string& what) {
	auto document = ParseCheatDocument(bytes, "test.json");
	Check(document.has_value(),
	      what + ": parses (" + (document.has_value() ? std::string() : document.error()) + ")");
	return *document;
}

std::string ReadBytes(const std::filesystem::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, kyty::core::kMaxCheatFileBytes);
	Check(bytes.has_value(), "read " + path.string());
	return *bytes;
}

// The remote document of the design's example, which ValidateRemoteImport accepts for
// PPSA01234 01.000 eboot.bin.
std::string ValidRemote() {
	return ContentGen::CheatDocumentText(
	    ContentGen::RemoteCheatSpec("PPSA01234", "01.000", "eboot.bin", {"Infinite HP", "Speed"}));
}

void TestCheatTitleIds() {
	for (const std::string_view title:
	     {"PPSA01234", "PPSA00000", "PPSA99999", "ppsa01234", "pPsA01234", "  PPSA01234\t",
	      "\nppsa01234 ", "\xE3\x80\x80PPSA01234\xC2\xA0"}) {
		Check(IsCheatTitleId(title), "'" + std::string(title) + "' has cheats");
	}
	for (const std::string_view title:
	     {"", "   ", "PPSA", "PPSA0123", "PPSA012345", "CUSA01234", "PPSB01234", "PPS01234",
	      "PPSA0123a", "PPSA01 234", "PPSA-1234", "PPSA+1234", "PPSA01234_00", "XPPSA01234",
	      // A fullwidth digit and a fullwidth letter are not ASCII.
	      "PPSA0123\xEF\xBC\x94", "\xEF\xBC\xB0PSA01234"}) {
		Check(!IsCheatTitleId(title), "'" + std::string(title) + "' has no cheats");
	}
	Check(!IsCheatTitleId(std::string_view("PPSA0123\0", 9)), "an embedded NUL is not a digit");
}

void TestCheatFilePath() {
	const std::filesystem::path data_dir = "/opt/kyty";
	const auto                  path     = CheatFilePath(data_dir, "  ppsa01234 ");
	Check(path.native() == "/opt/kyty/_Patches/PPSA01234.json",
	      "the cheat file is <data_dir>/_Patches/<TITLE_ID>.json: " + path.string());
	Check(path == kyty::core::CheatDirectory(data_dir) / "PPSA01234.json",
	      "the cheat file is in CheatDirectory()");
	Check(CheatFilePath("/opt/kyty/", "PPSA54321").native() == "/opt/kyty/_Patches/PPSA54321.json",
	      "a trailing separator on the Data_Directory");
	Check(CheatFilePath("data", "PPSA54321").native() == "data/_Patches/PPSA54321.json",
	      "a relative Data_Directory stays relative");
	for (const std::string_view title:
	     {"", "CUSA01234", "PPSA0123", "../PPSA01234", "PPSA01234/"}) {
		Check(CheatFilePath(data_dir, title).empty(),
		      "no cheat file for '" + std::string(title) + "'");
	}
}

void TestProcessName() {
	Check(kyty::core::CheatProcessName("") == "eboot.bin", "an empty elf is eboot.bin");
	Check(kyty::core::CheatProcessName("eboot.bin") == "eboot.bin", "eboot.bin");
	Check(kyty::core::CheatProcessName("bin/game.elf") == "game.elf", "the file name of the elf");
	Check(kyty::core::CheatProcessName("dir/").empty(), "a directory has no file name (as Qt)");
}

void TestParse() {
	const auto document = Parse(
	    R"({"id":"PPSA01234","name":"Game","version":"01.000","process":"eboot.bin",)"
	    R"("author":"Someone","credits":["A",1,"B"],"x-unknown":{"k":[1,2]},)"
	    R"("mods":[{"name":"Infinite HP","enabled":false,"memory":[{"offset":"0x10","on":"90","off":"0F"}]},)"
	    R"({"name":"Speed","note":"kept"},{"name":"On","enabled":true}],"trailer":null})",
	    "the design's example");
	Check(document.cheats.size() == 3, "three cheats");
	Check(document.cheats[0].name == "Infinite HP" && !document.cheats[0].enabled,
	      "a disabled cheat");
	Check(document.cheats[0].memory == R"([{"offset":"0x10","on":"90","off":"0F"}])",
	      "memory as compact JSON: " + document.cheats[0].memory);
	Check(document.cheats[1].enabled && document.cheats[1].memory.empty(),
	      "a cheat without enabled is enabled, and without memory has none");
	Check(document.cheats[2].enabled, "enabled true");
	Check(document.id == "PPSA01234" && document.name == "Game" && document.version == "01.000" &&
	          document.process == "eboot.bin" && document.author == "Someone",
	      "metadata");
	Check(document.credits == std::vector<std::string> {"A", "B"}, "string credits only");
	Check(document.json.find("\"x-unknown\":{\"k\":[1,2]}") != std::string::npos &&
	          document.json.find("\"trailer\":null") != std::string::npos,
	      "unknown members are kept: " + document.json);

	const auto minimal = Parse(R"({"mods":[]})", "only mods");
	Check(minimal.cheats.empty() && minimal.id.empty() && minimal.credits.empty(), "no cheats");
	const auto odd_metadata = Parse(R"({"id":5,"credits":"x","mods":[{"name":" "}]})", "odd");
	Check(odd_metadata.id.empty() && odd_metadata.credits.empty(), "non-string metadata is empty");
	Check(odd_metadata.cheats.size() == 1 && odd_metadata.cheats[0].name == " ",
	      "a blank name is not empty (as Qt)");
	const auto repeated =
	    Parse(R"({"mods":[{"name":"a"}],"mods":[{"name":"b"},{"name":"c"}]})", "a repeated key");
	Check(repeated.cheats.size() == 2 && repeated.cheats[0].name == "b",
	      "a repeated key has its last value");
	const auto unicode =
	    Parse("{\"mods\":[{\"name\":\"\xE2\x9C\x93 \\u00e9\"}]}", "non-ASCII names");
	Check(unicode.cheats[0].name == "\xE2\x9C\x93 \xC3\xA9", "names are UTF-8");
}

void TestParseErrors() {
	struct Case {
		std::string_view bytes;
		std::string_view reason;
	};
	for (const auto& [bytes, reason]: std::vector<Case> {
	         {"", "Invalid cheat JSON ("},
	         {"{", "Invalid cheat JSON ("},
	         {"{\"mods\":[]} x", "Invalid cheat JSON ("},
	         {"{\"mods\":[{\"name\":\"\xFF\"}]}", "Invalid cheat JSON ("},
	         {"[]", "Invalid cheat JSON (the top-level value is not an object)."},
	         {"\"text\"", "Invalid cheat JSON (the top-level value is not an object)."},
	         {"{}", "Cheat JSON has no \"mods\" array."},
	         {"{\"mods\":{}}", "Cheat JSON has no \"mods\" array."},
	         {"{\"mods\":null}", "Cheat JSON has no \"mods\" array."},
	         {"{\"mods\":[1]}", "Invalid cheat entry 1 (it is not an object)."},
	         {"{\"mods\":[{\"name\":\"a\"},[]]}", "Invalid cheat entry 2 (it is not an object)."},
	         {"{\"mods\":[{}]}", "Invalid cheat entry 1 (it has no name)."},
	         {"{\"mods\":[{\"name\":\"\"}]}", "Invalid cheat entry 1 (it has no name)."},
	         {"{\"mods\":[{\"name\":7}]}", "Invalid cheat entry 1 (it has no name)."},
	         {"{\"mods\":[{\"name\":\"a\",\"enabled\":\"true\"}]}",
	          "Invalid cheat entry 1 (\"enabled\" is not true or false)."},
	         {"{\"mods\":[{\"name\":\"a\",\"enabled\":null}]}",
	          "Invalid cheat entry 1 (\"enabled\" is not true or false)."},
	         {"{\"mods\":[{\"name\":\"a\",\"enabled\":1}]}",
	          "Invalid cheat entry 1 (\"enabled\" is not true or false)."},
	     }) {
		const auto document = ParseCheatDocument(bytes, "/data/_Patches/PPSA01234.json");
		Check(!document.has_value(), "'" + std::string(bytes) + "' is refused");
		const auto& error = document.error();
		Check(error.starts_with("/data/_Patches/PPSA01234.json: " + std::string(reason)),
		      "the error names the file and the reason: " + error);
	}
	const auto not_json = ParseCheatDocument("{", "f.json");
	Check(not_json.error().find("parse error") != std::string::npos,
	      "the parser's message is carried: " + not_json.error());
	// Phase 8 plan D9: the parser's reason without nlohmann's "[json.exception...]" prefix.
	const auto not_text = ParseCheatDocument("this is not JSON at all", "f.json");
	Check(not_text.error().starts_with("f.json: Invalid cheat JSON (parse error at line 1, ") &&
	          not_text.error().find("[json.exception") == std::string::npos,
	      "the reason has no exception prefix: " + not_text.error());
}

void TestSerialize() {
	const std::string text =
	    R"({"z":1,"id":"PPSA01234","mods":[{"name":"a","enabled":true,"x":[1]},)"
	    R"({"memory":{"k":"v"},"name":"b"},{"name":"c","enabled":false}],"a":"\u00e9"})";
	auto       document  = Parse(text, "serialize input");
	const auto unchanged = SerializeCheatDocument(document);
	Check(unchanged.ends_with("}\n") && unchanged.find("\n    \"z\": 1,") != std::string::npos,
	      "indented by 4 spaces with a final line break:\n" + unchanged);
	Check(unchanged.find("\"enabled\"") ==
	              unchanged.rfind("\"enabled\"", unchanged.find("\"b\"")) &&
	          unchanged.find("\"name\": \"b\"\n") != std::string::npos,
	      "an enabled cheat without \"enabled\" stays without it:\n" + unchanged);
	Check(unchanged.find("\"a\": \"\xC3\xA9\"") != std::string::npos, "UTF-8 is written as is");
	Check(Parse(unchanged, "reparse") == document, "an unchanged document round-trips exactly");
	Check(unchanged.find("\"z\"") < unchanged.find("\"id\"") &&
	          unchanged.find("\"mods\"") < unchanged.find("\"a\": "),
	      "member order is kept");

	document.cheats[0].enabled = false;
	document.cheats[1].enabled = false;
	document.cheats[2].enabled = true;
	const auto changed         = Parse(SerializeCheatDocument(document), "changed");
	Check(changed.cheats.size() == 3 && !changed.cheats[0].enabled && !changed.cheats[1].enabled &&
	          changed.cheats[2].enabled,
	      "enabled states are written back");
	Check(changed.cheats[1].memory == R"({"k":"v"})" && changed.cheats[0].name == "a",
	      "names and memory are kept");
	Check(changed.json.find(R"({"memory":{"k":"v"},"name":"b","enabled":false})") !=
	          std::string::npos,
	      "a new \"enabled\" is appended to its mod: " + changed.json);
	Check(changed.json.find(R"("x":[1])") != std::string::npos, "unknown mod members are kept");

	// Fewer cheats than mods: only the listed ones are written.
	auto shorter = document; // json as read: a enabled, b without "enabled", c disabled
	shorter.cheats.resize(1);
	shorter.cheats[0].enabled = false;
	const auto partial        = Parse(SerializeCheatDocument(shorter), "fewer cheats");
	Check(!partial.cheats[0].enabled && partial.cheats[1].enabled && !partial.cheats[2].enabled,
	      "only the first is changed; the others keep the stored states");

	Check(SerializeCheatDocument(CheatDocument {}).empty(), "a default document has no text");
	CheatDocument not_object;
	not_object.json = "[1]";
	Check(SerializeCheatDocument(not_object).empty(), "a non-object document has no text");
	CheatDocument no_mods;
	no_mods.json = R"({"k":1})";
	Check(SerializeCheatDocument(no_mods) == "{\n    \"k\": 1\n}\n", "no mods: written as it is");

	const auto disabled = kyty::core::WithAllDisabled(document);
	for (const auto& cheat: disabled.cheats) {
		Check(!cheat.enabled, "WithAllDisabled disables " + cheat.name);
	}
	Check(disabled.json == document.json && disabled.cheats.size() == document.cheats.size(),
	      "WithAllDisabled changes only the enabled states");
}

void TestValidateRemoteImport() {
	const auto valid = Parse(ValidRemote(), "valid remote");
	Check(!ValidateRemoteImport(valid, "PPSA01234", "01.000", "eboot.bin"), "a matching file");
	Check(!ValidateRemoteImport(valid, " ppsa01234 ", "01.000", "EBOOT.BIN"),
	      "title ID and process ignore letter case and the title ID is trimmed");

	const auto mismatch = [&](std::string_view title, std::string_view version,
	                          std::string_view process, std::string_view expected) {
		const auto error = ValidateRemoteImport(valid, title, version, process);
		Check(error && *error == expected,
		      "refused with '" + std::string(expected) + "': " + error.value_or("(accepted)"));
	};
	mismatch("PPSA01235", "01.000", "eboot.bin",
	         "The cheat file does not match this game's title ID.");
	mismatch("CUSA01234", "01.000", "eboot.bin",
	         "The cheat file does not match this game's title ID.");
	mismatch("", "01.000", "eboot.bin", "The cheat file does not match this game's title ID.");
	mismatch("PPSA01234", "01.001", "eboot.bin",
	         "The cheat file does not match this game's version (01.001).");
	mismatch("PPSA01234", "", "eboot.bin", "The cheat file does not match this game's version ().");
	mismatch("PPSA01234", " 01.000", "eboot.bin",
	         "The cheat file does not match this game's version ( 01.000).");
	mismatch("PPSA01234", "01.000", "game.elf",
	         "The cheat file does not match this game's executable (game.elf).");
	mismatch("PPSA01234", "01.000", "", "The cheat file does not match this game's executable ().");

	const auto check = [](const std::string& text, std::string_view expected) {
		auto document = ParseCheatDocument(text, "remote.json");
		Check(document.has_value(), "parses: " + text);
		const auto error = ValidateRemoteImport(*document, "PPSA01234", "01.000", "eboot.bin");
		Check(error && *error == expected, "'" + text + "' refused with '" + std::string(expected) +
		                                       "': " + error.value_or("(accepted)"));
	};
	const auto with_mod = [](const std::string& mod, std::string_view extra = "") {
		return R"({"id":"ppsa01234","version":"01.000","process":"eboot.bin",)" +
		       std::string(extra) + R"("mods":[)" + mod + "]}";
	};
	const std::string memory_unsupported = "Unsupported memory entries in cheat \"m\".";
	const std::string addressing =
	    "This JSON uses memory addressing features that are not supported yet.";
	check(with_mod(""), "The cheat file contains no cheats.");
	check(with_mod(R"({"name":"m","memory":[{"offset":"1","on":"90","off":"0F"}]})",
	               R"("master":{},)"),
	      "This JSON uses master codes, which are not supported yet.");
	check(with_mod(
	          R"({"name":"m","module_name":"x","memory":[{"offset":"1","on":"90","off":"0F"}]})"),
	      "This JSON uses module-specific cheats, which are not supported yet.");
	check(with_mod(R"({"name":"m"})"), memory_unsupported);
	check(with_mod(R"({"name":"m","memory":[]})"), memory_unsupported);
	check(with_mod(R"({"name":"m","memory":{}})"), memory_unsupported);
	check(with_mod(R"({"name":"m","memory":[1]})"), memory_unsupported);
	for (const std::string_view write: {
	         R"({"on":"90","off":"0F"})",
	         R"({"offset":"","on":"90","off":"0F"})",
	         R"({"offset":"0x","on":"90","off":"0F"})",
	         R"({"offset":"0x12345678901234567","on":"90","off":"0F"})",
	         R"({"offset":"12345678901234567","on":"90","off":"0F"})",
	         R"({"offset":"0xG1","on":"90","off":"0F"})",
	         R"({"offset":12,"on":"90","off":"0F"})",
	         R"({"offset":"1","on":"9","off":"0"})",
	         R"({"offset":"1","on":"","off":""})",
	         R"({"offset":"1","on":"9090","off":"0F"})",
	         R"({"offset":"1","on":"zz","off":"0F"})",
	         R"({"offset":"1","on":"90","off":"0g"})",
	         R"({"offset":"1","on":90,"off":"0F"})",
	     }) {
		check(with_mod(R"({"name":"m","memory":[)" + std::string(write) + "]}"),
		      memory_unsupported);
	}
	for (const std::string_view write: {
	         R"({"offset":"1","on":"90","off":"0F","absolute":true})",
	         R"({"offset":"1","on":"90","off":"0F","absolute":null})",
	         R"({"offset":"1","on":"90","off":"0F","absolute":"false"})",
	         R"({"offset":"1","on":"90","off":"0F","expected":"90"})",
	         R"({"offset":"1","on":"90","off":"0F","expected":15})",
	         R"({"offset":"1","on":"90","off":"0F","section":0})",
	         R"({"offset":"1","on":"90","off":"0F","sectionName":".text"})",
	         R"({"offset":"1","on":"90","off":"0F","sectionProtection":"rx"})",
	         R"({"offset":"1","on":"90","off":"0F","logicalOffset":"1"})",
	         R"({"offset":"1","on":"90","off":"0F","sectionOffset":"1"})",
	     }) {
		check(with_mod(R"({"name":"m","memory":[)" + std::string(write) + "]}"), addressing);
	}
	// Accepted variants: 0X prefix, 16 digits, "absolute": false, "expected" equal to "off".
	for (const std::string_view write: {
	         R"({"offset":"0X1234567890abcdef","on":"90AB","off":"0f85"})",
	         R"({"offset":"FFFF","on":"90","off":"0F","absolute":false})",
	         R"({"offset":"0","on":"90","off":"0F","expected":"0f"})",
	     }) {
		auto document = ParseCheatDocument(
		    with_mod(R"({"name":"m","memory":[)" + std::string(write) + "]}"), "remote.json");
		Check(!ValidateRemoteImport(*document, "PPSA01234", "01.000", "eboot.bin"),
		      "accepted: " + std::string(write));
	}

	Check(ValidateRemoteImport(CheatDocument {}, "PPSA01234", "01.000", "eboot.bin") ==
	          std::optional<std::string>("Invalid cheat JSON."),
	      "a document without JSON");
	CheatDocument bad;
	bad.json = R"({"mods":[{"name":""}]})";
	Check(ValidateRemoteImport(bad, "PPSA01234", "01.000", "eboot.bin") ==
	          std::optional<std::string>("Invalid cheat entry 1 (it has no name)."),
	      "an invalid document is checked again");
	// SDL_strcasecmp stops at a NUL byte; such an id must not match the title ID.
	check(R"({"id":"PPSA01234\u0000x","version":"01.000","process":"eboot.bin","mods":[)"
	      R"({"name":"m","memory":[{"offset":"1","on":"90","off":"0F"}]}]})",
	      "The cheat file does not match this game's title ID.");
}

void TestSaveIfUnchanged() {
	const KytyTests::TempDir temp("kyty_cheats");
	const auto               path     = temp.Path() / "data" / "_Patches" / "PPSA01234.json";
	const auto               document = Parse(ValidRemote(), "document");
	const auto               text     = SerializeCheatDocument(document);
	using kyty::core::SaveIfUnchanged;

	// No file, nothing expected: written, creating the directory.
	Check(SaveIfUnchanged(path, document, std::nullopt).has_value(), "a new file is written");
	Check(ReadBytes(path) == text, "the file holds SerializeCheatDocument");

	// The file exists: nullopt needs confirmation; the right bytes replace it.
	auto other = kyty::core::WithAllDisabled(document);
	auto saved = SaveIfUnchanged(path, other, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::NeedsConfirmation &&
	          saved.error().message.find(path.string()) != std::string::npos,
	      "an existing file needs confirmation: " + saved.error().message);
	Check(ReadBytes(path) == text, "unchanged before the confirmation");
	saved = SaveIfUnchanged(path, other, std::string_view("something else"));
	Check(!saved && saved.error().kind == CheatSaveErrorKind::ChangedOnDisk &&
	          saved.error().message == "Cheat file changed; reload the dialog.",
	      "other expected bytes: changed on disk");
	Check(ReadBytes(path) == text, "unchanged after a refusal");
	Check(SaveIfUnchanged(path, other, text).has_value(), "the expected bytes: replaced");
	Check(ReadBytes(path) == SerializeCheatDocument(other), "replaced");

	// Expected bytes but no file: changed on disk; an empty file is a file.
	const auto missing = temp.Path() / "data" / "_Patches" / "PPSA05678.json";
	saved              = SaveIfUnchanged(missing, document, std::string_view(""));
	Check(!saved && saved.error().kind == CheatSaveErrorKind::ChangedOnDisk &&
	          !std::filesystem::exists(missing),
	      "a missing file with expected bytes");
	temp.WriteFile("data/_Patches/PPSA05678.json", "");
	Check(SaveIfUnchanged(missing, document, std::string_view("")).has_value(),
	      "an empty file with empty expected bytes");

	// Failures.
	saved = SaveIfUnchanged({}, document, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed &&
	          saved.error().message == "Invalid cheat file.",
	      "an empty path");
	saved = SaveIfUnchanged(temp.Path() / "x.json", CheatDocument {}, std::nullopt);
	Check(!saved && saved.error().message == "Invalid cheat file." &&
	          !std::filesystem::exists(temp.Path() / "x.json"),
	      "a document without JSON");
	temp.MakeDirectories("dir.json");
	saved = SaveIfUnchanged(temp.Path() / "dir.json", document, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed &&
	          saved.error().message.starts_with("Could not read cheat file: "),
	      "a directory at the path: " + saved.error().message);
	temp.WriteFile("file", "not a directory");
	saved =
	    SaveIfUnchanged(temp.Path() / "file" / "sub" / "PPSA01234.json", document, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed,
	      "a parent that is a file: " + saved.error().message);
	temp.WriteFile("big.json", std::string(kyty::core::kMaxCheatFileBytes + 1, ' '));
	saved = SaveIfUnchanged(temp.Path() / "big.json", document, std::string_view(""));
	Check(!saved && saved.error().message.starts_with("Could not read cheat file: "),
	      "a file over the limit cannot be compared");
	temp.WriteFile("blocked", "file");
	saved = SaveIfUnchanged(temp.Path() / "blocked" / "PPSA01234.json", document, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed,
	      "ENOTDIR on the read: " + saved.error().message);

	// The directory cannot be created: _Patches is a dangling symbolic link, so the file reads as
	// missing but create_directories fails.
	const KytyTests::TempDir blocked("kyty_cheats_dir");
	blocked.MakeSymlink("/nonexistent/kyty-cheats-target", "_Patches");
	saved = SaveIfUnchanged(blocked.Path() / "_Patches" / "PPSA01234.json", document, std::nullopt);
	Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed &&
	          saved.error().message.starts_with("Could not create the local cheats directory " +
	                                            (blocked.Path() / "_Patches").string() + ": "),
	      "the directory cannot be created: " + saved.error().message);

	// Injected write faults leave the file unchanged.
	for (const auto step: {kyty::core::AtomicFileStep::CreateTemp,
	                       kyty::core::AtomicFileStep::Write, kyty::core::AtomicFileStep::Rename}) {
		const auto before = ReadBytes(path);
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](kyty::core::AtomicFileStep current) { return current == step ? EIO : 0; });
		saved = SaveIfUnchanged(path, document, before);
		kyty::core::SetAtomicFileFaultForTesting({});
		Check(!saved && saved.error().kind == CheatSaveErrorKind::Failed &&
		          saved.error().message.starts_with("Could not save cheat file: "),
		      "a write fault: " + (saved ? std::string() : saved.error().message));
		Check(ReadBytes(path) == before, "a write fault leaves the file unchanged");
	}
}

void TestImport() {
	const KytyTests::TempDir temp("kyty_cheats_import");
	const auto               data = temp.Path() / "data";
	const auto               spec =
	    ContentGen::RemoteCheatSpec("ppsa01234", "01.000", "eboot.bin", {"One", "Two"});
	auto remote              = Parse(ContentGen::CheatDocumentText(spec), "remote");
	remote.cheats[1].enabled = true;
	const auto imported = kyty::core::ImportRemoteCheats(data, " ppsa01234 ", remote, std::nullopt);
	Check(imported.has_value() && *imported == data / "_Patches" / "PPSA01234.json",
	      "imported to _Patches/<UPPERCASE>.json");
	const auto local = kyty::core::LoadLocalCheats(data, "PPSA01234");
	Check(local.document.has_value() && local.document->cheats.size() == 2, "two cheats");
	for (const auto& cheat: local.document->cheats) {
		Check(!cheat.enabled, "imported cheats are disabled: " + cheat.name);
	}
	const auto expected =
	    Parse(ContentGen::CheatDocumentText(ContentGen::AllDisabledSpec(spec)), "expected");
	Check(local.document->json == expected.json,
	      "every other member is preserved: " + local.document->json);

	// A second import needs confirmation, then replaces the file with the read bytes.
	auto again = kyty::core::ImportRemoteCheats(data, "PPSA01234", remote, std::nullopt);
	Check(!again && again.error().kind == CheatSaveErrorKind::NeedsConfirmation,
	      "an existing local file needs confirmation");
	again = kyty::core::ImportRemoteCheats(data, "PPSA01234", remote, *local.bytes);
	Check(again.has_value(), "confirmed with the bytes LoadLocalCheats read");

	const auto unsupported =
	    kyty::core::ImportRemoteCheats(data, "CUSA01234", remote, std::nullopt);
	Check(!unsupported && unsupported.error().message == "Unsupported title ID.",
	      "no import for a title without cheats");
}

void TestLoadLocalCheats() {
	const KytyTests::TempDir temp("kyty_cheats_local");
	const auto               data = temp.Path();

	auto local = kyty::core::LoadLocalCheats(data, "CUSA01234");
	Check(local.path.empty() && !local.readable && local.message == "Unsupported title ID.",
	      "a title without cheats");

	local           = kyty::core::LoadLocalCheats(data, "ppsa01234");
	const auto path = data / "_Patches" / "PPSA01234.json";
	Check(local.path == path && local.readable && !local.bytes && !local.document &&
	          local.message == "No local cheat file: " + path.string(),
	      "no file: " + local.message);

	const std::string invalid = "{\"mods\":[{\"name\":\"\"}]}";
	temp.WriteFile("_Patches/PPSA01234.json", invalid);
	local = kyty::core::LoadLocalCheats(data, "PPSA01234");
	Check(local.readable && local.bytes == invalid && !local.document &&
	          local.message == path.string() + ": Invalid cheat entry 1 (it has no name).",
	      "a file that does not parse: " + local.message);
	Check(ReadBytes(path) == invalid, "the file is left unchanged");

	temp.WriteFile("_Patches/PPSA01234.json", ValidRemote());
	local = kyty::core::LoadLocalCheats(data, "PPSA01234");
	Check(local.document && local.document->cheats.size() == 2 &&
	          local.message == "Loaded 2 cheat(s) from " + path.string() + ".",
	      "a valid file: " + local.message);

	std::filesystem::remove(path);
	temp.MakeDirectories("_Patches/PPSA01234.json");
	local = kyty::core::LoadLocalCheats(data, "PPSA01234");
	Check(!local.readable && !local.bytes && !local.document &&
	          local.message.starts_with("Could not read cheat file: "),
	      "an unreadable file: " + local.message);
	// p7-u7-1: the reason alone, for the launcher's own sentence with the path.
	Check(local.reason == std::generic_category().message(EISDIR),
	      "an unreadable file's reason is the system's text alone: " + local.reason);
	Check(local.message.find(local.reason) != std::string::npos,
	      "the message carries the same reason: " + local.message);

	std::filesystem::remove(path);
	temp.WriteFile("_Patches/PPSA01234.json", std::string(kyty::core::kMaxCheatFileBytes + 1, ' '));
	local = kyty::core::LoadLocalCheats(data, "PPSA01234");
	Check(!local.readable && !local.bytes && local.reason == std::generic_category().message(EFBIG),
	      "a file over the limit: " + local.reason);
	std::filesystem::remove(path);
	for (const char* title: {"PPSA01234", "CUSA01234"}) {
		Check(kyty::core::LoadLocalCheats(data, title).reason.empty(),
		      std::string("no reason without a read error: ") + title);
	}
	temp.WriteFile("_Patches/PPSA01234.json", ValidRemote());
	Check(kyty::core::LoadLocalCheats(data, "PPSA01234").reason.empty(),
	      "no reason for a file that was read");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("CheatsTests");
	TestCheatTitleIds();
	TestCheatFilePath();
	TestProcessName();
	TestParse();
	TestParseErrors();
	TestSerialize();
	TestValidateRemoteImport();
	TestSaveIfUnchanged();
	TestImport();
	TestLoadLocalCheats();
	std::printf("CheatsTests: all tests passed\n");
	return 0;
}
