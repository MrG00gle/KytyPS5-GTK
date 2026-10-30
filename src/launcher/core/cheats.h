#pragma once

// Cheat files (task 16.2; design.md, "Cheat file"; Requirements 11.4-11.7; plan D23): which games
// have cheats, where their cheat file is, and the ETAHen JSON documents in it. The Emulator reads
// that file with --game-patch (emulatorArguments.h). Ported from the Qt launcher's Cheats namespace
// (src/launcher/src/cheatFile.cpp) and the file handling of its cheats dialog
// (src/launcher/src/patchesDialog.cpp). The HEN Cheats Collection file list and downloads are in
// cheatCatalog.h and remoteCheats.h.
//
// Document format (design.md, "Cheat file"):
//   { "id": "PPSA01234", "name": "...", "version": "01.000", "process": "eboot.bin",
//     "credits": ["..."], "mods": [ { "name": "Infinite HP", "enabled": false,
//     "memory": [ { "offset": "0x1234", "on": "9090", "off": "0F85" } ] } ] }
// Every member, known or not, keeps its value and its place through a parse and a serialization
// (nlohmann::ordered_json); a repeated key keeps its first place and its last value.

#include "launcher/core/expected.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kyty::core {

// True when NormalizeTitleId(title_id) (titleId.h) is "PPSA" followed by exactly five ASCII
// digits: the games that get the cheats dialog (Requirement 11.4).
bool IsCheatTitleId(std::string_view title_id);

// The game's cheat file, CheatDirectory(data_dir) / (NormalizeTitleId(title_id) + ".json")
// (dataPaths.h: <data_dir>/_Patches/<TITLE_ID>.json, Requirement 11.5). An empty path when
// IsCheatTitleId(title_id) is false, as Qt's PlanPath, so no other title ID names a file.
std::filesystem::path CheatFilePath(const std::filesystem::path& data_dir,
                                    std::string_view             title_id);

// The largest cheat file that is read: a downloaded one (Requirement 11.17) and a local one (the
// Qt launcher read local files without a limit).
inline constexpr uint64_t kMaxCheatFileBytes = uint64_t {8} << 20u;

// The executable a downloaded cheat file must name in "process": the file name of the game's elf
// setting, or "eboot.bin" when the setting is empty (the Qt cheats dialog's rule).
std::string CheatProcessName(std::string_view elf);

// One cheat: an element of the document's "mods" array.
struct Cheat {
	std::string name;           // the mod's "name", never empty
	bool        enabled = true; // the mod's "enabled"; a mod without it is enabled, as Qt showed it
	// The mod's "memory" member as compact JSON, "" when the mod has none: what the cheat writes.
	// Only for display and comparison; SerializeCheatDocument writes the member as it was read.
	std::string memory;

	friend bool operator==(const Cheat&, const Cheat&) = default;
};

struct CheatDocument {
	// The "mods", in order. SerializeCheatDocument writes back only their enabled states (the
	// Local view's check boxes); the other fields describe the file as it was read.
	std::vector<Cheat> cheats;
	// String members for the remote preview, "" when absent or not a string; credits holds the
	// string elements of "credits" (Qt showed them joined by ", ", or "author" when there were
	// none).
	std::string              id;
	std::string              name;
	std::string              version;
	std::string              process;
	std::string              author;
	std::vector<std::string> credits;
	// The whole document as read, as compact JSON with every member in file order; the source of
	// SerializeCheatDocument and ValidateRemoteImport. Empty in a default-constructed document.
	std::string json;

	friend bool operator==(const CheatDocument&, const CheatDocument&) = default;
};

// Parses the bytes of a cheat file (Requirement 11.6). It fails when the bytes are not JSON, the
// top-level value is not an object, it has no "mods" array, or a mod is not an object, has a
// missing, empty, or non-string "name", or has an "enabled" that is not true or false (Qt's
// ValidateDocument). The error starts with file_name (the path or name the user knows the file
// by), then ": ", then the reason; for bytes that are not JSON the reason carries the parser's
// message.
Expected<CheatDocument, std::string> ParseCheatDocument(std::string_view bytes,
                                                        std::string_view file_name);

// The cheat file text for document: its json with the "enabled" member of mod i set to
// cheats[i].enabled for every i below both counts (a mod without "enabled" stays without it while
// its cheat is enabled), every other member unchanged and in place, indented by 4 spaces, ending
// with a line break. "" when json is not a JSON object (a default-constructed document).
std::string SerializeCheatDocument(const CheatDocument& document);

// Why a downloaded document must not be imported for the game, or nothing when it may (Requirement
// 11.17), ported unchanged from Qt's Cheats::ValidateImport: the document must be valid
// (ParseCheatDocument's rules); title_id must be a cheat title ID (IsCheatTitleId) equal to "id"
// ignoring letter case after TrimWhitespace(title_id); version must be non-empty and equal to
// "version"; process must be non-empty and equal to "process" ignoring letter case; there must be
// at least one mod and no "master" member; no mod may have "module_name"; every mod needs a
// non-empty "memory" array of objects with "offset" (hexadecimal, optionally 0x, 1-16 digits),
// "on" and "off" (pairs of hexadecimal digits, the same length), no "absolute" other than false, no
// "expected" other than "off" (ignoring letter case), and none of "section", "sectionName",
// "sectionProtection", "logicalOffset", "sectionOffset". The messages are Qt's.
std::optional<std::string> ValidateRemoteImport(const CheatDocument& document,
                                                std::string_view title_id, std::string_view version,
                                                std::string_view process);

// document with every cheat disabled: an import starts with every cheat off (Requirement 11.5).
CheatDocument WithAllDisabled(CheatDocument document);

enum class CheatSaveErrorKind {
	NeedsConfirmation, // the file exists and expected_bytes was nullopt: ask the user first
	ChangedOnDisk,     // the file is not what the caller read: reload it
	Failed             // the file could not be checked or written; message says why
};

struct CheatSaveError {
	CheatSaveErrorKind kind = CheatSaveErrorKind::Failed;
	std::string        message;

	friend bool operator==(const CheatSaveError&, const CheatSaveError&) = default;
};

// Writes SerializeCheatDocument(document) to path through WriteFileAtomically (atomicFile.h),
// creating its directory, only when the file is what the caller expects (plan D23):
// - expected_bytes nullopt: the file must not exist; an existing file is NeedsConfirmation, and
//   the UI asks the user, reads the file again, and calls again with those bytes (Requirement
//   11.5: an existing local cheat file is replaced only after the user confirms);
// - expected_bytes set: the file's bytes must equal it; other bytes or a missing file are
//   ChangedOnDisk (Qt's "Cheat file changed; reload the dialog.").
// Failed: an empty path or a document without JSON ("Invalid cheat file."), a file that cannot be
// read (or is larger than kMaxCheatFileBytes), a directory that cannot be created, or a failed
// write. On any error the file is unchanged.
Expected<void, CheatSaveError> SaveIfUnchanged(const std::filesystem::path&    path,
                                               const CheatDocument&            document,
                                               std::optional<std::string_view> expected_bytes);

// Imports a downloaded document for a game (Requirement 11.5; Property 32): SaveIfUnchanged of
// WithAllDisabled(document) to CheatFilePath(data_dir, title_id), so the file is
// <data_dir>/_Patches/<TITLE_ID>.json with the title ID upper-cased. Returns that path. The caller
// checks ValidateRemoteImport first and passes the local file's bytes as LoadLocalCheats read them
// (nullopt when there was no file). Failed "Unsupported title ID." for a title without cheats.
Expected<std::filesystem::path, CheatSaveError>
ImportRemoteCheats(const std::filesystem::path& data_dir, std::string_view title_id,
                   const CheatDocument&            document,
                   std::optional<std::string_view> expected_local_bytes);

// The local cheat file of a game, as the cheats dialog's Local view loads it (Requirement 11.6).
struct LocalCheatFile {
	std::filesystem::path path; // CheatFilePath; empty for a title without cheats
	// The file does not exist or was read: an import may replace it (Qt refused to import over a
	// file it could not read).
	bool readable = false;
	// The bytes read (also when they do not parse); nullopt when there is no file or it could not
	// be read. SaveIfUnchanged and ImportRemoteCheats take them as the expected bytes.
	std::optional<std::string> bytes;
	// The parsed file; nullopt (zero cheats listed) when there is no file or it does not parse.
	std::optional<CheatDocument> document;
	// The status line, Qt's texts: "Unsupported title ID.", "No local cheat file: <path>",
	// "Could not read cheat file: <reason>", the parse error (it names the path), or
	// "Loaded <n> cheat(s) from <path>.".
	std::string message;
};

// Reads (at most kMaxCheatFileBytes) and parses CheatFilePath(data_dir, title_id). It never writes:
// a file that does not parse stays as it is.
LocalCheatFile LoadLocalCheats(const std::filesystem::path& data_dir, std::string_view title_id);

} // namespace kyty::core
