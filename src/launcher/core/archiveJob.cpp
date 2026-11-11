#include "launcher/core/archiveJob.h"

#include "common/archive.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

// The highest percentage before the packer has returned: 100 means the archive is complete.
constexpr int kMaxRunningPercent = 99;

// Whether anything (a dangling symbolic link included) exists at path, or the error (CheckFailed)
// when that cannot be determined.
Expected<bool, ArchiveError> Exists(const fs::path& path) {
	std::error_code ec;
	const auto      type = fs::symlink_status(path, ec).type();
	if (type == fs::file_type::not_found) {
		return false;
	}
	if (ec) {
		return MakeUnexpected(ArchiveError {
		    ArchiveErrorKind::CheckFailed,
		    {},
		    path,
		    ec.value(),
		    path.string() + ": cannot check whether it exists: " + ec.message() + "."});
	}
	return true;
}

ArchiveError AlreadyExists(const fs::path& archive) {
	return ArchiveError {
	    ArchiveErrorKind::Exists, {}, archive, 0, archive.string() + " already exists."};
}

int PackPercent(std::uint64_t done, std::uint64_t total) {
	if (total == 0) {
		return 0;
	}
	if (done >= total) {
		return 100;
	}
	return static_cast<int>(static_cast<unsigned __int128>(done) * 100 / total);
}

bool DefaultPacker(const fs::path& directory, const fs::path& archive,
                   const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
	return Common::PackArchive(directory, archive, progress);
}

// "/games/Foo/" is "/games/Foo".
fs::path WithoutTrailingSeparator(const fs::path& path) {
	return path.has_relative_path() && !path.has_filename() ? path.parent_path() : path;
}

} // namespace

bool CanOpenArchive(const fs::path& archive) {
	return Common::OpenArchive(archive) != nullptr;
}

fs::path ArchivePathFor(const fs::path& game_dir) {
	fs::path archive = WithoutTrailingSeparator(game_dir);
	archive += ".zar";
	return archive;
}

ArchiveJob::ArchiveJob(fs::path source, fs::path archive, ArchivePacker packer,
                       ArchiveVerifier verifier)
    : m_source(std::move(source)), m_archive(std::move(archive)), m_packer(std::move(packer)),
      m_verifier(std::move(verifier)) {}

Expected<ArchiveJob, ArchiveError>
ArchiveJob::Prepare(const fs::path& game_dir, ArchivePacker packer, ArchiveVerifier verifier) {
	auto       archive = ArchivePathFor(game_dir);
	const auto exists  = Exists(archive);
	if (!exists) {
		return MakeUnexpected(exists.error());
	}
	if (*exists) {
		return MakeUnexpected(AlreadyExists(archive));
	}
	return ArchiveJob(WithoutTrailingSeparator(game_dir), std::move(archive),
	                  packer ? std::move(packer) : ArchivePacker(DefaultPacker),
	                  verifier ? std::move(verifier) : ArchiveVerifier(CanOpenArchive));
}

void ArchiveJob::Fail(ArchiveError error) {
	m_outcome      = ArchiveOutcome::Failed;
	m_error        = std::move(error);
	m_error.source = m_source;
}

void ArchiveJob::Run(const std::function<void(int percent)>& progress, const CancelToken& cancel) {
	if (m_outcome != ArchiveOutcome::Pending) {
		return;
	}
	const auto report = [&progress](int percent) {
		if (progress) {
			progress(percent);
		}
	};
	const std::string failure = "Could not compress " + m_source.string();

	report(0);
	// A job cancelled before it started must not touch the archive path, which a later job may
	// already own (as in the Qt launcher).
	if (cancel.IsCancelled()) {
		m_outcome = ArchiveOutcome::Cancelled;
		return;
	}
	// Only an archive path that is free now can be deleted after a failure: the packer created it.
	if (const auto exists = Exists(m_archive); !exists || *exists) {
		auto error    = exists ? AlreadyExists(m_archive) : exists.error();
		error.message = failure + ": " + error.message;
		Fail(std::move(error));
		return;
	}

	int        last          = 0;
	bool       stopped       = false;
	const auto pack_progress = [&](std::uint64_t done, std::uint64_t total) {
		if (cancel.IsCancelled()) {
			stopped = true;
			return false;
		}
		const int percent = std::min(PackPercent(done, total), kMaxRunningPercent);
		if (percent > last) {
			last = percent;
			report(percent);
		}
		return true;
	};
	const bool packed = m_packer(m_source, m_archive, pack_progress);
	// 100 and Completed only for an archive that can be opened: a "successful" pack of a directory
	// without file data cannot (CanOpenArchive).
	if (packed && m_verifier(m_archive)) {
		m_outcome = ArchiveOutcome::Completed;
		report(100);
		return;
	}

	// The packer may have left a partial or unreadable archive (Common::PackArchive removes its own
	// only after a failure). remove() deletes a file, a link, or an empty directory, and does
	// nothing when the path is free.
	std::error_code ec;
	std::filesystem::remove(m_archive, ec);
	if (ec) {
		Fail(ArchiveError {ArchiveErrorKind::RemoveFailed,
		                   {},
		                   m_archive,
		                   ec.value(),
		                   failure + ": cannot remove the partial archive " + m_archive.string() +
		                       ": " + ec.message() + "."});
		return;
	}
	if (stopped || cancel.IsCancelled()) {
		m_outcome = ArchiveOutcome::Cancelled;
	} else if (packed) {
		m_outcome = ArchiveOutcome::VerifyFailed;
	} else {
		Fail(ArchiveError {ArchiveErrorKind::CompressFailed, {}, m_archive, 0, failure + "."});
	}
}

} // namespace kyty::core
