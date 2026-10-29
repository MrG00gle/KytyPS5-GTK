#pragma once

// Game_Settings_File export and import (design.md, "Game_Settings_File"; Requirements 9.8-9.11,
// 9.15; plan D21). The file is the Qt launcher's format, a JSON object keyed by title ID whose
// members hold the Game_Configuration fields as their text form (configurationFields.h), extended
// with input_mapping_custom and with host_input_mapping as an array of strings:
//
//   { "PPSA01234": { "screen_resolution": "R1920X1080", "user_id": "1000", ...,
//                    "host_input_mapping": ["Cross=J", "Pad:Cross=South"] } }
//
// Import accepts, per field, the text form as a JSON string, a JSON boolean for a Bool field, a
// JSON integer for an Int field, and only an array of strings for host_input_mapping; anything else
// is the wrong type. Ported from the Qt launcher's ImportGameSettings/ExportGameSettings
// (src/launcher/src/configurationListWidget.cpp) and Configuration::SetGameSettings.

#include "launcher/core/atomicFile.h"
#include "launcher/core/configurationFields.h"
#include "launcher/core/expected.h"
#include "launcher/core/gameConfiguration.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace kyty::core {

// The largest Game_Settings_File that ReadGameSettingsFile reads.
inline constexpr uint64_t kMaxGameSettingsFileBytes = uint64_t {16} << 20u;

// The fields of one member, by field key, with their values (only the fields the member has).
struct GameConfigurationPatch {
	std::map<std::string, FieldValue> fields;

	friend bool operator==(const GameConfigurationPatch&, const GameConfigurationPatch&) = default;
};

// Every member of a file, keyed by NormalizeTitleId(member key); bulk import (LFI-17) matches
// these keys against the library's title IDs.
struct ImportResult {
	std::map<std::string, GameConfigurationPatch> by_title;

	friend bool operator==(const ImportResult&, const ImportResult&) = default;
};

enum class ImportErrorKind {
	Unreadable,        // the file cannot be read
	InvalidJson,       // not JSON
	NotAnObject,       // the root is not a JSON object
	NoSettings,        // the root object has no member
	InvalidTitle,      // a member key is empty after trimming
	DuplicateTitle,    // two member keys are the same title ID
	MemberNotAnObject, // a member value is not a JSON object
	EmptyMember,       // a member has no field
	UnknownField,      // a field key that is not a Game_Configuration field
	WrongType,         // a field value of a JSON type that the field does not accept
	InvalidValue,      // a field value that does not parse or breaks a Validate() rule
	NoMatchingMember,  // per-game import: no member for the game, and not exactly one member
};

// Requirement 9.10: message names the rejected field and the title ID, or states why the file was
// rejected.
struct ImportError {
	ImportErrorKind kind = ImportErrorKind::InvalidJson;
	std::string     title; // the member's NormalizeTitleId key, when the error is about a member
	std::string     field; // the field key, when the error is about a field
	std::string     message;

	friend bool operator==(const ImportError&, const ImportError&) = default;
};

// The title ID as Game_Settings_File keys use it: TrimWhitespace, then ASCII letters upper-cased.
std::string NormalizeTitleId(std::string_view title_id);

// The file for one game (Requirement 9.8): one member keyed by NormalizeTitleId(title_id) with
// every field of configuration, indented by 4 spaces like Qt's QJsonDocument::Indented, ending
// with a line break. Nothing when the title ID is empty after trimming (the export action is
// disabled then). Ill-formed UTF-8 in a value is written as U+FFFD.
std::optional<std::string> ExportGameSettings(std::string_view         title_id,
                                              const GameConfiguration& configuration);

// Writes ExportGameSettings() to path through WriteFileAtomically, so a failure leaves the file
// that was at path unchanged (Requirement 9.15). An empty title ID gives an IoError with EINVAL
// and writes nothing.
Expected<void, IoError> WriteGameSettingsFile(const std::filesystem::path& path,
                                              std::string_view             title_id,
                                              const GameConfiguration&     configuration);

// Parses a Game_Settings_File; every Requirement 9.10 rejection is an ImportError. A member is
// accepted when each of its fields exists, has a JSON type the field accepts, parses, meets the
// field's rule, and the member applied to GameConfiguration{} passes Validate() (so a member that
// sets shader_log_direction File with an empty shader_log_folder is rejected; whether an output
// path may be empty can also depend on the configuration the member is applied to, which
// SaveCustomSettings validates).
Expected<ImportResult, ImportError> ParseGameSettingsFile(std::string_view bytes);

// Reads the file (at most kMaxGameSettingsFileBytes; else Unreadable) and parses it.
Expected<ImportResult, ImportError> ReadGameSettingsFile(const std::filesystem::path& path);

// The member for a per-game import (Requirement 9.9).
struct GameImport {
	std::string            title; // the member's key
	GameConfigurationPatch patch;
	// True when the member's key is not the game's title ID: the file has exactly one member,
	// exported for another title, and the user must confirm applying it.
	bool needs_confirmation = false;

	friend bool operator==(const GameImport&, const GameImport&) = default;
};

// The member keyed by NormalizeTitleId(title_id); else, when the file has exactly one member, that
// member with needs_confirmation; else NoMatchingMember.
Expected<GameImport, ImportError> SelectGameImport(const ImportResult& result,
                                                   std::string_view    title_id);

// base with every field of the patch set to the patch's value and every other field kept. Storing
// the result with SaveCustomSettings (settingsStore.h) marks the game as custom and validates the
// whole configuration.
GameConfiguration ApplyPatch(const GameConfiguration& base, const GameConfigurationPatch& patch);

} // namespace kyty::core
