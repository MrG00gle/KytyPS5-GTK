#include "launcher/core/gameScanner.h"

#include "common/archive.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

constexpr std::string_view kEbootName   = "eboot.bin";
constexpr std::string_view kNoCanonical = "cannot resolve the canonical path";

// The Qt launcher's archive predicate: an ASCII case-insensitive ".zar" suffix.
bool IsArchiveName(const std::string& name) {
	return Common::IsSupportedArchive(std::filesystem::path(name));
}

// The lexically normal path without a trailing separator ("/a/./b/" gives "/a/b").
std::filesystem::path LexicalKey(const std::filesystem::path& path) {
	auto normal = path.lexically_normal();
	if (!normal.has_filename() && normal.has_relative_path()) {
		normal = normal.parent_path();
	}
	return normal;
}

// path relative to folder with '/' separators. path is always folder / <names>, so the relative
// path is exactly those names ("." for the folder itself).
std::string RelativeTo(const std::filesystem::path& path, const std::filesystem::path& folder) {
	const auto relative = path.lexically_relative(folder);
	return relative.empty() ? path.generic_string() : relative.generic_string();
}

class Scanner {
public:
	Scanner(ScanFileSystem& fs, const MetadataReader& metadata, const CancelToken& cancel)
	    : m_fs(fs), m_metadata(metadata), m_cancel(cancel) {}

	// Walks one folder breadth-first; false when the scan was cancelled.
	bool ScanFolder(const std::filesystem::path& folder) {
		std::deque<std::filesystem::path> pending {folder};
		while (!pending.empty()) {
			if (m_cancel.IsCancelled()) {
				return false;
			}
			const auto directory = std::move(pending.front());
			pending.pop_front();

			// A directory listed before in this scan (an overlapping folder, an alias, or a bind
			// mount, possibly a cycle) is skipped; one without an identity is listed every time.
			const auto identity = m_fs.Identity(directory);
			if (identity && m_listed.contains(*identity)) {
				continue;
			}
			auto listing = m_fs.List(directory);
			if (!listing) {
				Warn(directory, listing.error());
				continue;
			}
			if (identity) {
				m_listed.insert(*identity);
			}

			bool                               has_eboot = false;
			std::vector<std::filesystem::path> archives;
			std::vector<std::filesystem::path> subdirectories;
			for (const auto& entry: *listing) {
				// Symbolic links are never followed into directories and never count as a .zar.
				if (entry.type == FsEntry::File) {
					if (entry.name == kEbootName) {
						has_eboot = true;
					} else if (IsArchiveName(entry.name)) {
						archives.push_back(directory / entry.name);
					}
				} else if (entry.type == FsEntry::Directory) {
					subdirectories.push_back(directory / entry.name);
				} else if (entry.type == FsEntry::Symlink && entry.name == kEbootName &&
				           !has_eboot) {
					// An eboot.bin link counts when it leads to a regular file.
					has_eboot = m_fs.ResolvesToRegularFile(directory / entry.name);
				}
			}

			for (const auto& archive: archives) {
				if (!AddCandidate(archive, GameEntry::Kind::Archive, folder)) {
					return false;
				}
			}
			if (has_eboot) {
				// A game directory: its subdirectories are not visited.
				if (!AddCandidate(directory, GameEntry::Kind::Directory, folder)) {
					return false;
				}
				continue;
			}
			// Name order, not listing order: when a bind mount shows one directory under two
			// spellings, the one kept does not depend on the enumeration order (Requirement 7.5).
			std::sort(subdirectories.begin(), subdirectories.end(),
			          [](const std::filesystem::path& a, const std::filesystem::path& b) {
				          return a.native() < b.native();
			          });
			for (auto& subdirectory: subdirectories) {
				pending.push_back(std::move(subdirectory));
			}
		}
		return true;
	}

	ScanResult Result() && {
		ScanResult result;
		result.entries.reserve(m_entries.size());
		for (auto& [key, entry]: m_entries) {
			result.entries.push_back(std::move(entry));
		}
		result.warnings.reserve(m_warnings.size());
		for (auto& [key, warning]: m_warnings) {
			result.warnings.push_back(std::move(warning));
		}
		std::sort(result.warnings.begin(), result.warnings.end(),
		          [](const ScanWarning& a, const ScanWarning& b) {
			          return a.path.native() < b.path.native();
		          });
		return result;
	}

private:
	// One warning per path and scan: the key is the canonical path, or the lexical one.
	void Warn(const std::filesystem::path& path, std::string reason,
	          ScanWarningKind kind = ScanWarningKind::Unreadable) {
		const auto canonical = m_fs.Canonical(path);
		auto       key       = canonical ? canonical->native() : LexicalKey(path).native();
		m_warnings.try_emplace(std::move(key), ScanWarning {path, std::move(reason), kind});
	}

	// Adds the entry at path unless its canonical path is known; false when cancelled.
	bool AddCandidate(const std::filesystem::path& path, GameEntry::Kind kind,
	                  const std::filesystem::path& folder) {
		if (m_cancel.IsCancelled()) {
			return false;
		}
		const auto canonical = m_fs.Canonical(path);
		if (!canonical) {
			Warn(path, std::string(kNoCanonical), ScanWarningKind::NoCanonicalPath);
			return true;
		}
		const auto& key = canonical->native();
		if (m_entries.contains(key) || m_rejected.contains(key)) {
			return true;
		}
		if (kind == GameEntry::Kind::Archive && !m_fs.ArchiveHasRootEboot(path)) {
			m_rejected.insert(key);
			return true;
		}
		if (m_cancel.IsCancelled()) {
			return false;
		}

		auto      metadata = m_metadata.Read(*canonical, kind);
		GameEntry entry;
		entry.path                 = *canonical;
		entry.kind                 = kind;
		entry.title_name           = std::move(metadata.title_name);
		entry.title_id             = std::move(metadata.title_id);
		entry.game_version         = std::move(metadata.game_version);
		entry.firmware_version     = std::move(metadata.firmware_version);
		entry.metadata_available   = metadata.metadata_available;
		entry.legacy_relative_path = RelativeTo(path, folder);
		m_entries.emplace(key, std::move(entry));
		return true;
	}

	ScanFileSystem&       m_fs;
	const MetadataReader& m_metadata;
	const CancelToken&    m_cancel;

	// Keyed by the native string, so the order is the byte order the result promises.
	std::map<std::string, GameEntry>   m_entries;
	std::set<std::string>              m_rejected; // archives without a root eboot.bin
	std::map<std::string, ScanWarning> m_warnings;
	std::set<FileIdentity>             m_listed; // directories listed in this scan
};

} // namespace

ScanResult ScanGameFolders(std::span<const std::filesystem::path> folders, ScanFileSystem& fs,
                           const MetadataReader& metadata, const CancelToken& cancel) {
	ScanResult cancelled;
	cancelled.cancelled = true;

	Scanner scanner(fs, metadata, cancel);
	for (const auto& folder: folders) {
		if (!scanner.ScanFolder(folder)) {
			return cancelled;
		}
	}
	if (cancel.IsCancelled()) {
		return cancelled;
	}
	return std::move(scanner).Result();
}

std::vector<std::filesystem::path> NormalizeGameFolders(std::span<const std::string> folders,
                                                        ScanFileSystem&              fs) {
	std::vector<std::filesystem::path> result;
	std::set<std::string>              seen;
	for (const auto& text: folders) {
		if (text.empty()) {
			continue;
		}
		std::error_code ec;
		auto            absolute = std::filesystem::absolute(std::filesystem::path(text), ec);
		if (ec || absolute.empty()) {
			continue;
		}
		const auto canonical = fs.Canonical(absolute);
		auto       key       = canonical ? canonical->native() : LexicalKey(absolute).native();
		if (seen.insert(std::move(key)).second) {
			result.push_back(std::move(absolute));
		}
	}
	return result;
}

} // namespace kyty::core
