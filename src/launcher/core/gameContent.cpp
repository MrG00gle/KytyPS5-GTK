#include "launcher/core/gameContent.h"

#include "common/archive.h"
#include "common/file.h"

#include <limits>
#include <memory>
#include <system_error>

namespace kyty::core {

namespace {

constexpr std::string_view kParamJson = "sce_sys/param.json";
constexpr std::string_view kIcon0     = "sce_sys/icon0.png";
constexpr std::string_view kPic0      = "sce_sys/pic0.png";
constexpr std::string_view kSnd0      = "sce_sys/snd0.at9";

// The size of a regular file, or nullopt when the path is missing or not a regular file.
std::optional<uint64_t> RegularFileSize(const std::filesystem::path& file, GameEntry::Kind kind) {
	if (kind == GameEntry::Kind::Archive) {
		const auto info = Common::GetArchiveInfo(file);
		if (!info || !info->is_file) {
			return std::nullopt;
		}
		return info->size;
	}
	std::error_code ec;
	const auto      status = std::filesystem::status(file, ec);
	if (ec || !std::filesystem::is_regular_file(status)) {
		return std::nullopt;
	}
	const auto size = std::filesystem::file_size(file, ec);
	if (ec) {
		return std::nullopt;
	}
	return size;
}

} // namespace

std::filesystem::path ResolveEntryFile(const std::filesystem::path& entry_path,
                                       GameEntry::Kind kind, std::string_view relative) {
	const std::filesystem::path member(relative);
	return kind == GameEntry::Kind::Archive ? Common::MakeArchivePath(entry_path, member)
	                                        : entry_path / member;
}

bool EntryFileExists(const std::filesystem::path& entry_path, GameEntry::Kind kind,
                     std::string_view relative) {
	return RegularFileSize(ResolveEntryFile(entry_path, kind, relative), kind).has_value();
}

std::optional<std::string> ReadEntryFile(const std::filesystem::path& entry_path,
                                         GameEntry::Kind kind, std::string_view relative,
                                         uint64_t max_bytes) {
	// Keep the archive index and block cache alive across the checks and the read, as the Qt
	// launcher did.
	const auto reader =
	    kind == GameEntry::Kind::Archive ? Common::OpenArchive(entry_path) : nullptr;
	if (kind == GameEntry::Kind::Archive && reader == nullptr) {
		return std::nullopt;
	}

	const auto path = ResolveEntryFile(entry_path, kind, relative);
	const auto size = RegularFileSize(path, kind);
	if (!size || *size > max_bytes) {
		return std::nullopt;
	}

	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return std::nullopt;
	}
	// The size of the open file decides; the file may have changed since the check above.
	const uint64_t open_size = file.Size();
	if (open_size > max_bytes || open_size > std::numeric_limits<uint32_t>::max()) {
		return std::nullopt;
	}
	std::string data(static_cast<size_t>(open_size), '\0');
	uint32_t    bytes_read = 0;
	file.Read(data.data(), static_cast<uint32_t>(open_size), &bytes_read);
	if (bytes_read != open_size) {
		return std::nullopt;
	}
	return data;
}

std::optional<std::string> ReadParamJson(const std::filesystem::path& entry_path,
                                         GameEntry::Kind              kind) {
	return ReadEntryFile(entry_path, kind, kParamJson, kMaxParamJsonBytes);
}

std::optional<std::string> ReadIcon0(const std::filesystem::path& entry_path,
                                     GameEntry::Kind              kind) {
	return ReadEntryFile(entry_path, kind, kIcon0, kMaxImageBytes);
}

std::optional<std::string> ReadPic0(const std::filesystem::path& entry_path, GameEntry::Kind kind) {
	return ReadEntryFile(entry_path, kind, kPic0, kMaxImageBytes);
}

std::optional<std::string> ReadSnd0(const std::filesystem::path& entry_path, GameEntry::Kind kind) {
	return ReadEntryFile(entry_path, kind, kSnd0, kMaxTitleSoundBytes);
}

} // namespace kyty::core
