#pragma once

// Compression of a directory Game_Entry into a ZAR archive (task 16.3; design.md, "archiveJob.h";
// Requirements 11.8, 11.9, 11.10, 11.16; plan D22), ported from the Qt launcher's compress_game
// (src/launcher/src/configurationListWidget.cpp). The launcher runs Run on a worker thread and
// shows the percentages it reports; deleting the original directory afterwards and the rescan
// (Requirement 11.11) are the caller's, and only after Completed.

#include "launcher/core/cancelToken.h"
#include "launcher/core/expected.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace kyty::core {

// Packs directory into a new archive, as Common::PackArchive: progress(done, total) reports the
// bytes packed so far and returns false to cancel; the result is true on success.
using ArchivePacker = std::function<bool(
    const std::filesystem::path& directory, const std::filesystem::path& archive,
    const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress)>;

// True when the archive the packer wrote can be opened for reading.
using ArchiveVerifier = std::function<bool(const std::filesystem::path& archive)>;

// The default ArchiveVerifier: Common::OpenArchive(archive) succeeds. Common::PackArchive reports
// success for a directory without a single byte of file data (only empty directories or zero-byte
// files), but ZArchive cannot open the archive it writes then (orchestrator decision, option 2).
bool CanOpenArchive(const std::filesystem::path& archive);

enum class ArchiveOutcome {
	Pending,   // Run has not finished yet
	Completed, // the archive is complete and can be opened
	Cancelled, // the user cancelled; the partial archive is deleted and there is no error text
	Failed,    // Error() names the source directory; the partial archive is deleted
	// The packer reported success, but the verifier cannot open the archive; the archive is
	// deleted and the source is untouched. Error() is empty: the UI names Source() in its own
	// translated text ("Could not compress <folder>.", phase 6). The Qt launcher reported success.
	VerifyFailed,
};

// <game_dir>.zar: the game directory's path with ".zar" appended (a trailing separator is
// ignored), next to the directory (Requirement 11.8).
std::filesystem::path ArchivePathFor(const std::filesystem::path& game_dir);

class ArchiveJob {
public:
	// A job that packs game_dir into ArchivePathFor(game_dir) with packer (Common::PackArchive when
	// it is empty) and checks the result with verifier (CanOpenArchive when it is empty). Fails,
	// starting nothing, with "<archive> already exists." when anything exists at the archive path
	// (std::filesystem::symlink_status, so a dangling symbolic link counts; Requirement 11.9), or
	// with "<archive>: cannot check whether it exists: <reason>." when that cannot be determined.
	static Expected<ArchiveJob, std::string> Prepare(const std::filesystem::path& game_dir,
	                                                 ArchivePacker                packer   = {},
	                                                 ArchiveVerifier              verifier = {});

	// Packs the directory; call it once, on any thread. progress (may be empty) gets 0 first, then
	// floor(100 * done / total) of the packer's progress calls whenever that is higher than the
	// last value, at most 99, and 100 only after the packer succeeded and the verifier opened the
	// archive, so the values never decrease (Requirement 11.8) and 100 means Completed. A cancel
	// requested before the packer starts gives Cancelled without calling the packer; during the
	// pack the next progress call returns false. The verifier runs only after the packer
	// succeeded. When the packer fails or is cancelled, or the verifier refuses the archive, what
	// the packer left at the archive path is deleted (Requirements 11.10, 11.16) and the outcome is
	// Cancelled when cancel was requested, else VerifyFailed after a refused archive, else Failed
	// with "Could not compress <directory>.". If something exists at the archive path when Run
	// starts, the packer is not called and the outcome is Failed with "Could not compress
	// <directory>: <archive> already exists.", the existing file untouched. If the archive cannot
	// be deleted, the outcome is Failed with "Could not compress <directory>: cannot remove the
	// partial archive <archive>: <reason>.". A packer that succeeds after a late cancel request
	// keeps its archive when it can be opened (Completed), as in the Qt launcher.
	void Run(const std::function<void(int percent)>& progress, const CancelToken& cancel);

	[[nodiscard]] ArchiveOutcome               Outcome() const { return m_outcome; }
	[[nodiscard]] const std::string&           Error() const { return m_error; } // Failed only
	[[nodiscard]] const std::filesystem::path& Source() const { return m_source; }
	[[nodiscard]] const std::filesystem::path& Archive() const { return m_archive; }

private:
	ArchiveJob(std::filesystem::path source, std::filesystem::path archive, ArchivePacker packer,
	           ArchiveVerifier verifier);

	void Fail(std::string error);

	std::filesystem::path m_source;
	std::filesystem::path m_archive;
	ArchivePacker         m_packer;
	ArchiveVerifier       m_verifier;
	ArchiveOutcome        m_outcome = ArchiveOutcome::Pending;
	std::string           m_error;
};

} // namespace kyty::core
