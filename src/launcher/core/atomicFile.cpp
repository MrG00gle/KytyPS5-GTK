#include "launcher/core/atomicFile.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

// The kernel limit for symbolic links in one path resolution (MAXSYMLINKS on Linux).
constexpr int kMaxSymlinkDepth = 40;

// Temporary names tried before giving up with EEXIST.
constexpr int kMaxCreateAttempts = 100;

// Appends 16 hexadecimal digits from getrandom(); 0 or the errno.
int AppendRandomHex(std::string& text) {
	std::array<unsigned char, 8> bytes {};
	size_t                       filled = 0;
	while (filled < bytes.size()) {
		const ssize_t count = ::getrandom(bytes.data() + filled, bytes.size() - filled, 0);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		filled += static_cast<size_t>(count);
	}
	static constexpr char kHex[] = "0123456789abcdef";
	for (const unsigned char byte: bytes) {
		text += kHex[byte >> 4u];
		text += kHex[byte & 0xFu];
	}
	return 0;
}

std::function<int(AtomicFileStep)>& FaultHook() {
	static std::function<int(AtomicFileStep)> hook;
	return hook;
}

int InjectedFault(AtomicFileStep step) {
	const auto& hook = FaultHook();
	return hook ? hook(step) : 0;
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

Unexpected<IoError> Failure(const fs::path& path, int error_number, const std::string& what,
                            IoErrorKind kind, fs::path detail = {}, fs::path other = {},
                            uint64_t limit = 0) {
	return Unexpected<IoError>(IoError {path, error_number, what + ": " + ErrorText(error_number),
	                                    kind, std::move(detail), std::move(other), limit});
}

// Follows target while it is a symbolic link. A relative link is resolved against the directory
// of the link. A path whose status cannot be read is returned as is; the later steps report it.
Expected<fs::path, IoError> ResolveLinks(const fs::path& target) {
	fs::path current = target;
	for (int depth = 0; depth <= kMaxSymlinkDepth; depth++) {
		std::error_code ec;
		const auto      status = fs::symlink_status(current, ec);
		if (ec || status.type() != fs::file_type::symlink) {
			return current;
		}
		fs::path link = fs::read_symlink(current, ec);
		if (ec) {
			return Failure(target, ec.value(), "cannot read the symbolic link " + current.string(),
			               IoErrorKind::ReadLink, current);
		}
		current = link.is_absolute() ? std::move(link) : current.parent_path() / link;
	}
	return Failure(target, ELOOP, "cannot resolve " + target.string(), IoErrorKind::ResolveLinks);
}

// Owns the temporary file until it has been renamed over the target.
class TempFile {
public:
	TempFile() = default;
	~TempFile() {
		if (m_fd >= 0) {
			::close(m_fd);
		}
		if (!m_path.empty()) {
			::unlink(m_path.c_str());
		}
	}
	TempFile(const TempFile&)            = delete;
	TempFile& operator=(const TempFile&) = delete;

	// Creates prefix + 16 random hexadecimal digits with O_EXCL and mode 0666, so that the kernel
	// applies the umask and the default ACL; a name that exists is tried again with another suffix.
	// Then, when mode is set (the existing target's permission bits), the file gets that mode.
	int Create(const std::string& prefix, std::optional<mode_t> mode) {
		int fd = -1;
		for (int attempt = 0; fd < 0; attempt++) {
			if (attempt == kMaxCreateAttempts) {
				return EEXIST;
			}
			std::string path = prefix;
			if (const int error_number = AppendRandomHex(path); error_number != 0) {
				return error_number;
			}
			fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
			if (fd >= 0) {
				m_path = std::move(path);
			} else if (errno != EEXIST) {
				return errno;
			}
		}
		m_fd = fd;
		if (mode && ::fchmod(m_fd, *mode) != 0) {
			return errno;
		}
		return 0;
	}

	int Write(std::string_view bytes) const {
		constexpr size_t kMaxWrite = size_t {1} << 30;
		while (!bytes.empty()) {
			const ssize_t written = ::write(m_fd, bytes.data(), std::min(bytes.size(), kMaxWrite));
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

	int Sync() const {
		while (::fsync(m_fd) != 0) {
			if (errno != EINTR) {
				return errno;
			}
		}
		return 0;
	}

	// Releases the descriptor whatever the result (close() must not be retried on Linux).
	int Close(int injected) {
		const int result       = ::close(m_fd);
		const int error_number = errno;
		m_fd                   = -1;
		if (injected != 0) {
			return injected;
		}
		return result == 0 ? 0 : error_number;
	}

	// Called once the file has been renamed: nothing is left to remove.
	void Release() { m_path.clear(); }

	[[nodiscard]] const std::string& Path() const { return m_path; }

private:
	int         m_fd = -1;
	std::string m_path;
};

// Best effort: makes the rename durable. Errors are ignored (the new file is already in place).
void SyncDirectory(const fs::path& directory) {
	const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		::fsync(fd);
		::close(fd);
	}
}

} // namespace

Expected<void, IoError> WriteFileAtomically(const fs::path& target, std::string_view bytes) {
	auto resolved = ResolveLinks(target);
	if (!resolved) {
		return Unexpected<IoError>(std::move(resolved).error());
	}
	const fs::path real = std::move(resolved).value();
	if (!real.has_filename()) {
		return Failure(target, EISDIR, "cannot write " + target.string(), IoErrorKind::NoFileName);
	}
	fs::path directory = real.parent_path();
	if (directory.empty()) {
		directory = ".";
	}

	// A new target keeps the mode that the kernel derives from 0666 and the umask.
	std::optional<mode_t> mode;
	struct stat           existing {};
	if (::stat(real.c_str(), &existing) == 0) {
		mode = existing.st_mode & 0777;
	}

	TempFile temp;
	int      error_number = InjectedFault(AtomicFileStep::CreateTemp);
	if (error_number == 0) {
		error_number =
		    temp.Create((directory / ("." + real.filename().string() + ".tmp-")).string(), mode);
	}
	if (error_number != 0) {
		return Failure(target, error_number,
		               "cannot create a temporary file in " + directory.string(),
		               IoErrorKind::CreateTemp, directory);
	}

	error_number = InjectedFault(AtomicFileStep::Write);
	if (error_number == 0) {
		error_number = temp.Write(bytes);
	}
	if (error_number != 0) {
		return Failure(target, error_number, "cannot write " + temp.Path(), IoErrorKind::WriteTemp,
		               temp.Path());
	}

	error_number = InjectedFault(AtomicFileStep::Sync);
	if (error_number == 0) {
		error_number = temp.Sync();
	}
	if (error_number != 0) {
		return Failure(target, error_number, "cannot sync " + temp.Path(), IoErrorKind::SyncTemp,
		               temp.Path());
	}

	error_number = temp.Close(InjectedFault(AtomicFileStep::Close));
	if (error_number != 0) {
		return Failure(target, error_number, "cannot close " + temp.Path(), IoErrorKind::CloseTemp,
		               temp.Path());
	}

	error_number = InjectedFault(AtomicFileStep::Rename);
	if (error_number == 0 && ::rename(temp.Path().c_str(), real.c_str()) != 0) {
		error_number = errno;
	}
	if (error_number != 0) {
		return Failure(target, error_number,
		               "cannot rename " + temp.Path() + " to " + real.string(), IoErrorKind::Rename,
		               temp.Path(), real);
	}
	temp.Release();

	SyncDirectory(directory);
	return {};
}

Expected<std::string, IoError> ReadFileLimited(const fs::path& path, uint64_t max_bytes) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return Failure(path, errno, "cannot open " + path.string(), IoErrorKind::Open);
	}
	// Closes fd on every return.
	struct Descriptor {
		int fd;
		~Descriptor() { ::close(fd); }
	} descriptor {fd};

	struct stat info {};
	if (::fstat(fd, &info) != 0) {
		return Failure(path, errno, "cannot read " + path.string(), IoErrorKind::Read);
	}
	if (S_ISDIR(info.st_mode)) {
		return Failure(path, EISDIR, "cannot read " + path.string(), IoErrorKind::Read);
	}
	const std::string too_large =
	    "cannot read " + path.string() + " (limit " + std::to_string(max_bytes) + " bytes)";
	const auto too_large_failure = [&] {
		return Failure(path, EFBIG, too_large, IoErrorKind::TooLarge, {}, {}, max_bytes);
	};
	std::string bytes;
	if (S_ISREG(info.st_mode)) {
		const auto size = static_cast<uint64_t>(info.st_size);
		if (size > max_bytes) {
			return too_large_failure();
		}
		bytes.reserve(static_cast<size_t>(size));
	}

	std::array<char, 65536> buffer {};
	for (;;) {
		const ssize_t count = ::read(fd, buffer.data(), buffer.size());
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			return Failure(path, errno, "cannot read " + path.string(), IoErrorKind::Read);
		}
		if (count == 0) {
			break;
		}
		// The file may have grown since fstat(), and non-regular files have no size.
		if (static_cast<uint64_t>(count) > max_bytes - bytes.size()) {
			return too_large_failure();
		}
		bytes.append(buffer.data(), static_cast<size_t>(count));
	}
	return bytes;
}

void SetAtomicFileFaultForTesting(std::function<int(AtomicFileStep)> fault) {
	FaultHook() = std::move(fault);
}

} // namespace kyty::core
