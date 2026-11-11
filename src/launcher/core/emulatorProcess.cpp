#include "launcher/core/emulatorProcess.h"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::core {

namespace {

namespace fs = std::filesystem;

std::function<int(SpawnStep)>& FaultHook() {
	static std::function<int(SpawnStep)> hook;
	return hook;
}

int InjectedFault(SpawnStep step) {
	const auto& hook = FaultHook();
	return hook ? hook(step) : 0;
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

Unexpected<SpawnError> Failure(SpawnErrorKind kind, const fs::path& executable, int error_number,
                               std::string reason, SpawnCause cause = SpawnCause::ErrorNumber,
                               std::size_t argument = 0, fs::path cwd = {}) {
	SpawnError error;
	error.kind         = kind;
	error.executable   = executable;
	error.error_number = error_number;
	error.reason       = std::move(reason);
	error.message      = "Cannot start " + executable.string() + ": " + error.reason;
	error.cause        = cause;
	error.argument     = argument;
	error.cwd          = std::move(cwd);
	return Unexpected<SpawnError>(std::move(error));
}

// Closes the descriptor on destruction unless it was released.
class Descriptor {
public:
	Descriptor() = default;
	~Descriptor() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	Descriptor(const Descriptor&)            = delete;
	Descriptor& operator=(const Descriptor&) = delete;

	int Release() { return std::exchange(fd, -1); }

	int fd = -1;
};

// pipe2(O_NONBLOCK | O_CLOEXEC), then the write end, which the child gets as its standard output
// or error, goes back to blocking: O_NONBLOCK belongs to the open file description, which the child
// shares, and the Emulator expects blocking writes.
int OpenPipe(Descriptor& read_end, Descriptor& write_end) {
	if (const int injected = InjectedFault(SpawnStep::Pipe); injected != 0) {
		return injected;
	}
	int fds[2] = {-1, -1};
	if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
		return errno;
	}
	read_end.fd     = fds[0];
	write_end.fd    = fds[1];
	const int flags = ::fcntl(write_end.fd, F_GETFL);
	if (flags < 0 || ::fcntl(write_end.fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
		return errno;
	}
	return 0;
}

class FileActions {
public:
	FileActions() = default;
	~FileActions() {
		if (m_initialized) {
			::posix_spawn_file_actions_destroy(&m_actions);
		}
	}
	FileActions(const FileActions&)            = delete;
	FileActions& operator=(const FileActions&) = delete;

	// 0 or the error number.
	int Init(int stdout_fd, int stderr_fd, const fs::path& cwd) {
		if (const int injected = InjectedFault(SpawnStep::FileActions); injected != 0) {
			return injected;
		}
		if (const int result = ::posix_spawn_file_actions_init(&m_actions); result != 0) {
			return result;
		}
		m_initialized = true;
		int result    = ::posix_spawn_file_actions_adddup2(&m_actions, stdout_fd, STDOUT_FILENO);
		if (result == 0) {
			result = ::posix_spawn_file_actions_adddup2(&m_actions, stderr_fd, STDERR_FILENO);
		}
		if (result == 0) {
			result = ::posix_spawn_file_actions_addopen(&m_actions, STDIN_FILENO, "/dev/null",
			                                            O_RDONLY, 0);
		}
		if (result == 0) {
			result = ::posix_spawn_file_actions_addclosefrom_np(&m_actions, 3);
		}
		if (result == 0 && !cwd.empty()) {
			result = ::posix_spawn_file_actions_addchdir_np(&m_actions, cwd.c_str());
		}
		return result;
	}

	[[nodiscard]] const posix_spawn_file_actions_t* Get() const { return &m_actions; }

private:
	posix_spawn_file_actions_t m_actions {};
	bool                       m_initialized = false;
};

class SpawnAttributes {
public:
	SpawnAttributes() = default;
	~SpawnAttributes() {
		if (m_initialized) {
			::posix_spawnattr_destroy(&m_attributes);
		}
	}
	SpawnAttributes(const SpawnAttributes&)            = delete;
	SpawnAttributes& operator=(const SpawnAttributes&) = delete;

	// Default SIGPIPE and SIGTERM actions and an empty signal mask; 0 or the error number.
	int Init() {
		if (const int injected = InjectedFault(SpawnStep::Attributes); injected != 0) {
			return injected;
		}
		if (const int result = ::posix_spawnattr_init(&m_attributes); result != 0) {
			return result;
		}
		m_initialized = true;
		sigset_t defaults;
		sigemptyset(&defaults);
		sigaddset(&defaults, SIGPIPE);
		sigaddset(&defaults, SIGTERM);
		sigset_t empty;
		sigemptyset(&empty);
		int result = ::posix_spawnattr_setsigdefault(&m_attributes, &defaults);
		if (result == 0) {
			result = ::posix_spawnattr_setsigmask(&m_attributes, &empty);
		}
		if (result == 0) {
			result = ::posix_spawnattr_setflags(&m_attributes,
			                                    POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
		}
		return result;
	}

	[[nodiscard]] const posix_spawnattr_t* Get() const { return &m_attributes; }

private:
	posix_spawnattr_t m_attributes {};
	bool              m_initialized = false;
};

} // namespace

Expected<SpawnedProcess, SpawnError> SpawnEmulator(const SpawnRequest& request) {
	// posix_spawn changes the directory before execve, so a relative path would be resolved
	// against request.cwd.
	std::error_code ec;
	fs::path        executable = fs::absolute(request.executable, ec);
	if (ec) {
		executable = request.executable;
	}

	struct stat info {};
	if (::stat(executable.c_str(), &info) != 0) {
		const int  error_number = errno;
		const auto kind         = error_number == ENOENT || error_number == ENOTDIR
		                              ? SpawnErrorKind::Missing
		                              : SpawnErrorKind::NotExecutable;
		return Failure(kind, executable, error_number, ErrorText(error_number));
	}
	if (!S_ISREG(info.st_mode)) {
		return Failure(SpawnErrorKind::NotExecutable, executable, EACCES, "not a regular file",
		               SpawnCause::NotRegularFile);
	}
	if (::access(executable.c_str(), X_OK) != 0) {
		const int error_number = errno;
		return Failure(SpawnErrorKind::NotExecutable, executable, error_number,
		               ErrorText(error_number));
	}
	for (size_t i = 0; i < request.args.size(); i++) {
		if (request.args[i].find('\0') != std::string::npos) {
			return Failure(SpawnErrorKind::SpawnFailed, executable, EINVAL,
			               "argument " + std::to_string(i + 1) + " contains a NUL byte",
			               SpawnCause::NulArgument, i + 1);
		}
	}

	Descriptor stdout_read;
	Descriptor stdout_write;
	Descriptor stderr_read;
	Descriptor stderr_write;
	int        pipe_error = OpenPipe(stdout_read, stdout_write);
	if (pipe_error == 0) {
		pipe_error = OpenPipe(stderr_read, stderr_write);
	}
	if (pipe_error != 0) {
		return Failure(SpawnErrorKind::SpawnFailed, executable, pipe_error,
		               "cannot create a pipe: " + ErrorText(pipe_error), SpawnCause::Pipe);
	}

	FileActions actions;
	if (const int error_number = actions.Init(stdout_write.fd, stderr_write.fd, request.cwd);
	    error_number != 0) {
		return Failure(SpawnErrorKind::SpawnFailed, executable, error_number,
		               "cannot prepare the spawn file actions: " + ErrorText(error_number),
		               SpawnCause::FileActions);
	}
	SpawnAttributes attributes;
	if (const int error_number = attributes.Init(); error_number != 0) {
		return Failure(SpawnErrorKind::SpawnFailed, executable, error_number,
		               "cannot prepare the spawn attributes: " + ErrorText(error_number),
		               SpawnCause::Attributes);
	}

	std::string              path = executable.string();
	std::vector<std::string> args = request.args;
	std::vector<char*>       argv;
	argv.reserve(args.size() + 2);
	argv.push_back(path.data());
	for (auto& arg: args) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);

	pid_t pid          = -1;
	int   error_number = InjectedFault(SpawnStep::Spawn);
	if (error_number == 0) {
		error_number = ::posix_spawn(&pid, path.c_str(), actions.Get(), attributes.Get(),
		                             argv.data(), environ);
	}
	if (error_number != 0) {
		std::string reason = ErrorText(error_number);
		if (!request.cwd.empty()) {
			reason += " (working directory " + request.cwd.string() + ")";
		}
		return Failure(SpawnErrorKind::SpawnFailed, executable, error_number, std::move(reason),
		               SpawnCause::Spawn, 0, request.cwd);
	}

	// The write ends close here; the child holds its own copies.
	SpawnedProcess process;
	process.pid       = pid;
	process.stdout_fd = stdout_read.Release();
	process.stderr_fd = stderr_read.Release();
	return process;
}

bool SendTerminate(pid_t pid) {
	return pid > 0 && ::kill(pid, SIGTERM) == 0;
}

bool SendKill(pid_t pid) {
	return pid > 0 && ::kill(pid, SIGKILL) == 0;
}

void SetSpawnFaultForTesting(std::function<int(SpawnStep)> fault) {
	FaultHook() = std::move(fault);
}

} // namespace kyty::core
