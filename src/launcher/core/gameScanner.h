#pragma once

// Game_Folder scanning (Requirements 7.1, 7.2, 7.5, 7.6, 7.10, 7.15; design.md "Scan rules").

#include "launcher/core/cancelToken.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/scanFileSystem.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace kyty::core {

// A Game_Folder that is missing or a directory that cannot be listed.
struct ScanWarning {
	std::filesystem::path path;   // the path as the scan reached it (the folder, or folder/sub/dir)
	std::string           reason; // ScanFileSystem::List's error text

	friend bool operator==(const ScanWarning&, const ScanWarning&) = default;
};

struct ScanResult {
	std::vector<GameEntry>   entries;  // sorted by path (byte order of the native string)
	std::vector<ScanWarning> warnings; // sorted by path (byte order of the native string)
	// True when the token was cancelled during the scan; entries and warnings are then empty, so a
	// cancelled scan is discarded whole.
	bool cancelled = false;

	friend bool operator==(const ScanResult&, const ScanResult&) = default;
};

// Scans the folders breadth-first, one after the other in list order:
// - Every visited directory (the folder itself included) is listed once. Entries of type Symlink
//   are ignored entirely: never followed, never an eboot.bin, never a .zar.
// - In every visited directory, each regular file whose name ends with ".zar" (case-sensitive) is a
//   Game_Entry of kind Archive when ArchiveHasRootEboot accepts it.
// - A directory that lists a regular file named "eboot.bin" is a Game_Entry of kind Directory and
//   its subdirectories are not visited.
// - Entries are deduplicated by Canonical(); GameEntry::path is the canonical path, and
//   legacy_relative_path is relative to the first folder that found the entry. Metadata is read
//   once per entry. A candidate whose canonical path cannot be resolved is skipped with a warning.
// - Each missing folder or unreadable directory gives one warning per scan, deduplicated by its
//   canonical path (the lexically normal path when it has none) across overlapping folders.
// The token is checked before every directory listing, archive check, and metadata read; once it is
// cancelled the scan stops and returns {.cancelled = true}.
ScanResult ScanGameFolders(std::span<const std::filesystem::path> folders, ScanFileSystem& fs,
                           const MetadataReader& metadata, const CancelToken& cancel);

// The Game_Folders to store (Requirement 7.1): each non-empty string made absolute
// (std::filesystem::absolute), keeping the first occurrence of each directory in list order.
// Directories are compared by Canonical(), or by the lexically normal absolute path without a
// trailing separator when Canonical() fails (so a missing folder is kept, and warned about by the
// next scan). The result keeps the absolute, not the canonical, spelling of each first occurrence.
// A string that cannot be made absolute is dropped.
std::vector<std::filesystem::path> NormalizeGameFolders(std::span<const std::string> folders,
                                                        ScanFileSystem&              fs);

} // namespace kyty::core
