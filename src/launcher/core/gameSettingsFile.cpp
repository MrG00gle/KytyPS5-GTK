#include "launcher/core/gameSettingsFile.h"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kyty::core {

namespace {

using Json = nlohmann::json;

Unexpected<ImportError> Fail(ImportErrorKind kind, std::string title, std::string field,
                             std::string message) {
	return Unexpected<ImportError>(
	    ImportError {kind, std::move(title), std::move(field), std::move(message)});
}

std::string Quoted(std::string_view text) {
	return "\"" + std::string(text) + "\"";
}

std::string FieldOf(const FieldDescriptor& field, const std::string& title) {
	return "game setting " + Quoted(field.name) + " for " + title;
}

std::string AcceptedTypes(FieldKind kind) {
	switch (kind) {
		case FieldKind::Bool: return "a string or a boolean";
		case FieldKind::Int: return "a string or an integer";
		case FieldKind::StringList: return "an array of strings";
		case FieldKind::Enum:
		case FieldKind::String: break;
	}
	return "a string";
}

// Why a text form of the field does not parse.
std::string ParseFailure(const FieldDescriptor& field) {
	switch (field.kind) {
		case FieldKind::Bool: return "The value must be true or false.";
		case FieldKind::Int: return "The value must be a decimal integer.";
		case FieldKind::Enum: return field.check(FieldValue(int64_t {-1})).value_or(std::string());
		case FieldKind::String:
		case FieldKind::StringList: break;
	}
	return "The value is not valid.";
}

// The value of one member field: the JSON types the field accepts, then parse, check, and set.
Expected<FieldValue, ImportError> ReadField(const FieldDescriptor& field, const Json& json,
                                            const std::string& title) {
	bool                      accepted  = false;
	bool                      from_text = false;
	std::optional<FieldValue> value;
	if (const auto* text = json.get_ptr<const Json::string_t*>()) {
		accepted  = field.kind != FieldKind::StringList;
		from_text = true;
		if (accepted) {
			value = field.parse(*text);
		}
	} else if (const auto* flag = json.get_ptr<const Json::boolean_t*>()) {
		accepted = field.kind == FieldKind::Bool;
		value    = FieldValue(*flag);
	} else if (json.is_number_integer()) {
		accepted = field.kind == FieldKind::Int;
		if (const auto* number = json.get_ptr<const Json::number_integer_t*>()) {
			value = FieldValue(static_cast<int64_t>(*number));
		} else if (const auto* unsigned_number = json.get_ptr<const Json::number_unsigned_t*>();
		           unsigned_number != nullptr &&
		           *unsigned_number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			value = FieldValue(static_cast<int64_t>(*unsigned_number));
		}
	} else if (json.is_array() && field.kind == FieldKind::StringList) {
		std::vector<std::string> list;
		accepted = true;
		for (const auto& element: json) {
			const auto* text = element.get_ptr<const Json::string_t*>();
			if (text == nullptr) {
				accepted = false;
				break;
			}
			list.push_back(*text);
		}
		value = FieldValue(std::move(list));
	}

	if (!accepted) {
		return Fail(ImportErrorKind::WrongType, title, std::string(field.name),
		            "The " + FieldOf(field, title) + " must be " + AcceptedTypes(field.kind) + ".");
	}
	const auto invalid = [&](const std::string& reason) {
		return Fail(ImportErrorKind::InvalidValue, title, std::string(field.name),
		            "Invalid value for " + FieldOf(field, title) + ": " + reason);
	};
	if (!value) {
		return invalid(from_text ? ParseFailure(field) : "The value is out of range.");
	}
	// check() also covers what set() needs: every integer field's range is inside int32_t.
	if (auto message = field.check(*value)) {
		return invalid(*message);
	}
	return std::move(*value);
}

// The object keys that the file repeats, which the parsed value no longer shows (the last value of
// a repeated key wins): member keys, and per member key the first field key written twice.
struct RepeatedKeys {
	std::set<std::string>              members;
	std::map<std::string, std::string> fields;
};

// Json::parse with a callback that records RepeatedKeys. Depth 1 holds the keys of the root
// object, depth 2 the keys of an object that is the value of a root member; deeper keys belong to
// field values, which are rejected for their type anyway.
Json ParseRecordingRepeatedKeys(std::string_view bytes, RepeatedKeys& repeated) {
	std::vector<std::set<std::string>> open; // the keys of each object being parsed
	std::string                        member;
	const Json::parser_callback_t      callback = [&](int depth, Json::parse_event_t event,
	                                                  Json& parsed) {
		if (event == Json::parse_event_t::object_start) {
			open.emplace_back();
		} else if (event == Json::parse_event_t::object_end) {
			if (!open.empty()) {
				open.pop_back();
			}
		} else if (event == Json::parse_event_t::key && !open.empty()) {
			const auto* key = parsed.get_ptr<const Json::string_t*>();
			if (key != nullptr) {
				const bool repeated_key = !open.back().insert(*key).second;
				if (depth == 1) {
					member = *key;
					if (repeated_key) {
						repeated.members.insert(*key);
					}
				} else if (depth == 2 && repeated_key) {
					repeated.fields.try_emplace(member, *key);
				}
			}
		}
		return true;
	};
	// No exceptions: a parse error gives a discarded value.
	return Json::parse(bytes.begin(), bytes.end(), callback, false);
}

Expected<GameConfigurationPatch, ImportError>
ReadMember(const std::string& title, const Json& member, const std::string* repeated_field) {
	if (!member.is_object()) {
		return Fail(ImportErrorKind::MemberNotAnObject, title, {},
		            "The game settings for " + title + " are not a JSON object.");
	}
	if (member.empty()) {
		return Fail(ImportErrorKind::EmptyMember, title, {},
		            "The game settings for " + title + " contain no setting.");
	}
	if (repeated_field != nullptr) {
		return Fail(ImportErrorKind::DuplicateField, title, *repeated_field,
		            "The game settings for " + title + " contain the game setting " +
		                Quoted(*repeated_field) + " more than once.");
	}
	GameConfigurationPatch patch;
	for (const auto& [name, json]: member.items()) {
		const auto* field = FindGameConfigurationField(name);
		if (field == nullptr) {
			return Fail(ImportErrorKind::UnknownField, title, name,
			            "Unknown game setting " + Quoted(name) + " for " + title + ".");
		}
		auto value = ReadField(*field, json, title);
		if (!value) {
			return MakeUnexpected(std::move(value).error());
		}
		patch.fields.emplace(name, std::move(*value));
	}
	// Rules between fields of the member (an empty output path while its output is on) and the
	// input mapping entries (when the member turns input_mapping_custom on).
	if (auto error = Validate(ApplyPatch(GameConfiguration {}, patch), ValidationScope::Game)) {
		return Fail(ImportErrorKind::InvalidValue, title, error->field,
		            "Invalid value for game setting " + Quoted(error->field) + " for " + title +
		                ": " + error->message);
	}
	return patch;
}

} // namespace

std::optional<std::string> ExportGameSettings(std::string_view         title_id,
                                              const GameConfiguration& configuration) {
	auto title = NormalizeTitleId(title_id);
	if (title.empty()) {
		return std::nullopt;
	}
	Json member = Json::object();
	for (const auto& field: GameConfigurationFields()) {
		const auto value = field.get(configuration);
		if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
			member[std::string(field.name)] = *list;
		} else {
			member[std::string(field.name)] = field.format(value);
		}
	}
	Json root              = Json::object();
	root[std::move(title)] = std::move(member);
	return root.dump(4, ' ', false, Json::error_handler_t::replace) + "\n";
}

Expected<void, IoError> WriteGameSettingsFile(const std::filesystem::path& path,
                                              std::string_view             title_id,
                                              const GameConfiguration&     configuration) {
	const auto bytes = ExportGameSettings(title_id, configuration);
	if (!bytes) {
		return MakeUnexpected(IoError {path, EINVAL,
		                               "Cannot export game settings to " + path.string() +
		                                   ": the game has no title ID"});
	}
	return WriteFileAtomically(path, *bytes);
}

Expected<ImportResult, ImportError> ParseGameSettingsFile(std::string_view bytes) {
	RepeatedKeys repeated;
	const auto   root = ParseRecordingRepeatedKeys(bytes, repeated);
	if (root.is_discarded()) {
		return Fail(ImportErrorKind::InvalidJson, {}, {}, "The file is not valid JSON.");
	}
	if (!root.is_object()) {
		return Fail(ImportErrorKind::NotAnObject, {}, {},
		            "The file must contain a JSON object with game settings keyed by title ID.");
	}
	if (root.empty()) {
		return Fail(ImportErrorKind::NoSettings, {}, {}, "The file contains no game settings.");
	}
	ImportResult result;
	for (const auto& [key, member]: root.items()) {
		auto title = NormalizeTitleId(key);
		if (title.empty()) {
			return Fail(ImportErrorKind::InvalidTitle, {}, {},
			            "The file contains game settings without a title ID.");
		}
		if (result.by_title.contains(title) || repeated.members.contains(key)) {
			return Fail(ImportErrorKind::DuplicateTitle, title, {},
			            "The file contains game settings for " + title + " more than once.");
		}
		const auto field = repeated.fields.find(key);
		auto       patch =
		    ReadMember(title, member, field != repeated.fields.end() ? &field->second : nullptr);
		if (!patch) {
			return MakeUnexpected(std::move(patch).error());
		}
		result.by_title.emplace(std::move(title), std::move(*patch));
	}
	return result;
}

Expected<ImportResult, ImportError> ReadGameSettingsFile(const std::filesystem::path& path) {
	auto bytes = ReadFileLimited(path, kMaxGameSettingsFileBytes);
	if (!bytes) {
		return Fail(ImportErrorKind::Unreadable, {}, {}, bytes.error().message);
	}
	return ParseGameSettingsFile(*bytes);
}

Expected<GameImport, ImportError> SelectGameImport(const ImportResult& result,
                                                   std::string_view    title_id) {
	const auto title = NormalizeTitleId(title_id);
	if (!title.empty()) {
		if (const auto it = result.by_title.find(title); it != result.by_title.end()) {
			return GameImport {it->first, it->second, false};
		}
	}
	if (result.by_title.size() == 1) {
		const auto& [key, patch] = *result.by_title.begin();
		return GameImport {key, patch, true};
	}
	return Fail(ImportErrorKind::NoMatchingMember, title, {},
	            title.empty() ? "The file contains no game settings for this game, which has no "
	                            "title ID."
	                          : "The file contains no game settings for " + title + ".");
}

GameConfiguration ApplyPatch(const GameConfiguration& base, const GameConfigurationPatch& patch) {
	GameConfiguration result = base;
	for (const auto& [name, value]: patch.fields) {
		if (const auto* field = FindGameConfigurationField(name)) {
			field->set(result, value);
		}
	}
	return result;
}

} // namespace kyty::core
