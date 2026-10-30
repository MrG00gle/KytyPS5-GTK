#pragma once

// Save data removal (task 16.3; Requirement 11.13; plan D21): the save data of a game is
// <Data_Directory>/_SaveData/<TITLE_ID> (SaveDataDirectory in dataPaths.h), and removing it touches
// nothing outside that directory. Ported from the Qt launcher's remove_save_data
// (src/launcher/src/configurationListWidget.cpp), which looked in several roots; the launcher now
// uses the Data_Directory only.

#include "launcher/core/expected.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kyty::core {

// Removes single directory entries; tests inject failures through it.
class PathRemover {
public:
	virtual ~PathRemover() = default;

	// Removes path itself without following a symbolic link: a file, a symbolic link, or an empty
	// directory. A path that no longer exists counts as removed. Returns the error otherwise.
	virtual std::error_code Remove(const std::filesystem::path& path) = 0;
};

// std::filesystem::remove.
PathRemover& HostPathRemover();

// SaveDataDirectory(data_dir) / NormalizeTitleId(title_id), or an error naming the title ID when
// the normalized title ID is empty, ".", "..", or contains '/' or a NUL byte (it must be exactly
// one path component below _SaveData).
Expected<std::filesystem::path, std::string> SaveDataPath(const std::filesystem::path& data_dir,
                                                          std::string_view             title_id);

// The save data directories to list in the confirmation dialog: SaveDataPath when it is a
// directory (a symbolic link to a directory counts), else none (the Qt launcher then said "No save
// data folder found for this game."). Errors as SaveDataPath.
Expected<std::vector<std::filesystem::path>, std::string>
ListSaveData(const std::filesystem::path& data_dir, std::string_view title_id);

// A path that RemoveSaveData could not delete.
struct RemovalFailure {
	std::filesystem::path path;
	std::error_code       error;

	friend bool operator==(const RemovalFailure&, const RemovalFailure&) = default;
};

struct SaveDataRemoval {
	std::filesystem::path       directory; // SaveDataPath(data_dir, title_id)
	std::vector<RemovalFailure> failures;  // empty when everything was deleted
};

// Deletes SaveDataPath(data_dir, title_id) and everything below it through remover, deepest paths
// first (the entries of a directory in name order, then the directory). Symbolic links are removed,
// never followed, so nothing outside the directory changes; when the directory itself is a link,
// only the link is removed. Every path that could not be deleted is in failures, in the order of
// the attempts: a path the remover failed on, a directory that could not be listed (it is not
// removed), and each directory above them, which is then not empty. A missing directory is a
// success without failures. The title ID is checked as in SaveDataPath; when it is refused, nothing
// is removed and the remover is not called.
Expected<SaveDataRemoval, std::string> RemoveSaveData(const std::filesystem::path& data_dir,
                                                      std::string_view             title_id,
                                                      PathRemover& remover = HostPathRemover());

} // namespace kyty::core
