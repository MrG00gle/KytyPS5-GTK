#include "updater/core/updateResult.h"

#include <array>
#include <cerrno>
#include <cstdlib> // mkostemp
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace kyty::updater {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSuccessLine        = "success\n";
constexpr std::string_view kFailureLine        = "failure\n";
constexpr size_t           kMaxResultFileBytes = kSuccessLine.size() + kMaxUpdateResultMessageBytes;

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

// The first kMaxUpdateResultMessageBytes bytes of message. A UTF-8 sequence has at most three
// continuation bytes (10xxxxxx), so backing off over at most three of them drops the character
// that crosses the limit whole.
std::string_view CutMessage(std::string_view message) {
	if (message.size() <= kMaxUpdateResultMessageBytes) {
		return message;
	}
	size_t cut = kMaxUpdateResultMessageBytes;
	for (int i = 0; i < 3 && (static_cast<unsigned char>(message[cut]) & 0xC0u) == 0x80u; i++) {
		cut--;
	}
	return message.substr(0, cut);
}

// 0 or the errno of the first failed write.
int WriteAll(int fd, std::string_view bytes) {
	while (!bytes.empty()) {
		const ssize_t written = ::write(fd, bytes.data(), bytes.size());
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		if (written == 0) {
			return EIO;
		}
		bytes.remove_prefix(static_cast<size_t>(written));
	}
	return 0;
}

// Deletes path itself (a link, not its target); an empty directory with rmdir().
void RemovePath(const fs::path& path) {
	if (::unlink(path.c_str()) != 0 && errno == EISDIR) {
		::rmdir(path.c_str());
	}
}

PreviousResult Unreadable(const std::string& reason) {
	return PreviousResult {false, "The previous update result could not be read: " + reason};
}

} // namespace

kyty::core::Expected<void, std::string> WriteUpdateResult(const fs::path&       install_dir,
                                                          const PreviousResult& result) {
	const fs::path destination = install_dir / kUpdateResultFileName;
	const auto     failure     = [&](const std::string& what, int error_number) {
		return kyty::core::MakeUnexpected("Cannot write " + destination.string() + ": " + what +
		                                  ": " + ErrorText(error_number));
	};

	std::string bytes(result.success ? kSuccessLine : kFailureLine);
	bytes += CutMessage(result.message);

	std::string temporary =
	    (install_dir / (std::string(kUpdateResultFileName) + ".XXXXXX")).string();
	const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
	if (fd < 0) {
		return failure("cannot create a temporary file in " + install_dir.string(), errno);
	}
	int error_number = WriteAll(fd, bytes);
	if (::close(fd) != 0 && error_number == 0) {
		error_number = errno;
	}
	if (error_number != 0) {
		::unlink(temporary.c_str());
		return failure("cannot write " + temporary, error_number);
	}

	if (::rename(temporary.c_str(), destination.c_str()) != 0) {
		error_number = errno;
		// rename() cannot replace a directory with a file: an empty one is removed first.
		if (error_number == EISDIR && ::rmdir(destination.c_str()) == 0) {
			error_number = ::rename(temporary.c_str(), destination.c_str()) == 0 ? 0 : errno;
		}
	}
	if (error_number != 0) {
		::unlink(temporary.c_str());
		return failure("cannot rename " + temporary, error_number);
	}
	return {};
}

std::optional<PreviousResult> ReadAndClearPreviousResult(const fs::path& install_dir) {
	const fs::path path = install_dir / kUpdateResultFileName;
	// O_NONBLOCK: opening a FIFO there must not wait for a writer.
	const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		const int error_number = errno;
		if (error_number == ENOENT || error_number == ENOTDIR) {
			return std::nullopt;
		}
		RemovePath(path);
		return Unreadable(error_number == ELOOP ? "it is a symbolic link"
		                                        : ErrorText(error_number));
	}

	struct stat info {};
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		::close(fd);
		RemovePath(path);
		return Unreadable("it is not a regular file");
	}
	std::string             bytes;
	int                     error_number = 0;
	std::array<char, 16384> buffer {};
	while (bytes.size() <= kMaxResultFileBytes) {
		const ssize_t count = ::read(fd, buffer.data(), buffer.size());
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			error_number = errno;
			break;
		}
		if (count == 0) {
			break;
		}
		bytes.append(buffer.data(), static_cast<size_t>(count));
	}
	::close(fd);
	RemovePath(path);

	if (error_number != 0) {
		return Unreadable(ErrorText(error_number));
	}
	if (bytes.size() > kMaxResultFileBytes) {
		return Unreadable("it is larger than " + std::to_string(kMaxResultFileBytes) + " bytes");
	}
	const std::string_view text = bytes;
	if (text.starts_with(kSuccessLine)) {
		return PreviousResult {true, std::string(text.substr(kSuccessLine.size()))};
	}
	if (text.starts_with(kFailureLine)) {
		return PreviousResult {false, std::string(text.substr(kFailureLine.size()))};
	}
	return Unreadable("its first line is not \"success\" or \"failure\"");
}

} // namespace kyty::updater
