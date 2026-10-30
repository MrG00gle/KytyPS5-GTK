#include "launcher/core/dataPaths.h"

#include "launcher/core/compatibility.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string_view>
#include <sys/random.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

constexpr char kFlatpakMarker[]    = "/.flatpak-info";
constexpr char kKytyDirectory[]    = "Kyty";
constexpr char kSettingsFileName[] = "Kyty.ini";
constexpr char kProbePrefix[]      = ".kyty-write-probe-";

// Probe names tried before giving up with EEXIST.
constexpr int kMaxCreateAttempts = 100;

// Lexically normal, without a trailing separator ("/a/b/" is "/a/b"; "/" stays "/").
fs::path Normal(const fs::path& path) {
	fs::path normal = path.lexically_normal();
	if (normal.has_relative_path() && !normal.has_filename()) {
		normal = normal.parent_path();
	}
	return normal;
}

fs::path AbsoluteAgainst(const fs::path& path, const fs::path& base) {
	return Normal(path.is_absolute() ? path : base / path);
}

// QDir::homePath(): $HOME, or "/" when it is unset or empty.
fs::path HomeDirectory(const Environment& env, const fs::path& cwd) {
	const auto home = env.Get("HOME");
	if (!home || home->empty()) {
		return "/";
	}
	return AbsoluteAgainst(*home, cwd);
}

// QStandardPaths::writableLocation() of Qt 6 and the XDG Base Directory rule: $<name> when it is an
// absolute path, else home / fallback (an unset, empty, or relative value is ignored).
fs::path XdgDirectory(const Environment& env, std::string_view name, const fs::path& home,
                      const fs::path& fallback) {
	const auto value = env.Get(name);
	if (value && !value->empty() && value->front() == '/') {
		return Normal(*value);
	}
	return Normal(home / fallback);
}

std::function<int(WritableProbeStep)>& FaultHook() {
	static std::function<int(WritableProbeStep)> hook;
	return hook;
}

int InjectedFault(WritableProbeStep step) {
	const auto& hook = FaultHook();
	return hook ? hook(step) : 0;
}

Unexpected<std::string> Failure(const fs::path& dir, const std::string& step,
                                const std::string& reason) {
	return Unexpected<std::string>(dir.string() + ": cannot " + step + ": " + reason);
}

Unexpected<std::string> Failure(const fs::path& dir, const std::string& step, int error_number) {
	return Failure(dir, step, std::generic_category().message(error_number));
}

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

// Creates a new probe file in dir; the descriptor, or -1 with error_number and the last name tried.
int CreateProbe(const fs::path& dir, std::string& probe, int& error_number) {
	for (int attempt = 0; attempt < kMaxCreateAttempts; attempt++) {
		probe        = (dir / kProbePrefix).string();
		error_number = AppendRandomHex(probe);
		if (error_number != 0) {
			return -1;
		}
		error_number = InjectedFault(WritableProbeStep::Create);
		if (error_number == 0) {
			const int fd = ::open(probe.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
			if (fd >= 0) {
				return fd;
			}
			error_number = errno;
		}
		if (error_number != EEXIST) {
			return -1;
		}
	}
	return -1;
}

int WriteOneByte(int fd) {
	for (;;) {
		const ssize_t written = ::write(fd, "k", 1);
		if (written == 1) {
			return 0;
		}
		if (written < 0 && errno == EINTR) {
			continue;
		}
		return written < 0 ? errno : EIO;
	}
}

} // namespace

ResolvedPaths ResolvePaths(const Environment& env, const fs::path& launcher_cwd,
                           const fs::path& emulator_dir) {
	const fs::path cwd    = AbsoluteAgainst(launcher_cwd, "/");
	const fs::path home   = HomeDirectory(env, cwd);
	const fs::path config = XdgDirectory(env, "XDG_CONFIG_HOME", home, ".config");

	ResolvedPaths paths;
	paths.flatpak = env.FileExists(kFlatpakMarker);
	if (paths.flatpak) {
		paths.settings_file = Normal(config / kKytyDirectory / kSettingsFileName);
		paths.data_dir =
		    Normal(XdgDirectory(env, "XDG_DATA_HOME", home, ".local/share") / kKytyDirectory);
		return paths;
	}
	const fs::path portable = Normal(cwd / kSettingsFileName);
	paths.settings_file =
	    env.FileExists(portable) ? portable : Normal(config / kKytyDirectory / kSettingsFileName);
	paths.data_dir = AbsoluteAgainst(emulator_dir, cwd);
	return paths;
}

fs::path SettingsDirectory(const ResolvedPaths& paths) {
	return paths.settings_file.parent_path();
}

fs::path CompatibilityCachePath(const fs::path& data_dir) {
	return data_dir / kCompatibilityCacheFileName;
}

fs::path CheatDirectory(const fs::path& data_dir) {
	return data_dir / "_Patches";
}

fs::path SaveDataDirectory(const fs::path& data_dir) {
	return data_dir / "_SaveData";
}

fs::path SpawnWorkingDirectory(const ResolvedPaths& paths) {
	return paths.data_dir;
}

Expected<void, std::string> EnsureWritableDirectory(const fs::path& dir) {
	if (const int injected = InjectedFault(WritableProbeStep::CreateDirectories); injected != 0) {
		return Failure(dir, "create the directory", injected);
	}
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		return Failure(dir, "create the directory", ec.message());
	}

	std::string probe;
	int         error_number = 0;
	const int   fd           = CreateProbe(dir, probe, error_number);
	if (fd < 0) {
		return Failure(dir, "create the write probe " + probe, error_number);
	}

	error_number = InjectedFault(WritableProbeStep::Write);
	if (error_number == 0) {
		error_number = WriteOneByte(fd);
	}
	if (error_number != 0) {
		::close(fd);
		::unlink(probe.c_str());
		return Failure(dir, "write the write probe " + probe, error_number);
	}

	// The descriptor is released whatever the result (close() must not be retried on Linux).
	const int injected    = InjectedFault(WritableProbeStep::Close);
	const int closed      = ::close(fd);
	const int close_errno = errno;
	error_number          = injected != 0 ? injected : closed == 0 ? 0 : close_errno;
	if (error_number != 0) {
		::unlink(probe.c_str());
		return Failure(dir, "close the write probe " + probe, error_number);
	}

	error_number = InjectedFault(WritableProbeStep::Remove);
	if (error_number == 0 && ::unlink(probe.c_str()) != 0) {
		error_number = errno;
	}
	if (error_number != 0) {
		return Failure(dir, "remove the write probe " + probe, error_number);
	}
	return {};
}

void SetWritableProbeFaultForTesting(std::function<int(WritableProbeStep)> fault) {
	FaultHook() = std::move(fault);
}

} // namespace kyty::core
