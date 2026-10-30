#include "updater/core/processRunner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <optional>
#include <spawn.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace kyty::updater {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kRemovedVariable = "TAR_OPTIONS=";

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

std::string CannotStart(const fs::path& program, const std::string& reason) {
	return "Cannot start " + program.string() + ": " + reason;
}

// posix_spawn() changes the directory before it executes program, so a relative program would be
// resolved against cwd.
fs::path Absolute(const fs::path& program) {
	std::error_code ec;
	fs::path        absolute = fs::absolute(program, ec);
	return ec ? program : absolute;
}

// Why args cannot be passed intact, if they cannot.
std::optional<std::string> CheckArguments(const std::vector<std::string>& args) {
	for (size_t i = 0; i < args.size(); i++) {
		if (args[i].find('\0') != std::string::npos) {
			return "argument " + std::to_string(i + 1) + " contains a NUL byte";
		}
	}
	return std::nullopt;
}

// Closes the descriptor on destruction unless it was closed already.
class Descriptor {
public:
	Descriptor() = default;
	~Descriptor() { Close(); }
	Descriptor(const Descriptor&)            = delete;
	Descriptor& operator=(const Descriptor&) = delete;

	void Close() {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}

	int fd = -1;
};

// The file actions and attributes of one posix_spawn() call.
class Spawn {
public:
	Spawn() = default;
	~Spawn() {
		if (m_actions_ready) {
			::posix_spawn_file_actions_destroy(&m_actions);
		}
		if (m_attributes_ready) {
			::posix_spawnattr_destroy(&m_attributes);
		}
	}
	Spawn(const Spawn&)            = delete;
	Spawn& operator=(const Spawn&) = delete;

	// Standard output from stdout_fd (/dev/null when it is negative), standard error from
	// stderr_fd, standard input from /dev/null; 0 or the error number.
	int Prepare(int stdout_fd, int stderr_fd, const fs::path& cwd, bool new_session) {
		int result = ::posix_spawn_file_actions_init(&m_actions);
		if (result != 0) {
			return result;
		}
		m_actions_ready = true;
		// The duplications come first, so a descriptor below 3 cannot be replaced before it is
		// duplicated.
		if (stdout_fd >= 0) {
			result = ::posix_spawn_file_actions_adddup2(&m_actions, stdout_fd, STDOUT_FILENO);
		}
		if (result == 0) {
			result = ::posix_spawn_file_actions_adddup2(&m_actions, stderr_fd, STDERR_FILENO);
		}
		if (result == 0) {
			result = ::posix_spawn_file_actions_addopen(&m_actions, STDIN_FILENO, "/dev/null",
			                                            O_RDONLY, 0);
		}
		if (result == 0 && stdout_fd < 0) {
			result = ::posix_spawn_file_actions_addopen(&m_actions, STDOUT_FILENO, "/dev/null",
			                                            O_WRONLY, 0);
		}
		if (result == 0) {
			result = ::posix_spawn_file_actions_addclosefrom_np(&m_actions, 3);
		}
		if (result == 0 && !cwd.empty()) {
			result = ::posix_spawn_file_actions_addchdir_np(&m_actions, cwd.c_str());
		}
		if (result != 0) {
			return result;
		}

		result = ::posix_spawnattr_init(&m_attributes);
		if (result != 0) {
			return result;
		}
		m_attributes_ready = true;
		sigset_t defaults;
		sigemptyset(&defaults);
		sigaddset(&defaults, SIGPIPE);
		sigset_t empty;
		sigemptyset(&empty);
		short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
		if (new_session) {
			flags = static_cast<short>(flags | POSIX_SPAWN_SETSID);
		}
		result = ::posix_spawnattr_setsigdefault(&m_attributes, &defaults);
		if (result == 0) {
			result = ::posix_spawnattr_setsigmask(&m_attributes, &empty);
		}
		if (result == 0) {
			result = ::posix_spawnattr_setflags(&m_attributes, flags);
		}
		return result;
	}

	// Spawns program with this process's environment minus TAR_OPTIONS; 0 or the error number.
	int Start(const fs::path& program, std::vector<std::string> args, pid_t& pid) {
		std::string        path = program.string();
		std::vector<char*> argv;
		argv.reserve(args.size() + 2);
		argv.push_back(path.data());
		for (auto& arg: args) {
			argv.push_back(arg.data());
		}
		argv.push_back(nullptr);

		std::vector<char*> envp;
		for (char** entry = environ; *entry != nullptr; entry++) {
			if (!std::string_view(*entry).starts_with(kRemovedVariable)) {
				envp.push_back(*entry);
			}
		}
		envp.push_back(nullptr);

		return ::posix_spawn(&pid, path.c_str(), &m_actions, &m_attributes, argv.data(),
		                     envp.data());
	}

private:
	posix_spawn_file_actions_t m_actions {};
	posix_spawnattr_t          m_attributes {};
	bool                       m_actions_ready    = false;
	bool                       m_attributes_ready = false;
};

std::string SpawnFailure(const fs::path& program, int error_number, const fs::path& cwd) {
	std::string reason = ErrorText(error_number);
	if (!cwd.empty()) {
		reason += " (working directory " + cwd.string() + ")";
	}
	return CannotStart(program, reason);
}

class PosixProcessRunner final: public ProcessRunner {
public:
	ProcessResult Run(const fs::path& program, std::vector<std::string> args,
	                  const fs::path& cwd) override {
		ProcessResult  result;
		const fs::path absolute = Absolute(program);
		if (auto problem = CheckArguments(args)) {
			result.error = CannotStart(absolute, *problem);
			return result;
		}

		Descriptor read_end;
		Descriptor write_end;
		int        fds[2] = {-1, -1};
		if (::pipe2(fds, O_CLOEXEC) != 0) {
			result.error = CannotStart(absolute, "cannot create a pipe: " + ErrorText(errno));
			return result;
		}
		read_end.fd  = fds[0];
		write_end.fd = fds[1];

		Spawn spawn;
		pid_t pid          = -1;
		int   error_number = spawn.Prepare(-1, write_end.fd, cwd, false);
		if (error_number != 0) {
			result.error =
			    CannotStart(absolute, "cannot prepare the spawn: " + ErrorText(error_number));
			return result;
		}
		error_number = spawn.Start(absolute, std::move(args), pid);
		if (error_number != 0) {
			result.error = SpawnFailure(absolute, error_number, cwd);
			return result;
		}
		// The child holds its own copy; reading ends when it and its children close theirs.
		write_end.Close();

		std::array<char, 16384> buffer {};
		for (;;) {
			const ssize_t count = ::read(read_end.fd, buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR) {
				continue;
			}
			if (count <= 0) {
				break;
			}
			const size_t room = kMaxCapturedStandardError - result.standard_error.size();
			result.standard_error.append(buffer.data(), std::min(room, static_cast<size_t>(count)));
		}
		read_end.Close();

		int status = 0;
		while (::waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				result.error = "Cannot wait for " + absolute.string() + ": " + ErrorText(errno);
				return result;
			}
		}
		result.wait_status = status;
		return result;
	}

	kyty::core::Expected<void, std::string> StartDetached(const fs::path&          program,
	                                                      std::vector<std::string> args,
	                                                      const fs::path&          cwd,
	                                                      const fs::path& log_file) override {
		const fs::path absolute = Absolute(program);
		if (auto problem = CheckArguments(args)) {
			return kyty::core::MakeUnexpected(CannotStart(absolute, *problem));
		}
		Descriptor log;
		log.fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
		if (log.fd < 0) {
			return kyty::core::MakeUnexpected(CannotStart(absolute, "cannot open the log file " +
			                                                            log_file.string() + ": " +
			                                                            ErrorText(errno)));
		}

		Spawn spawn;
		pid_t pid          = -1;
		int   error_number = spawn.Prepare(log.fd, log.fd, cwd, true);
		if (error_number != 0) {
			return kyty::core::MakeUnexpected(
			    CannotStart(absolute, "cannot prepare the spawn: " + ErrorText(error_number)));
		}
		error_number = spawn.Start(absolute, std::move(args), pid);
		if (error_number != 0) {
			return kyty::core::MakeUnexpected(SpawnFailure(absolute, error_number, cwd));
		}
		return {};
	}
};

} // namespace

std::unique_ptr<ProcessRunner> MakePosixProcessRunner() {
	return std::make_unique<PosixProcessRunner>();
}

} // namespace kyty::updater
