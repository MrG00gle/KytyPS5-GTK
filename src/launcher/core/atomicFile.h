#pragma once

// Crash-safe file replacement and bounded file reads on the host file system (Requirements 9.15,
// 12.8). Settings, caches, cheat files, and exports are written through WriteFileAtomically, so a
// failure at any point leaves the previous file byte-identical.

#include "launcher/core/expected.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace kyty::core {

// The step that failed, so that the GTK layer can say it in its own words (plan V1); message says
// the same in English for the log. The paths of the sentences are path, detail, and other.
enum class IoErrorKind {
	Open,            // "cannot open <path>"
	Read,            // "cannot read <path>"
	TooLarge,        // "cannot read <path> (limit <limit> bytes)"
	ReadLink,        // "cannot read the symbolic link <detail>"
	ResolveLinks,    // "cannot resolve <path>" (too many symbolic links)
	NoFileName,      // "cannot write <path>" (the target names no file)
	CreateTemp,      // "cannot create a temporary file in <detail>" (the target's directory)
	WriteTemp,       // "cannot write <detail>" (the temporary file)
	SyncTemp,        // "cannot sync <detail>"
	CloseTemp,       // "cannot close <detail>"
	Rename,          // "cannot rename <detail> to <other>"
	CreateDirectory, // "cannot create the directory <detail>" (SaveSettingsStore)
	NotLoaded,       // SaveSettingsStore refused to replace a file that could not be loaded
	NoTitleId,       // WriteGameSettingsFile for a game without a title ID
};

struct IoError {
	std::filesystem::path path;             // the file the operation was about
	int                   error_number = 0; // errno value
	std::string           message;          // names the failed step, the path, and the error text
	IoErrorKind           kind = IoErrorKind::Open;
	std::filesystem::path detail;    // the step's file or directory when it is not path
	std::filesystem::path other;     // Rename: the file the temporary file replaces
	uint64_t              limit = 0; // TooLarge: the byte limit

	friend bool operator==(const IoError&, const IoError&) = default;
};

// Replaces target with bytes: a temporary file .<name>.tmp-<16 random hexadecimal digits> in the
// target's directory is written in full, fsync()ed, closed, and rename()d over the target; then the
// directory is fsync()ed (a failure of that last step is ignored, because the new file is already
// in place). The temporary file is created with mode 0666, so a new target gets 0666 minus the
// process umask (and the directory's default ACL), as any file the process creates and as Qt's
// QSaveFile does; when the target exists, the new file gets the target's permission bits instead.
// A target that is a symbolic link is written through: the file the link resolves to is replaced
// and the link stays. On failure the temporary file is removed and the target is unchanged.
Expected<void, IoError> WriteFileAtomically(const std::filesystem::path& target,
                                            std::string_view             bytes);

// Reads a whole file of at most max_bytes bytes. A larger file is an error with EFBIG; a missing
// file gives ENOENT, a directory EISDIR.
Expected<std::string, IoError> ReadFileLimited(const std::filesystem::path& path,
                                               uint64_t                     max_bytes);

// The steps of WriteFileAtomically, for fault injection in tests.
enum class AtomicFileStep { CreateTemp, Write, Sync, Close, Rename };

// Tests only, not thread-safe: before each step, WriteFileAtomically calls fault(step); a non-zero
// return value is the errno that the step then fails with, without performing it (Close still
// releases the descriptor). An empty function removes the hook.
void SetAtomicFileFaultForTesting(std::function<int(AtomicFileStep)> fault);

} // namespace kyty::core
