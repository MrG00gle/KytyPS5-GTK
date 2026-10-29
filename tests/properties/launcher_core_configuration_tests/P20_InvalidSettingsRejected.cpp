// clang-format off
// Feature: linux-gtk-modernization, Property 20: Invalid settings files are rejected without changes
// clang-format on
//
// Validates Requirement 9.10. A Game_Settings_File of 1-3 valid members (random field subsets of
// valid configurations, in the accepted JSON forms) gets exactly one defect: an unknown field name
// (red_zone_protection_enabled, entry keys, global-only fields, other text), a value of a JSON
// type its field does not accept, a value outside its range (every Requirement 9.4-invalid value,
// the gpu_index, console_language, elf, enum, and input mapping rules, unparsable text, an empty
// output path while the member turns its output on), an empty member, an empty root object, a
// member that is not an object, or no member for the selected title among two or more; or an
// empty output path that the game's own configuration makes invalid. Also non-JSON bytes and JSON
// whose root is not an object. The import flow of the GTK launcher (ParseGameSettingsFile,
// SelectGameImport, ApplyPatch, SaveCustomSettings) must fail with the expected kind and a message
// that names the rejected field (and the title) or states the reason, and leave the target
// configuration, the LauncherSettings, and the Kyty.ini text unchanged.

#include "launcher/core/configurationFields.h"
#include "launcher/core/gameConfiguration.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/gameSettingsFile.h"
#include "launcher/core/settingsStore.h"
#include "support/configurationGenerators.h"
#include "support/property.h"

#include <cstdint>
#include <map>
#include <optional>
#include <rapidcheck.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace KytyTests::ConfigGen;
using kyty::core::FieldKind;
using kyty::core::ImportErrorKind;

// What the flow reports.
struct Failure {
	std::optional<ImportErrorKind> kind; // nothing for a ValidationError from SaveCustomSettings
	std::string                    title;
	std::string                    field;
	std::string                    message;
};

std::optional<Failure> Import(kyty::core::LauncherSettings& settings,
                              const kyty::core::GameEntry& entry, const std::string& bytes) {
	const auto parsed = kyty::core::ParseGameSettingsFile(bytes);
	if (!parsed) {
		const auto& e = parsed.error();
		return Failure {e.kind, e.title, e.field, e.message};
	}
	const auto selected = kyty::core::SelectGameImport(*parsed, entry.title_id);
	if (!selected) {
		const auto& e = selected.error();
		return Failure {e.kind, e.title, e.field, e.message};
	}
	const auto base   = kyty::core::EnableCustomSettings(settings, entry);
	const auto result = kyty::core::ApplyPatch(base, selected->patch);
	const auto saved  = kyty::core::SaveCustomSettings(settings, entry, result);
	if (!saved) {
		return Failure {std::nullopt, {}, saved.error().field, saved.error().message};
	}
	return std::nullopt;
}

using Member = std::map<std::string, std::string>; // field -> JSON text

Member ValidMember() {
	const auto source = ValidConfiguration(true);
	Member     member;
	const auto all  = kyty::core::GameConfigurationFields();
	const int  rate = Choose(1, 6);
	for (const auto& field: all) {
		if (OneIn(rate)) {
			member[std::string(field.name)] = EncodeField(field, field.get(source));
		}
	}
	if (member.empty()) {
		const auto& field               = all[Choose<size_t>(0, all.size())];
		member[std::string(field.name)] = EncodeField(field, field.get(source));
	}
	return member;
}

std::string MemberJson(const Member& member) {
	return JsonObject(
	    std::vector<std::pair<std::string, std::string>>(member.begin(), member.end()));
}

std::string FileJson(const std::map<std::string, std::string>& members) {
	return JsonObject(
	    std::vector<std::pair<std::string, std::string>>(members.begin(), members.end()));
}

// A JSON value of a type the field does not accept.
std::string WrongType(FieldKind kind) {
	switch (kind) {
		case FieldKind::Bool:
			return PickOf(std::vector<std::string> {"1", "0.5", "null", "[]", "{}"});
		case FieldKind::Int:
			return PickOf(
			    std::vector<std::string> {"true", "60.0", "1e2", "null", "[\"60\"]", "{}"});
		case FieldKind::Enum:
		case FieldKind::String:
			return PickOf(std::vector<std::string> {"1", "false", "null", "[\"x\"]", "{}", "-0.5"});
		case FieldKind::StringList:
			return PickOf(std::vector<std::string> {"\"Cross=J\"", "1", "true", "null", "{}",
			                                        "[\"Cross=J\", 2]", "[[]]"});
	}
	return "null";
}

struct OutOfRange {
	std::string field;
	std::string json;
	Member      extra; // other fields the defect needs (the output that is turned on)
};

OutOfRange OutOfRangeValue() {
	const auto                    q      = [](const char* text) { return JsonQuote(text); };
	const std::vector<OutOfRange> values = {
	    {"user_name", q(""), {}},
	    {"user_name", q(" \t"), {}},
	    {"user_name", q("0123456789abcdefg"), {}},
	    {"user_name", q("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9!"), {}},
	    {"user_id", "-1", {}},
	    {"user_id", "254", {}},
	    {"user_id", q("255"), {}},
	    {"user_id", "2147482880", {}},
	    {"user_id", "9223372036854775808", {}},
	    {"vblank_frequency", "29", {}},
	    {"vblank_frequency", q("361"), {}},
	    {"trophy_duration_seconds", "4", {}},
	    {"trophy_duration_seconds", q("11"), {}},
	    {"gpu_index", "-2", {}},
	    {"gpu_index", "2147483648", {}},
	    {"console_language", "30", {}},
	    {"console_language", q("-1"), {}},
	    {"elf", q(""), {}},
	    {"elf", q("/eboot.bin"), {}},
	    {"elf", q("a/../b"), {}},
	    {"present_mode", q("Vsync"), {}},
	    {"screen_resolution", q("4"), {}},
	    {"shader_log_direction", q("file"), {}},
	    {"hdr_enabled", q("maybe"), {}},
	    {"hdr_enabled", q("1"), {}},
	    {"vblank_frequency", q("sixty"), {}},
	    {"vblank_frequency", q("+60"), {}},
	    {"host_input_mapping", "[\"Cross\"]", {}},
	    {"host_input_mapping", "[\"Cross=J\", \" =J\"]", {}},
	    {"shader_log_folder", q(""), {{"shader_log_direction", q("File")}}},
	    {"printf_output_file", q(""), {{"printf_direction", q("2")}}},
	    {"command_buffer_dump_folder", q(""), {{"command_buffer_dump_enabled", "true"}}},
	};
	return PickOf(values);
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
		case ImportErrorKind::UnknownField: return "UnknownField";
		case ImportErrorKind::WrongType: return "WrongType";
		case ImportErrorKind::InvalidValue: return "InvalidValue";
		case ImportErrorKind::NoMatchingMember: return "NoMatchingMember";
	}
	return "?";
}

} // namespace

KYTY_PROPERTY(20, "Invalid settings files are rejected without changes") {
	return rc::check(name, []() {
		// The target: a game with or without custom settings.
		kyty::core::LauncherSettings settings = ValidSettings();
		kyty::core::GameEntry        entry;
		entry.title_id = "PPSA" + std::to_string(Choose(10000, 100000));
		entry.path     = "/p20/" + entry.title_id;
		if (OneIn(2)) {
			kyty::core::GameSettingsEntry stored;
			stored.game_path     = entry.path.string();
			stored.configuration = ValidConfiguration();
			settings.games.push_back(stored);
		}
		// Valid members; the game's own member is one of them.
		std::map<std::string, Member> members;
		members[entry.title_id] = ValidMember();
		const int extra         = Choose(0, 3);
		for (int i = 0; i < extra; i++) {
			members["PPSA" + std::to_string(Choose(10000, 100000))] = ValidMember();
		}
		std::vector<std::string> keys;
		for (const auto& [key, member]: members) {
			keys.push_back(key);
		}
		const std::string defective = PickOf(keys);

		std::string                    bytes;
		std::optional<ImportErrorKind> kind = ImportErrorKind::InvalidValue;
		std::string                    title;
		std::string                    field;
		const auto                     render = [&] {
			std::map<std::string, std::string> rendered;
			for (const auto& [key, member]: members) {
				rendered[key] = MemberJson(member);
			}
			return rendered;
		};

		const int defect = Choose(0, 11);
		switch (defect) {
			case 0: { // Unknown field.
				field = PickOf(std::vector<std::string> {"red_zone_protection_enabled", "name",
				                                         "game_path", "basedir", "custom_settings",
				                                         "controller_color", "Elf", "user name"});
				if (OneIn(3)) {
					field = "x_" + VisibleText(6);
				}
				members[defective][field] = JsonQuote("1");
				kind                      = ImportErrorKind::UnknownField;
				title                     = defective;
				bytes                     = FileJson(render());
				break;
			}
			case 1: { // Wrong JSON type.
				const auto  all           = kyty::core::GameConfigurationFields();
				const auto& f             = all[Choose<size_t>(0, all.size())];
				field                     = std::string(f.name);
				members[defective][field] = WrongType(f.kind);
				kind                      = ImportErrorKind::WrongType;
				title                     = defective;
				bytes                     = FileJson(render());
				break;
			}
			case 2:
			case 3: { // Out of range.
				const auto value = OutOfRangeValue();
				for (const auto& [name_, json]: value.extra) {
					members[defective][name_] = json;
				}
				field                     = value.field;
				members[defective][field] = value.json;
				kind                      = ImportErrorKind::InvalidValue;
				title                     = defective;
				bytes                     = FileJson(render());
				break;
			}
			case 4: { // Empty member.
				members[defective].clear();
				kind  = ImportErrorKind::EmptyMember;
				title = defective;
				bytes = FileJson(render());
				break;
			}
			case 5: { // A member that is not an object.
				auto rendered       = render();
				rendered[defective] = PickOf(std::vector<std::string> {"5", "[]", "\"x\"", "null"});
				kind                = ImportErrorKind::MemberNotAnObject;
				title               = defective;
				bytes               = FileJson(rendered);
				break;
			}
			case 6: { // No member for the selected title, among two or more.
				members.erase(entry.title_id);
				while (members.size() < 2) {
					members["PPSB" + std::to_string(Choose(10000, 100000))] = ValidMember();
				}
				kind  = ImportErrorKind::NoMatchingMember;
				title = entry.title_id;
				bytes = FileJson(render());
				break;
			}
			case 7: { // An empty root object.
				bytes = OneIn(2) ? "{}" : " {\n} ";
				kind  = ImportErrorKind::NoSettings;
				break;
			}
			case 8: { // JSON whose root is not an object.
				bytes = PickOf(std::vector<std::string> {"[" + FileJson(render()) + "]", "42",
				                                         "\"PPSA01234\"", "null", "false", "[]"});
				kind  = ImportErrorKind::NotAnObject;
				break;
			}
			case 9: { // Not JSON: a cut file, or random bytes that cannot be a JSON object.
				if (OneIn(2)) {
					const auto whole = FileJson(render());
					bytes            = whole.substr(0, Choose<size_t>(0, whole.size()));
					kind             = ImportErrorKind::InvalidJson;
				} else {
					bytes            = *rc::gen::arbitrary<std::string>();
					const auto first = bytes.find_first_not_of(" \t\r\n");
					// Neither an object nor a byte order mark before one.
					if ((first != std::string::npos && bytes[first] == '{') ||
					    bytes.starts_with("\xEF\xBB\xBF")) {
						bytes.insert(0, "x");
					}
					kind.reset(); // InvalidJson or NotAnObject
				}
				break;
			}
			default: { // An empty output path that the game's configuration turns invalid.
				const std::vector<std::pair<std::string, std::string>> outputs = {
				    {"shader_log_folder", "shader_log_direction"},
				    {"printf_output_file", "printf_direction"},
				    {"command_buffer_dump_folder", "command_buffer_dump_enabled"}};
				const auto& [path, output] = PickOf(outputs);
				// The game's configuration turns that output on (custom settings, or the global
				// ones the import starts from).
				auto& on =
				    settings.games.empty() || settings.games.back().game_path != entry.path.string()
				        ? settings.global
				        : settings.games.back().configuration;
				// The member's other fields may turn the other outputs on: give their paths a
				// value, so that the only rule broken is the one for path.
				for (auto* other: {&on.shader_log_folder, &on.command_buffer_dump_folder,
				                   &on.printf_output_file}) {
					if (other->empty()) {
						*other = "kept";
					}
				}
				if (output == "command_buffer_dump_enabled") {
					on.command_buffer_dump_enabled = true;
				} else if (output == "printf_direction") {
					on.printf_direction = kyty::core::LogDirection::File;
				} else {
					on.shader_log_direction = kyty::core::LogDirection::File;
				}
				members[entry.title_id].erase(output);
				members[entry.title_id][path] = JsonQuote("");
				field                         = path;
				kind.reset();
				title = "";
				bytes = FileJson(render());
				break;
			}
		}
		RC_LOG() << "defect " << defect << ": " << bytes << "\n";

		const auto before_settings = settings;
		const auto before_target   = kyty::core::EnableCustomSettings(settings, entry);
		const auto before_ini      = kyty::core::SerializeSettings(settings);
		const auto failure         = Import(settings, entry, bytes);
		RC_ASSERT(failure.has_value());
		RC_LOG() << "message: " << failure->message << "\n";
		RC_ASSERT(!failure->message.empty());
		if (defect == 9 && !kind) {
			RC_ASSERT(failure->kind == ImportErrorKind::InvalidJson ||
			          failure->kind == ImportErrorKind::NotAnObject);
		} else if (defect >= 10) {
			RC_ASSERT(!failure->kind.has_value()); // the ValidationError of SaveCustomSettings
			RC_ASSERT(failure->field == field);
		} else {
			RC_LOG() << "kind " << (failure->kind ? KindName(*failure->kind) : "validation")
			         << ", expected " << KindName(*kind) << "\n";
			RC_ASSERT(failure->kind == kind);
			RC_ASSERT(failure->field == field);
			RC_ASSERT(failure->title == title);
		}
		if (!failure->field.empty()) {
			RC_ASSERT(failure->message.find(failure->field) != std::string::npos ||
			          !failure->kind.has_value());
		}
		if (!failure->title.empty()) {
			RC_ASSERT(failure->message.find(failure->title) != std::string::npos);
		}
		RC_ASSERT(settings == before_settings);
		RC_ASSERT(kyty::core::EnableCustomSettings(settings, entry) == before_target);
		RC_ASSERT(kyty::core::SerializeSettings(settings) == before_ini);

		static const char* const kNames[] = {
		    "unknown field", "wrong type",        "out of range",
		    "out of range",  "empty member",      "member not an object",
		    "no member",     "empty root",        "root not an object",
		    "not JSON",      "invalid in context"};
		RC_TAG(kNames[defect]);
	});
}
