#pragma once

// Game_Entry data and param.json metadata (Requirements 7.8 and 7.9). Ported from the Qt launcher's
// GetGameMetadata() and SetGameFiles() (src/launcher/src/configurationListWidget.cpp).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace kyty::core {

// One game found by the scanner (design.md, "Data Models").
struct GameEntry {
	// Absolute and canonical (the scanner resolves it); the game directory, or the .zar file.
	std::filesystem::path path;
	enum class Kind { Directory, Archive } kind = Kind::Directory;
	// Localized titleName; fallback: the directory name or the .zar stem (7.9).
	std::string title_name;
	// titleId, or the contentId[7..16) fallback; may be empty.
	std::string title_id;
	// appVersion, else contentVersion.
	std::string game_version;
	// "M.mm" or "M.mm.pp" (7.8); empty when unparsable.
	std::string firmware_version;
	// False when param.json is missing, larger than 1 MiB, or not a JSON object.
	bool metadata_available = false;
	// The path relative to the first Game_Folder (in list order) that contains the entry, with '/'
	// separators ("." for the folder itself); used to migrate the Qt launcher's per-game entries.
	std::string legacy_relative_path;

	friend bool operator==(const GameEntry&, const GameEntry&) = default;
};

// What sce_sys/param.json says about a game.
struct GameMetadata {
	std::string title_name;
	std::string title_id;
	std::string game_version;
	std::string firmware_version;
	bool        metadata_available = false;

	friend bool operator==(const GameMetadata&, const GameMetadata&) = default;
};

// The largest param.json that is read (Requirement 7.9): exactly 1 MiB is accepted.
inline constexpr uint64_t kMaxParamJsonBytes = uint64_t {1} << 20u;

// The title used when the metadata has none: the directory name for a directory, and the file name
// without its extension for an archive (std::filesystem::path::stem(), as the Qt launcher's
// QFileInfo::completeBaseName(): "Game.zar" gives "Game", "a.b.zar" gives "a.b"; a file named just
// ".zar" keeps its name). A trailing separator is ignored; a path without any file name ("/") gives
// its generic string.
std::string FallbackTitle(const std::filesystem::path& entry_path, GameEntry::Kind kind);

// Parses the bytes of a param.json. Unusable bytes (more than kMaxParamJsonBytes, not JSON, or a
// root that is not a JSON object) give fallback_title as title, empty other fields, and
// metadata_available = false. Otherwise metadata_available is true and, with every string value
// trimmed (TrimWhitespace) and non-string values ignored:
// - title_name: localizedParameters[defaultLanguage].titleName, else
//   localizedParameters["en-US"].titleName, else the first non-empty titleName of the
//   localizedParameters members in key order, else fallback_title;
// - title_id: titleId, else bytes [7, 16) of contentId when contentId is 36 bytes long with '-' at
//   index 6 and '_' at index 16 ("UP0000-PPSA01234_00-..."), else "";
// - game_version: appVersion, else contentVersion;
// - firmware_version: FormatSystemSoftwareVersion(requiredSystemSoftwareVersion).
GameMetadata ParseParamJson(std::string_view bytes, std::string_view fallback_title);

// Reads the metadata of a Game_Entry; the scanner calls it once per new entry. Implementations
// must be safe to call from the scanning thread.
class MetadataReader {
public:
	virtual ~MetadataReader() = default;

	[[nodiscard]] virtual GameMetadata Read(const std::filesystem::path& entry_path,
	                                        GameEntry::Kind              kind) const = 0;
};

// Reads sce_sys/param.json from the game directory or the .zar archive (ReadParamJson in
// gameContent.h) and parses it with ParseParamJson and FallbackTitle. A missing, unreadable, or
// too large file gives the fallback metadata.
std::unique_ptr<MetadataReader> MakeHostMetadataReader();

} // namespace kyty::core
