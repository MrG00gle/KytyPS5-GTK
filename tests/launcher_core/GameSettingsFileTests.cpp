// Example tests of Game_Settings_File export and import (src/launcher/core/gameSettingsFile.h,
// task 13.4; Requirements 9.8-9.11, 9.15): tests/fixtures/settings/qt6-game-configs.json imports
// all three members unchanged; export then parse; every Requirement 9.10 rejection (unreadable,
// invalid JSON, non-object root, no usable member, empty member, unknown field, wrong JSON type per
// kind, out of range, each Requirement 9.4 value, a member or field key written twice), each naming
// the field or the reason; the one-member confirmation flag; partial import marks the game custom;
// imports into a Qt-loaded game whose stored mapping entries have no '=' (plan D26); an export
// whose write fails leaves the old file (injected faults at each AtomicFileStep).

#include "launcher/core/atomicFile.h"
#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/settingsStore.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::AtomicFileStep;
using kyty::core::ExportGameSettings;
using kyty::core::FieldValue;
using kyty::core::GameConfiguration;
using kyty::core::GameConfigurationPatch;
using kyty::core::GameEntry;
using kyty::core::ImportErrorKind;
using kyty::core::ImportResult;
using kyty::core::LauncherSettings;
using kyty::core::LogDirection;
using kyty::core::ParseGameSettingsFile;
using kyty::core::PresentMode;
using kyty::core::ScreenResolution;
using kyty::core::ShaderOptimizationType;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GameSettingsFileTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ReadBytes(const fs::path& path) {
	auto bytes = kyty::core::ReadFileLimited(path, 1u << 20u);
	Check(bytes.has_value(), "cannot read " + path.string());
	return *bytes;
}

const char* KindName(ImportErrorKind kind) {
	switch (kind) {
		case ImportErrorKind::Unreadable: return "Unreadable";
		case ImportErrorKind::InvalidJson: return "InvalidJson";
		case ImportErrorKind::NotAnObject: return "NotAnObject";
		case ImportErrorKind::NoSettings: return "NoSettings";
		case ImportErrorKind::InvalidTitle: return "InvalidTitle";
		case ImportErrorKind::DuplicateTitle: return "DuplicateTitle";
		case ImportErrorKind::MemberNotAnObject: return "MemberNotAnObject";
		case ImportErrorKind::EmptyMember: return "EmptyMember";
		case ImportErrorKind::DuplicateField: return "DuplicateField";
		case ImportErrorKind::UnknownField: return "UnknownField";
		case ImportErrorKind::WrongType: return "WrongType";
		case ImportErrorKind::InvalidValue: return "InvalidValue";
		case ImportErrorKind::NoMatchingMember: return "NoMatchingMember";
	}
	return "?";
}

ImportResult Parsed(std::string_view bytes, const std::string& what) {
	auto result = ParseGameSettingsFile(bytes);
	Check(result.has_value(), what + " parses, got " + (result ? "" : result.error().message));
	return std::move(*result);
}

// The file is rejected with kind; the message names field (when given) and the title (when given).
void CheckRejected(std::string_view bytes, ImportErrorKind kind, const std::string& field,
                   const std::string& title, const std::string& what) {
	const auto result = ParseGameSettingsFile(bytes);
	Check(!result.has_value(), what + " is rejected");
	const auto& error = result.error();
	Check(error.kind == kind, what + ": kind " + KindName(error.kind) + ", expected " +
	                              KindName(kind) + " (" + error.message + ")");
	Check(error.field == field, what + ": field '" + error.field + "', expected '" + field + "'");
	Check(error.title == title, what + ": title '" + error.title + "', expected '" + title + "'");
	Check(!error.message.empty(), what + ": a message");
	if (!field.empty()) {
		Check(error.message.find("\"" + field + "\"") != std::string::npos,
		      what + ": the message names the field: " + error.message);
	}
	if (!title.empty()) {
		Check(error.message.find(title) != std::string::npos,
		      what + ": the message names the title: " + error.message);
	}
}

std::string Member(const std::string& fields, const std::string& title = "PPSA01234") {
	return "{\"" + title + "\": {" + fields + "}}";
}

GameEntry Entry(const std::string& path, const std::string& title_id) {
	GameEntry entry;
	entry.path       = path;
	entry.title_id   = title_id;
	entry.title_name = "Game " + title_id;
	return entry;
}

GameConfiguration Sample() {
	GameConfiguration c;
	c.screen_resolution           = ScreenResolution::R2560X1440;
	c.user_name                   = "@Kyty, \"=;\\\xC3\xA9";
	c.user_id                     = 256;
	c.audio_input_device          = "Mic \"1\"";
	c.present_mode                = PresentMode::Fifo;
	c.gpu_index                   = 2;
	c.hdr_enabled                 = true;
	c.trophy_enabled              = false;
	c.trophy_duration_seconds     = 5;
	c.vblank_frequency            = 30;
	c.console_language            = 0;
	c.shader_optimization_type    = ShaderOptimizationType::Size;
	c.shader_log_direction        = LogDirection::File;
	c.shader_log_folder           = "logs\\shaders;v=1";
	c.command_buffer_dump_enabled = true;
	c.command_buffer_dump_folder  = "dumps, \"cb\"";
	c.printf_direction            = LogDirection::Console;
	c.printf_output_file          = "";
	c.elf                         = "bin/eboot_alt.bin";
	c.input_mapping_custom        = true;
	c.host_input_mapping          = {"Cross=J", "L3=Left Shift"};
	return c;
}

// ---- The Qt fixture

void TestQtFixture() {
	const auto path =
	    fs::path(KYTY_TEST_SOURCE_DIR) / "tests/fixtures/settings/qt6-game-configs.json";
	const auto result = kyty::core::ReadGameSettingsFile(path);
	Check(result.has_value(), "qt6-game-configs.json imports");
	Check(result->by_title.size() == 3 && result->by_title.contains("PPSA01234") &&
	          result->by_title.contains("PPSA02468") && result->by_title.contains("PPSA09876"),
	      "three members");
	for (const auto& [title, patch]: result->by_title) {
		Check(patch.fields.size() == 33, title + ": the 33 fields of the Qt export");
		Check(!patch.fields.contains("input_mapping_custom") &&
		          !patch.fields.contains("host_input_mapping"),
		      title + ": the Qt export has no input mapping fields");
	}

	GameConfiguration astro;
	astro.gpu_index                = 0;
	astro.hdr_enabled              = true;
	astro.present_mode             = PresentMode::Fifo;
	astro.screen_resolution        = ScreenResolution::R1920X1080;
	astro.shader_optimization_type = ShaderOptimizationType::None;
	astro.trophy_duration_seconds  = 7;
	astro.vblank_frequency         = 120;
	GameConfiguration souls;
	souls.console_language     = 0;
	souls.printf_direction     = LogDirection::File;
	souls.printf_output_file   = "demons-souls.log";
	souls.screen_resolution    = ScreenResolution::R2560X1440;
	souls.shader_log_direction = LogDirection::Console;
	GameConfiguration special;
	special.audio_input_device                                            = "@Mic";
	special.command_buffer_dump_folder                                    = "dumps, \"cb\"";
	special.printf_output_file                                            = "@kyty=log;1.txt";
	special.shader_log_folder                                             = "logs\\shaders;v=1";
	special.user_name                                                     = "@Kyty, \"=;\\é";
	const std::vector<std::pair<std::string, GameConfiguration>> expected = {
	    {"PPSA01234", astro}, {"PPSA02468", souls}, {"PPSA09876", special}};
	for (const auto& [title, configuration]: expected) {
		const auto& patch = result->by_title.at(title);
		Check(kyty::core::ApplyPatch(GameConfiguration {}, patch) == configuration,
		      title + ": every field imports unchanged");
		// Each value is the text form of the expected field: format(value) reproduces the export.
		for (const auto& [name, value]: patch.fields) {
			const auto* field = kyty::core::FindGameConfigurationField(name);
			Check(field != nullptr && value == field->get(configuration),
			      title + ": field " + name);
		}
	}
	// The import keeps fields the file does not have.
	auto base                 = GameConfiguration {};
	base.host_input_mapping   = {"Cross=K"};
	base.input_mapping_custom = true;
	const auto applied        = kyty::core::ApplyPatch(base, result->by_title.at("PPSA01234"));
	Check(applied.host_input_mapping == base.host_input_mapping && applied.input_mapping_custom,
	      "fields absent from the file keep their previous value");
}

// ---- Export (Requirement 9.8)

void TestExport() {
	const auto config = Sample();
	const auto text   = ExportGameSettings(" ppsa01234\t", config);
	Check(text.has_value(), "export");
	Check(text->starts_with("{\n    \"PPSA01234\": {\n        \"amd_cpu_enabled\": \"false\",\n"),
	      "4-space indent, the trimmed upper-case title, fields in key order:\n" + *text);
	Check(text->ends_with("\n    }\n}\n"), "ends with a line break");
	Check(text->find("        \"host_input_mapping\": [\n            \"Cross=J\",\n"
	                 "            \"L3=Left Shift\"\n        ],\n") != std::string::npos,
	      "host_input_mapping is an array");
	for (const auto* line: {"\"input_mapping_custom\": \"true\"", "\"user_id\": \"256\"",
	                        "\"screen_resolution\": \"R2560X1440\"", "\"present_mode\": \"Fifo\"",
	                        "\"printf_output_file\": \"\"", "\"elf\": \"bin/eboot_alt.bin\"",
	                        "\"user_name\": \"@Kyty, \\\"=;\\\\\xC3\xA9\"",
	                        "\"shader_log_folder\": \"logs\\\\shaders;v=1\""}) {
		Check(text->find(line) != std::string::npos, std::string("the export has ") + line);
	}

	const auto parsed = Parsed(*text, "the export");
	Check(parsed.by_title.size() == 1 && parsed.by_title.contains("PPSA01234"),
	      "one member keyed by the trimmed upper-case title ID");
	const auto& patch = parsed.by_title.at("PPSA01234");
	Check(patch.fields.size() == kyty::core::GameConfigurationFields().size(), "every field");
	GameConfiguration other;
	other.vblank_frequency   = 200;
	other.host_input_mapping = {"x=y", "z=w"};
	Check(kyty::core::ApplyPatch(other, patch) == config, "the import gives the exported values");

	GameConfiguration empty_mapping;
	const auto        plain = ExportGameSettings("PPSA00001", empty_mapping);
	Check(plain.has_value() && plain->find("\"host_input_mapping\": []") != std::string::npos,
	      "an empty mapping is an empty array");
	Check(Parsed(*plain, "the default export")
	              .by_title.at("PPSA00001")
	              .fields.at("host_input_mapping") == FieldValue(std::vector<std::string> {}),
	      "an empty mapping imports as an empty list");

	for (const auto* title: {"", " ", "\t\n", "\xE3\x80\x80"}) {
		Check(!ExportGameSettings(title, config).has_value(),
		      "no export without a title ID after trimming");
	}
	Check(kyty::core::NormalizeTitleId("  cusa0001x ") == "CUSA0001X" &&
	          kyty::core::NormalizeTitleId("ppsa\xC3\xA9") == "PPSA\xC3\xA9",
	      "NormalizeTitleId trims and upper-cases ASCII letters");

	auto bad_utf8      = GameConfiguration {};
	bad_utf8.user_name = "a\xFF"
	                     "b";
	const auto bad     = ExportGameSettings("PPSA00001", bad_utf8);
	Check(bad.has_value() && bad->find("\"user_name\": \"a\xEF\xBF\xBD"
	                                   "b\"") != std::string::npos,
	      "ill-formed UTF-8 is written as U+FFFD");
}

const char* StepName(AtomicFileStep step) {
	switch (step) {
		case AtomicFileStep::CreateTemp: return "CreateTemp";
		case AtomicFileStep::Write: return "Write";
		case AtomicFileStep::Sync: return "Sync";
		case AtomicFileStep::Close: return "Close";
		case AtomicFileStep::Rename: return "Rename";
	}
	return "?";
}

std::set<std::string> Names(const fs::path& directory) {
	std::set<std::string> names;
	std::error_code       ec;
	for (const auto& entry: fs::directory_iterator(directory, ec)) {
		names.insert(entry.path().filename().string());
	}
	Check(!ec, "list " + directory.string());
	return names;
}

// Requirement 9.15.
void TestWriteFile() {
	const TempDir dir("kyty_game_settings");
	const auto    target = dir.Path() / "PPSA01234.json";
	Check(kyty::core::WriteGameSettingsFile(target, "PPSA01234", Sample()).has_value(), "write");
	const auto written = kyty::core::ReadGameSettingsFile(target);
	Check(written.has_value() &&
	          kyty::core::ApplyPatch({}, written->by_title.at("PPSA01234")) == Sample(),
	      "the written file imports back");

	const std::string old = "{\"old\": true}\n";
	dir.WriteFile("PPSA01234.json", old);
	for (const auto step: {AtomicFileStep::CreateTemp, AtomicFileStep::Write, AtomicFileStep::Sync,
	                       AtomicFileStep::Close, AtomicFileStep::Rename}) {
		kyty::core::SetAtomicFileFaultForTesting(
		    [step](AtomicFileStep current) { return current == step ? ENOSPC : 0; });
		const auto result = kyty::core::WriteGameSettingsFile(target, "PPSA01234", Sample());
		kyty::core::SetAtomicFileFaultForTesting({});
		// The launcher shows IoError::path (the export path) and the message (the reason).
		Check(!result.has_value() && result.error().error_number == ENOSPC &&
		          result.error().path == target &&
		          result.error().message.find(std::generic_category().message(ENOSPC)) !=
		              std::string::npos,
		      std::string("a fault at ") + StepName(step) + " fails and names the path");
		Check(ReadBytes(target) == old,
		      std::string("the old file stays after a fault at ") + StepName(step));
		Check(Names(dir.Path()) == std::set<std::string> {"PPSA01234.json"},
		      std::string("no temporary file after ") + StepName(step));
	}

	const auto untitled = dir.Path() / "untitled.json";
	const auto no_title = kyty::core::WriteGameSettingsFile(untitled, "  ", Sample());
	Check(!no_title.has_value() && no_title.error().error_number == EINVAL &&
	          no_title.error().message.find(untitled.string()) != std::string::npos &&
	          !fs::exists(untitled),
	      "an empty title ID writes nothing");
	const auto no_dir =
	    kyty::core::WriteGameSettingsFile(dir.Path() / "none" / "x.json", "PPSA01234", Sample());
	Check(!no_dir.has_value() && no_dir.error().error_number == ENOENT,
	      "a missing directory fails");

	// Unreadable files (Requirement 9.10).
	const auto missing = kyty::core::ReadGameSettingsFile(dir.Path() / "missing.json");
	Check(!missing.has_value() && missing.error().kind == ImportErrorKind::Unreadable &&
	          missing.error().message.find("missing.json") != std::string::npos,
	      "a missing file is unreadable, and the message names it");
	const auto directory = kyty::core::ReadGameSettingsFile(dir.Path());
	Check(!directory.has_value() && directory.error().kind == ImportErrorKind::Unreadable,
	      "a directory is unreadable");
}

// ---- Import (Requirements 9.9, 9.10)

void TestRejectedFiles() {
	for (const auto* text: {"", " ", "{", "nope", "{\"PPSA01234\": {}", "[1,", "\xFF",
	                        "{\"PPSA01234\": {\"user_id\": 01}}", "{'PPSA01234': {}}"}) {
		CheckRejected(text, ImportErrorKind::InvalidJson, "", "", std::string("not JSON: ") + text);
	}
	for (const auto* text: {"[]", "[{}]", "5", "\"PPSA01234\"", "null", "true"}) {
		CheckRejected(text, ImportErrorKind::NotAnObject, "", "",
		              std::string("not an object: ") + text);
	}
	for (const auto* text: {"{}", " { }\n"}) {
		CheckRejected(text, ImportErrorKind::NoSettings, "", "", "no settings");
	}
	CheckRejected(Member("\"vblank_frequency\": \"60\"", ""), ImportErrorKind::InvalidTitle, "", "",
	              "an empty title");
	CheckRejected(Member("\"vblank_frequency\": \"60\"", " \\t\\u3000"),
	              ImportErrorKind::InvalidTitle, "", "", "a blank title");
	CheckRejected("{\"ppsa01234\": {\"user_id\": \"1\"}, \"PPSA01234\": {\"user_id\": \"2\"}}",
	              ImportErrorKind::DuplicateTitle, "", "PPSA01234", "the same title twice");
	// Plan D30: a key written twice, which the JSON parser merges (the last value wins).
	CheckRejected("{\"PPSA01234\": {\"user_id\": \"1\"}, \"PPSA01234\": {\"user_id\": \"2\"}}",
	              ImportErrorKind::DuplicateTitle, "", "PPSA01234", "the same member key twice");
	CheckRejected("{\"PPSA01234\": {\"user_id\": \"1\"}, \"PPSA00001\": {\"user_id\": \"3\"}, "
	              "\"PPSA01234\": {\"vblank_frequency\": \"90\"}}",
	              ImportErrorKind::DuplicateTitle, "", "PPSA01234",
	              "the same member key twice, apart");
	CheckRejected("{\" ppsa01234\": {\"user_id\": \"1\"}, \" ppsa01234\": {\"user_id\": \"2\"}}",
	              ImportErrorKind::DuplicateTitle, "", "PPSA01234",
	              "the same untrimmed member key twice");
	CheckRejected(Member("\"user_id\": \"1\", \"user_id\": \"2\""), ImportErrorKind::DuplicateField,
	              "user_id", "PPSA01234", "the same field twice");
	CheckRejected(Member("\"user_id\": \"1\", \"hdr_enabled\": true, \"user_id\": \"1\""),
	              ImportErrorKind::DuplicateField, "user_id", "PPSA01234",
	              "the same field and value twice, apart");
	CheckRejected("{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA01234\": {\"elf\": \"a\", \"x\": "
	              "{\"k\": 1, \"k\": 2}, \"elf\": \"b\"}}",
	              ImportErrorKind::DuplicateField, "elf", "PPSA01234",
	              "the same field twice in a second member");
	// The same field in two members, and repeated keys deeper than a member's fields, are not
	// repeated member fields.
	Check(Parsed("{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA00002\": {\"user_id\": \"1\"}}",
	             "one field in two members")
	              .by_title.size() == 2,
	      "the same field in two members is accepted");
	CheckRejected(Member("\"host_input_mapping\": [{\"k\": 1, \"k\": 2}]"),
	              ImportErrorKind::WrongType, "host_input_mapping", "PPSA01234",
	              "a repeated key inside a field value is that value's wrong type");
	CheckRejected("{\"PPSA01234\": {\"user_id\": \"1\"}, \"PPSA01234\": 5}",
	              ImportErrorKind::DuplicateTitle, "", "PPSA01234",
	              "the same member key twice, the second not an object");
	for (const auto* value: {"5", "[]", "\"x\"", "null", "true"}) {
		CheckRejected(std::string("{\"PPSA01234\": ") + value + "}",
		              ImportErrorKind::MemberNotAnObject, "", "PPSA01234",
		              std::string("member ") + value);
	}
	CheckRejected(Member(""), ImportErrorKind::EmptyMember, "", "PPSA01234", "an empty member");
	CheckRejected("{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA01234\": {}}",
	              ImportErrorKind::EmptyMember, "", "PPSA01234", "an empty second member");
	for (const auto* name: {"red_zone_protection_enabled", "name", "game_path", "basedir",
	                        "custom_settings", "controller_color", "Vblank_frequency", "foo", ""}) {
		CheckRejected(Member("\"" + std::string(name) + "\": \"1\""), ImportErrorKind::UnknownField,
		              name, "PPSA01234", std::string("unknown field ") + name);
	}
}

void TestWrongTypes() {
	struct Case {
		std::string field;
		std::string json;
	};
	const std::vector<Case> cases = {
	    // Bool: a string or a boolean.
	    {"hdr_enabled", "1"},
	    {"hdr_enabled", "1.5"},
	    {"hdr_enabled", "null"},
	    {"hdr_enabled", "[]"},
	    {"hdr_enabled", "{}"},
	    {"hdr_enabled", "[\"true\"]"},
	    // Int: a string or an integer.
	    {"vblank_frequency", "true"},
	    {"vblank_frequency", "60.0"},
	    {"vblank_frequency", "6e1"},
	    {"vblank_frequency", "null"},
	    {"vblank_frequency", "[\"60\"]"},
	    {"vblank_frequency", "{}"},
	    // Enum and String: a string.
	    {"present_mode", "1"},
	    {"present_mode", "true"},
	    {"present_mode", "[\"Fifo\"]"},
	    {"user_name", "1"},
	    {"user_name", "false"},
	    {"user_name", "null"},
	    {"user_name", "[\"Kyty\"]"},
	    {"elf", "{}"},
	    // StringList: an array of strings.
	    {"host_input_mapping", "\"Cross=J\""},
	    {"host_input_mapping", "[\"Cross=J\", 1]"},
	    {"host_input_mapping", "[null]"},
	    {"host_input_mapping", "5"},
	    {"host_input_mapping", "{}"},
	    {"host_input_mapping", "true"},
	};
	for (const auto& c: cases) {
		CheckRejected(Member("\"" + c.field + "\": " + c.json), ImportErrorKind::WrongType, c.field,
		              "PPSA01234", c.field + " = " + c.json);
	}
	const auto message = ParseGameSettingsFile(Member("\"hdr_enabled\": 1")).error().message;
	Check(message.find("a string or a boolean") != std::string::npos,
	      "the message names the types");
}

void TestInvalidValues() {
	struct Case {
		std::string fields;
		std::string field;
	};
	const std::string       seventeen = "\"0123456789abcdefg\"";
	const std::vector<Case> cases     = {
	    {"\"hdr_enabled\": \"maybe\"", "hdr_enabled"},
	    {"\"hdr_enabled\": \"TRUE\"", "hdr_enabled"},
	    {"\"hdr_enabled\": \"1\"", "hdr_enabled"},
	    {"\"vblank_frequency\": \"abc\"", "vblank_frequency"},
	    {"\"vblank_frequency\": \"60.0\"", "vblank_frequency"},
	    {"\"vblank_frequency\": \"+60\"", "vblank_frequency"},
	    {"\"vblank_frequency\": \"29\"", "vblank_frequency"},
	    {"\"vblank_frequency\": 361", "vblank_frequency"},
	    {"\"trophy_duration_seconds\": \"4\"", "trophy_duration_seconds"},
	    {"\"trophy_duration_seconds\": 11", "trophy_duration_seconds"},
	    {"\"user_id\": -1", "user_id"},
	    {"\"user_id\": 254", "user_id"},
	    {"\"user_id\": \"255\"", "user_id"},
	    {"\"user_id\": 2147482880", "user_id"},
	    {"\"user_id\": 9223372036854775808", "user_id"},
	    {"\"user_id\": 18446744073709551615", "user_id"},
	    {"\"user_id\": \"99999999999999999999\"", "user_id"},
	    {"\"gpu_index\": -2", "gpu_index"},
	    {"\"gpu_index\": 2147483648", "gpu_index"},
	    {"\"console_language\": \"30\"", "console_language"},
	    {"\"console_language\": -1", "console_language"},
	    {"\"user_name\": \"\"", "user_name"},
	    {"\"user_name\": \"   \"", "user_name"},
	    {"\"user_name\": " + seventeen, "user_name"},
	    {"\"elf\": \"\"", "elf"},
	    {"\"elf\": \"/bin/sh\"", "elf"},
	    {"\"elf\": \"../eboot.bin\"", "elf"},
	    {"\"present_mode\": \"Vsync\"", "present_mode"},
	    {"\"present_mode\": \"3\"", "present_mode"},
	    {"\"screen_resolution\": \"R1280x720\"", "screen_resolution"},
	    // The mapping entries are checked when the member turns the per-game mapping on.
	    {"\"input_mapping_custom\": true, \"host_input_mapping\": [\"Cross\"]",
	     "host_input_mapping"},
	    {"\"host_input_mapping\": [\"Cross=J\", \"=J\"], \"input_mapping_custom\": \"true\"",
	     "host_input_mapping"},
	    {"\"shader_log_direction\": \"File\", \"shader_log_folder\": \"\"", "shader_log_folder"},
	    {"\"printf_direction\": \"File\", \"printf_output_file\": \"\"", "printf_output_file"},
	    {"\"command_buffer_dump_enabled\": true, \"command_buffer_dump_folder\": \"\"",
	     "command_buffer_dump_folder"},
	};
	for (const auto& c: cases) {
		CheckRejected(Member(c.fields), ImportErrorKind::InvalidValue, c.field, "PPSA01234",
		              "{" + c.fields + "}");
	}
	Check(ParseGameSettingsFile(Member("\"vblank_frequency\": 29"))
	              .error()
	              .message.find("between 30 and 360") != std::string::npos,
	      "the message states the range");
	Check(ParseGameSettingsFile(Member("\"present_mode\": \"Vsync\""))
	              .error()
	              .message.find("Fifo, Mailbox, or Immediate") != std::string::npos,
	      "the message lists the enum keys");
	// The first bad member fails the whole file.
	CheckRejected("{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA00002\": {\"user_id\": 255}}",
	              ImportErrorKind::InvalidValue, "user_id", "PPSA00002", "a bad second member");
	const auto mapping = ParseGameSettingsFile(
	    Member("\"input_mapping_custom\": true, \"host_input_mapping\": [\"Cross=J\", \"bad\", "
	           "\"=K\"]"));
	Check(!mapping.has_value() &&
	          mapping.error().message.find("entry 2 \"bad\", entry 3 \"=K\"") != std::string::npos,
	      "the message lists every invalid entry: " +
	          (mapping ? std::string() : mapping.error().message));

	// Plan D26: entries of a mapping that the member leaves unused are not checked here; the
	// configuration the member is applied to decides (SaveCustomSettings, TestQtMappingImport).
	const std::vector<std::pair<std::string, std::vector<std::string>>> unused = {
	    {"\"host_input_mapping\": [\"Cross\", \"=J\"]", {"Cross", "=J"}},
	    {"\"host_input_mapping\": [\"Cross\"], \"input_mapping_custom\": false", {"Cross"}},
	};
	for (const auto& [fields, entries]: unused) {
		const auto parsed = Parsed(Member(fields), "an unused mapping {" + fields + "}");
		Check(parsed.by_title.at("PPSA01234").fields.at("host_input_mapping") ==
		          FieldValue(entries),
		      "the unused entries are imported as they are: " + fields);
	}
}

void TestAcceptedForms() {
	const auto result  = Parsed(Member("\"hdr_enabled\": true, \"trophy_enabled\": false, "
	                                   "\"vblank_frequency\": 144, \"user_id\": 0, "
	                                   "\"gpu_index\": -1, \"console_language\": 29, "
	                                   "\"screen_resolution\": \"1920x1080\", "
	                                   "\"present_mode\": \"0\", \"printf_direction\": \"File\", "
	                                   "\"host_input_mapping\": [], "
	                                   "\"input_mapping_custom\": true",
	                                   " ppsa01234 "),
	                            "native JSON types and legacy enum forms");
	const auto applied = kyty::core::ApplyPatch({}, result.by_title.at("PPSA01234"));
	Check(applied.hdr_enabled && !applied.trophy_enabled && applied.vblank_frequency == 144 &&
	          applied.user_id == 0 && applied.gpu_index == -1 && applied.console_language == 29 &&
	          applied.screen_resolution == ScreenResolution::R1920X1080 &&
	          applied.present_mode == PresentMode::Fifo &&
	          applied.printf_direction == LogDirection::File &&
	          applied.printf_output_file == "_kyty.txt" && applied.input_mapping_custom,
	      "accepted values");
	Check(result.by_title.at("PPSA01234").fields.size() == 11, "only the fields present");

	// Bulk import: every member keyed by its upper-case ID.
	const auto bulk = Parsed("{\"ppsa00001\": {\"user_id\": \"1\"}, \" PPSA00002 \": {\"user_id\": "
	                         "\"2\"}, \"CUSA12345\": {\"user_id\": \"3\"}}",
	                         "bulk");
	Check(bulk.by_title.size() == 3 && bulk.by_title.contains("PPSA00001") &&
	          bulk.by_title.contains("PPSA00002") && bulk.by_title.contains("CUSA12345"),
	      "bulk keys");
}

void TestSelectGameImport() {
	const auto one      = Parsed(Member("\"user_id\": \"7\"", "PPSA01234"), "one member");
	auto       selected = kyty::core::SelectGameImport(one, " ppsa01234 ");
	Check(selected.has_value() && selected->title == "PPSA01234" && !selected->needs_confirmation,
	      "the matching member");
	selected = kyty::core::SelectGameImport(one, "PPSA09999");
	Check(selected.has_value() && selected->title == "PPSA01234" && selected->needs_confirmation,
	      "one member for another title needs confirmation");
	selected = kyty::core::SelectGameImport(one, "");
	Check(selected.has_value() && selected->needs_confirmation,
	      "one member for a game without a title ID needs confirmation");

	const auto two =
	    Parsed("{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA00002\": {\"user_id\": \"2\"}}",
	           "two members");
	selected = kyty::core::SelectGameImport(two, "PPSA00002");
	Check(selected.has_value() && selected->title == "PPSA00002" && !selected->needs_confirmation &&
	          selected->patch.fields.at("user_id") == FieldValue(int64_t {2}),
	      "the matching member of two");
	for (const auto* title: {"PPSA09999", ""}) {
		const auto none = kyty::core::SelectGameImport(two, title);
		Check(!none.has_value() && none.error().kind == ImportErrorKind::NoMatchingMember &&
		          !none.error().message.empty(),
		      std::string("no member for '") + title + "' among two");
	}
	Check(kyty::core::SelectGameImport(two, "PPSA09999").error().message.find("PPSA09999") !=
	          std::string::npos,
	      "the message names the title");
}

// The import flow of the GTK launcher: parse, select, apply to the game's configuration, save.
std::optional<std::string> Import(LauncherSettings& settings, const GameEntry& entry,
                                  std::string_view bytes) {
	const auto parsed = ParseGameSettingsFile(bytes);
	if (!parsed) {
		return parsed.error().message;
	}
	const auto selected = kyty::core::SelectGameImport(*parsed, entry.title_id);
	if (!selected) {
		return selected.error().message;
	}
	const auto base   = kyty::core::EnableCustomSettings(settings, entry);
	const auto result = kyty::core::ApplyPatch(base, selected->patch);
	const auto saved  = kyty::core::SaveCustomSettings(settings, entry, result);
	if (!saved) {
		return saved.error().field + ": " + saved.error().message;
	}
	return std::nullopt;
}

void TestPartialImport() {
	LauncherSettings settings;
	settings.global.vblank_frequency   = 90;
	settings.global.host_input_mapping = {"Cross=K"};
	const auto game                    = Entry("/games/PPSA01234", "PPSA01234");

	// A game without custom settings: the Global_Configuration plus the file's fields.
	Check(!Import(settings, game, Member("\"hdr_enabled\": \"true\"")), "import");
	const auto* stored   = kyty::core::FindGameSettings(settings, game);
	auto        expected = settings.global;
	expected.hdr_enabled = true;
	Check(stored != nullptr && stored->configuration == expected,
	      "fields in the file change, the rest keeps the previous (global) value, marked custom");

	// A custom game: its own values stay.
	Check(!Import(settings, game, Member("\"user_id\": 5")), "second import");
	expected.user_id = 5;
	Check(kyty::core::FindGameSettings(settings, game)->configuration == expected,
	      "a second import changes only its field");

	// Rejected files and values change nothing.
	settings.games[0].configuration.shader_log_direction = LogDirection::File;
	const auto                     before                = settings;
	const std::vector<std::string> rejected              = {
	    "nope",
	    Member("\"user_id\": 254"),
	    Member("\"foo\": \"1\""),
	    "{\"PPSA00001\": {\"user_id\": \"1\"}, \"PPSA00002\": {\"user_id\": \"2\"}}",
	    // Valid on its own, but the game logs shaders to a file.
	    Member("\"shader_log_folder\": \"\""),
	    // Repeated keys (plan D30).
	    "{\"PPSA01234\": {\"user_id\": \"7\"}, \"PPSA01234\": {\"user_id\": \"8\"}}",
	    Member("\"user_id\": \"7\", \"user_id\": \"8\""),
	};
	for (const auto& bytes: rejected) {
		auto       copy  = settings;
		const auto error = Import(copy, game, bytes);
		Check(error.has_value(), "rejected: " + bytes);
		Check(copy == before, "a rejected import leaves the store unchanged: " + bytes);
	}
	const auto folder = Import(settings, game, Member("\"shader_log_folder\": \"\""));
	Check(folder.has_value() && folder->starts_with("shader_log_folder: "),
	      "an empty folder for a game that logs shaders to a file names the field: " +
	          folder.value_or(""));
}

// Plan D26 (Y1): imports into special game 4 of qt6-special-values.ini, whose four stored mapping
// entries have no '=' and whose per-game mapping is off.
void TestQtMappingImport() {
	const auto report = kyty::core::LoadSettings(fs::path(KYTY_TEST_SOURCE_DIR) /
	                                             "tests/fixtures/settings/qt6-special-values.ini");
	Check(report.has_value() && report->settings.games.size() == 4, "load qt6-special-values.ini");
	auto                           settings = report->settings;
	const auto                     game     = Entry(settings.games[3].game_path, "PPSA00002");
	const std::vector<std::string> stored = {" leading space", "trailing space ", "@", "@@double"};
	Check(settings.games[3].configuration.host_input_mapping == stored &&
	          !settings.games[3].configuration.input_mapping_custom,
	      "game 4 holds the four entries");

	Check(!Import(settings, game, Member("\"vblank_frequency\": 90", "PPSA00002")),
	      "an import that does not touch the mapping succeeds");
	const auto* imported = kyty::core::FindGameSettings(settings, game);
	Check(imported != nullptr && imported->configuration.vblank_frequency == 90 &&
	          imported->configuration.host_input_mapping == stored &&
	          !imported->configuration.input_mapping_custom,
	      "the import keeps the four entries");

	// Turning the per-game mapping on with the stored entries fails, naming the field and each
	// entry, and changes nothing.
	const auto before = settings;
	const auto error =
	    Import(settings, game, Member("\"input_mapping_custom\": true", "PPSA00002"));
	Check(error.has_value() && error->starts_with("host_input_mapping: "),
	      "an import that turns input_mapping_custom on fails naming the field: " +
	          error.value_or(""));
	for (size_t i = 0; i < stored.size(); i++) {
		Check(error->find("entry " + std::to_string(i + 1) + " \"" + stored[i] + "\"") !=
		          std::string::npos,
		      "the error names entry " + std::to_string(i + 1) + ": " + *error);
	}
	Check(settings == before, "the rejected import changes nothing");
	CheckRejected(
	    Member("\"input_mapping_custom\": true, \"host_input_mapping\": [\"@\"]", "PPSA00002"),
	    ImportErrorKind::InvalidValue, "host_input_mapping", "PPSA00002",
	    "a member that turns the mapping on with an invalid entry");

	// Valid entries with the mapping on replace the stored ones. After that, invalid entries in a
	// member without input_mapping_custom are checked against the game's mapping, which is now on.
	Check(!Import(settings, game,
	              Member("\"input_mapping_custom\": true, \"host_input_mapping\": [\"Cross=J\"]",
	                     "PPSA00002")),
	      "an import that turns the mapping on with valid entries succeeds");
	Check(kyty::core::FindGameSettings(settings, game)->configuration.host_input_mapping ==
	          std::vector<std::string> {"Cross=J"},
	      "the valid entries replace the stored ones");
	const auto after = settings;
	const auto on =
	    Import(settings, game, Member("\"host_input_mapping\": [\"Cross\"]", "PPSA00002"));
	Check(on.has_value() && on->starts_with("host_input_mapping: ") && settings == after,
	      "invalid entries for a game whose mapping is on are rejected: " + on.value_or(""));
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GameSettingsFileTests");
	TestQtFixture();
	TestExport();
	TestWriteFile();
	TestRejectedFiles();
	TestWrongTypes();
	TestInvalidValues();
	TestAcceptedForms();
	TestSelectGameImport();
	TestPartialImport();
	TestQtMappingImport();
	std::puts("GameSettingsFileTests: all tests passed");
	return 0;
}
