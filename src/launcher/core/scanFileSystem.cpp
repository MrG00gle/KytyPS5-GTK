#include "launcher/core/scanFileSystem.h"

#include "common/archive.h"

#include <system_error>
#include <utility>

namespace kyty::core {

namespace {

FsEntry::Type TypeOf(const std::filesystem::file_status& status) {
	switch (status.type()) {
		case std::filesystem::file_type::symlink: return FsEntry::Symlink;
		case std::filesystem::file_type::directory: return FsEntry::Directory;
		case std::filesystem::file_type::regular: return FsEntry::File;
		default: return FsEntry::Other;
	}
}

class HostScanFileSystem final: public ScanFileSystem {
public:
	Expected<std::vector<FsEntry>, std::string> List(const std::filesystem::path& dir) override {
		std::error_code                     ec;
		std::filesystem::directory_iterator it(dir, ec);
		if (ec) {
			return MakeUnexpected(ec.message());
		}
		std::vector<FsEntry> entries;
		for (const std::filesystem::directory_iterator end; it != end;) {
			FsEntry entry;
			entry.name = it->path().filename().native();
			// symlink_status() reports a link as a link; a failure (the entry vanished) is Other.
			std::error_code status_ec;
			const auto      status = it->symlink_status(status_ec);
			entry.type             = status_ec ? FsEntry::Other : TypeOf(status);
			entries.push_back(std::move(entry));
			it.increment(ec);
			if (ec) {
				return MakeUnexpected(ec.message());
			}
		}
		return entries;
	}

	bool ArchiveHasRootEboot(const std::filesystem::path& zar) override {
		// Hold the reader so that the lookup below reuses the opened index.
		const auto reader = Common::OpenArchive(zar);
		if (reader == nullptr) {
			return false;
		}
		const auto info = Common::GetArchiveInfo(Common::MakeArchivePath(zar, "eboot.bin"));
		return info.has_value() && info->is_file;
	}

	std::optional<std::filesystem::path> Canonical(const std::filesystem::path& path) override {
		std::error_code ec;
		auto            canonical = std::filesystem::canonical(path, ec);
		if (ec) {
			return std::nullopt;
		}
		return canonical;
	}
};

} // namespace

std::unique_ptr<ScanFileSystem> MakeHostScanFileSystem() {
	return std::make_unique<HostScanFileSystem>();
}

} // namespace kyty::core
