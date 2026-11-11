// Example tests of SpawnEmulator, SendTerminate, and SendKill (src/launcher/core/emulatorProcess.h,
// task 15.3) through the process helpers argv_echo and fake_emulator (task 15.6): argument bytes,
// the working directory, standard streams, closed descriptors, signal dispositions and mask,
// missing and non-executable executables, injected spawn faults, and SIGTERM followed by SIGKILL.

#include "launcher/core/emulatorProcess.h"
#include "launcher/core/exitStatus.h"
#include "support/helperPaths.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::core::SpawnedProcess;
using kyty::core::SpawnEmulator;
using kyty::core::SpawnErrorKind;
using kyty::core::SpawnRequest;
using kyty::core::SpawnStep;
using KytyTests::HelperPath;
using KytyTests::TempDir;
using namespace std::chrono_literals;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "EmulatorProcessTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

fs::path ArgvEcho() {
	return HelperPath("argv_echo");
}

fs::path FakeEmulator() {
	return HelperPath("fake_emulator");
}

// The open descriptors of this process.
std::vector<int> OpenDescriptors() {
	std::vector<int> descriptors;
	DIR*             directory = opendir("/proc/self/fd");
	Check(directory != nullptr, "opendir /proc/self/fd");
	const int own = dirfd(directory);
	while (const dirent* entry = readdir(directory)) {
		if (entry->d_name[0] != '.' && std::atoi(entry->d_name) != own) {
			descriptors.push_back(std::atoi(entry->d_name));
		}
	}
	closedir(directory);
	std::sort(descriptors.begin(), descriptors.end());
	return descriptors;
}

// The output of a running child, read from its non-blocking pipes.
class Child {
public:
	explicit Child(const SpawnedProcess& process): m_process(process) {}
	~Child() {
		CloseOutput();
		if (!m_reaped) {
			kill(m_process.pid, SIGKILL);
			int status = 0;
			waitpid(m_process.pid, &status, 0);
		}
	}
	Child(const Child&)            = delete;
	Child& operator=(const Child&) = delete;

	[[nodiscard]] pid_t              Pid() const { return m_process.pid; }
	[[nodiscard]] const std::string& Out() const { return m_out; }
	[[nodiscard]] const std::string& Err() const { return m_err; }

	// Reads what is available, waiting at most timeout for something to arrive.
	void Pump(std::chrono::milliseconds timeout) {
		std::array<pollfd, 2> fds {};
		fds[0]          = {m_open[0] ? m_process.stdout_fd : -1, POLLIN, 0};
		fds[1]          = {m_open[1] ? m_process.stderr_fd : -1, POLLIN, 0};
		const int ready = poll(fds.data(), fds.size(), static_cast<int>(timeout.count()));
		if (ready < 0) {
			Check(errno == EINTR, "poll: " + ErrorText(errno));
			return;
		}
		for (size_t i = 0; i < fds.size(); i++) {
			if (m_open[i] && fds[i].revents != 0) {
				Drain(i);
			}
		}
	}

	void WaitForStdout(const std::string& text) {
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (m_out.find(text) == std::string::npos) {
			Check(std::chrono::steady_clock::now() < deadline,
			      "the child writes '" + text + "' within 10 s (stdout '" + m_out + "')");
			Check(m_open[0], "stdout closed before '" + text + "' (stdout '" + m_out + "')");
			Pump(100ms);
		}
	}

	void ReadToEnd() {
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (m_open[0] || m_open[1]) {
			Check(std::chrono::steady_clock::now() < deadline,
			      "the child closes its output within 10 s");
			Pump(100ms);
		}
		CloseOutput();
	}

	// The wait status; fails after the limit.
	int Wait(std::chrono::milliseconds limit = 10s) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (Running()) {
			Check(std::chrono::steady_clock::now() < deadline, "the child exits in time");
			std::this_thread::sleep_for(5ms);
		}
		return m_status;
	}

	// Reaps the child when it has exited.
	bool Running() {
		if (m_reaped) {
			return false;
		}
		const pid_t result = waitpid(m_process.pid, &m_status, WNOHANG);
		Check(result >= 0, "waitpid: " + ErrorText(errno));
		m_reaped = result == m_process.pid;
		return !m_reaped;
	}

	void CloseOutput() {
		for (size_t i = 0; i < 2; i++) {
			if (m_open[i]) {
				close(i == 0 ? m_process.stdout_fd : m_process.stderr_fd);
				m_open[i] = false;
			}
		}
	}

private:
	void Drain(size_t index) {
		const int               fd   = index == 0 ? m_process.stdout_fd : m_process.stderr_fd;
		std::string&            sink = index == 0 ? m_out : m_err;
		std::array<char, 65536> buffer {};
		for (;;) {
			const ssize_t count = read(fd, buffer.data(), buffer.size());
			if (count > 0) {
				sink.append(buffer.data(), static_cast<size_t>(count));
				continue;
			}
			if (count == 0) {
				close(fd);
				m_open[index] = false;
				return;
			}
			if (errno == EINTR) {
				continue;
			}
			Check(errno == EAGAIN, "read: " + ErrorText(errno));
			return;
		}
	}

	SpawnedProcess m_process;
	std::string    m_out;
	std::string    m_err;
	bool           m_open[2] = {true, true};
	bool           m_reaped  = false;
	int            m_status  = 0;
};

SpawnedProcess Spawn(const fs::path& executable, std::vector<std::string> args,
                     const fs::path& cwd) {
	auto result = SpawnEmulator(SpawnRequest {executable, std::move(args), cwd});
	Check(result.has_value(), "spawn " + executable.string() + ": " +
	                              (result.has_value() ? std::string() : result.error().message));
	return *result;
}

struct Finished {
	std::string out;
	std::string err;
	int         status = 0;
};

Finished Run(const fs::path& executable, std::vector<std::string> args, const fs::path& cwd) {
	Child child(Spawn(executable, std::move(args), cwd));
	child.ReadToEnd();
	const int status = child.Wait();
	return {child.Out(), child.Err(), status};
}

std::string NulTerminated(const std::vector<std::string>& args) {
	std::string text;
	for (const auto& arg: args) {
		text += arg;
		text.push_back('\0');
	}
	return text;
}

bool ExitedWith(int status, int code) {
	return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

bool KilledBy(int status, int signal_number) {
	return WIFSIGNALED(status) && WTERMSIG(status) == signal_number;
}

void TestArgumentBytes() {
	const TempDir                  temp("kyty_emulator_process");
	const std::vector<std::string> args = {"--game",
	                                       "/games/with space/'single' \"double\" back\\slash "
	                                       "`tick` $ ; & | < > * ? ( ) [ ] { } ! # ~",
	                                       "",
	                                       "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xAE",
	                                       "\xFF\xFE not UTF-8",
	                                       "new\nline\ttab",
	                                       "-",
	                                       "--game"};
	const auto                     result = Run(ArgvEcho(), args, temp.Path());
	Check(ExitedWith(result.status, 0), "argv_echo exits 0");
	Check(result.out == NulTerminated(args), "argv_echo receives every byte of every argument");
	Check(result.err.empty(), "argv_echo writes nothing to stderr");

	Check(Run(ArgvEcho(), {}, temp.Path()).out.empty(), "no arguments");
	// The shortest game path, one byte (Property 22 covers 1-4,095 bytes below a temporary root).
	Check(Run(ArgvEcho(), {"--game", "/"}, temp.Path()).out == NulTerminated({"--game", "/"}),
	      "a one-byte game path");
}

void TestReturnedDescriptors() {
	const TempDir temp("kyty_emulator_process");
	const auto    before = OpenDescriptors().size();
	{
		Child child(Spawn(FakeEmulator(), {"--stdout", "x"}, temp.Path()));
		child.ReadToEnd();
		Check(ExitedWith(child.Wait(), 0), "fake_emulator exits 0");
		Check(child.Out() == "x\n", "its output");
	}
	Check(OpenDescriptors().size() == before, "no descriptor is left after the child is read");

	auto spawned = SpawnEmulator(SpawnRequest {FakeEmulator(), {"--wait"}, temp.Path()});
	Check(spawned.has_value(), "spawn");
	Check(spawned->pid > 0, "a pid");
	for (const int fd: {spawned->stdout_fd, spawned->stderr_fd}) {
		Check(fd > 2, "a new descriptor");
		Check((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "the read end is non-blocking");
		Check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "the read end is close-on-exec");
	}
	Check(OpenDescriptors().size() == before + 2, "exactly the two read ends stay open");
	Child child(*spawned);
	Check(kyty::core::SendTerminate(child.Pid()), "SendTerminate");
	child.ReadToEnd();
	Check(KilledBy(child.Wait(), SIGTERM), "SIGTERM ends fake_emulator --wait");
}

void TestWorkingDirectory() {
	const TempDir   temp("kyty_emulator_process");
	const auto      data_dir = temp.MakeDirectories("Data Directory");
	std::error_code ec;
	const auto      real = fs::canonical(data_dir, ec);
	Check(!ec, "canonical");
	auto result = Run(FakeEmulator(), {"--print-cwd"}, data_dir);
	Check(result.out == real.string() + "\n", "the child runs in cwd: " + result.out);

	// A relative executable path is resolved against our working directory, not the child's.
	const auto relative = FakeEmulator().lexically_relative(fs::current_path(ec));
	Check(!ec && !relative.empty() && relative.is_relative(), "a relative helper path");
	result = Run(relative, {"--print-cwd"}, data_dir);
	Check(result.out == real.string() + "\n", "a relative executable with another cwd");

	// An empty cwd keeps ours.
	result = Run(FakeEmulator(), {"--print-cwd"}, "");
	Check(result.out == fs::canonical(fs::current_path(ec), ec).string() + "\n",
	      "an empty cwd keeps the working directory: " + result.out);
}

void TestStandardStreams() {
	const TempDir temp("kyty_emulator_process");
	auto          result = Run(FakeEmulator(), {"--describe-stdio"}, temp.Path());
	Check(result.out == "stdin dev-null rdonly\nstdout blocking\nstderr blocking\n",
	      "stdin is /dev/null and the child's pipe ends block: " + result.out);

	result = Run(FakeEmulator(),
	             {"--stdout", "line", "--stderr-hex", "80ff0a", "--stdout-hex", "7461696c"},
	             temp.Path());
	Check(result.out == "line\ntail", "stdout bytes, the last line unterminated");
	Check(result.err == "\x80\xff\n", "stderr bytes, invalid UTF-8 kept");
}

void TestDescriptorsClosed() {
	const TempDir temp("kyty_emulator_process");
	// Descriptors without FD_CLOEXEC, which the child would otherwise inherit.
	const int low = open("/dev/null", O_RDONLY);
	Check(low >= 0, "open /dev/null");
	const int high = fcntl(low, F_DUPFD, 200);
	Check(high >= 200, "F_DUPFD");
	Check((fcntl(low, F_GETFD) & FD_CLOEXEC) == 0 && (fcntl(high, F_GETFD) & FD_CLOEXEC) == 0,
	      "the extra descriptors are inheritable");
	const auto result = Run(FakeEmulator(), {"--list-fds"}, temp.Path());
	close(low);
	close(high);
	Check(result.out == "0 1 2\n", "the child has only descriptors 0, 1, and 2: " + result.out);
}

void TestSignalDefaults() {
	const TempDir    temp("kyty_emulator_process");
	struct sigaction ignore {};
	ignore.sa_handler = SIG_IGN;
	struct sigaction old_pipe {};
	struct sigaction old_term {};
	Check(sigaction(SIGPIPE, &ignore, &old_pipe) == 0, "ignore SIGPIPE");
	Check(sigaction(SIGTERM, &ignore, &old_term) == 0, "ignore SIGTERM");
	sigset_t block;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGTERM);
	sigset_t old_mask;
	Check(sigprocmask(SIG_BLOCK, &block, &old_mask) == 0, "block signals");

	const auto result = Run(FakeEmulator(), {"--print-signal-state"}, temp.Path());

	Check(sigprocmask(SIG_SETMASK, &old_mask, nullptr) == 0, "restore the mask");
	Check(sigaction(SIGTERM, &old_term, nullptr) == 0, "restore SIGTERM");
	Check(sigaction(SIGPIPE, &old_pipe, nullptr) == 0, "restore SIGPIPE");
	Check(result.out == "SIGPIPE default\nSIGTERM default\nblocked none\n",
	      "default SIGPIPE and SIGTERM and an empty mask although this process ignores and blocks "
	      "them: " +
	          result.out);
}

void CheckSpawnError(const fs::path& executable, SpawnErrorKind kind, int error_number,
                     const std::string& reason, const std::string& what) {
	const auto before = OpenDescriptors().size();
	auto       result = SpawnEmulator(SpawnRequest {executable, {"--game", "/x"}, "/"});
	Check(!result.has_value(), what + ": fails");
	const auto&     error = result.error();
	std::error_code ec;
	const auto      absolute = fs::absolute(executable, ec);
	Check(error.kind == kind, what + ": kind");
	Check(error.executable == absolute && error.executable.is_absolute(),
	      what + ": the absolute path " + error.executable.string());
	Check(error.error_number == error_number,
	      what + ": errno " + std::to_string(error.error_number));
	Check(error.reason == reason, what + ": reason '" + error.reason + "'");
	Check(error.message == "Cannot start " + absolute.string() + ": " + reason,
	      what + ": message '" + error.message + "'");
	// Phase 9 V1: "not a regular file" has a kind of its own; the others are the errno text.
	Check(error.cause == (reason == "not a regular file" ? kyty::core::SpawnCause::NotRegularFile
	                                                     : kyty::core::SpawnCause::ErrorNumber),
	      what + ": the reason's cause");
	Check(OpenDescriptors().size() == before, what + ": no descriptor is left");
}

void TestMissingAndNotExecutable() {
	const TempDir temp("kyty_emulator_process");
	CheckSpawnError(temp.Path() / "missing-emulator", SpawnErrorKind::Missing, ENOENT,
	                ErrorText(ENOENT), "a missing executable");
	CheckSpawnError("missing-relative-emulator", SpawnErrorKind::Missing, ENOENT, ErrorText(ENOENT),
	                "a missing relative executable");
	const auto file = temp.WriteFile("regular.txt", "x");
	CheckSpawnError(file / "emulator", SpawnErrorKind::Missing, ENOTDIR, ErrorText(ENOTDIR),
	                "a path below a regular file");
	CheckSpawnError(temp.MakeSymlink("nowhere", "dangling"), SpawnErrorKind::Missing, ENOENT,
	                ErrorText(ENOENT), "a dangling symbolic link");

	// Mode 0644 at creation: no execute bit for anyone, so access(X_OK) fails even for root.
	const auto script = temp.Path() / "kyty_emulator";
	const int  fd     = open(script.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	Check(fd >= 0, "create the non-executable file");
	Check(write(fd, "#!/bin/sh\nexit 0\n", 17) == 17, "write the script");
	close(fd);
	CheckSpawnError(script, SpawnErrorKind::NotExecutable, EACCES, ErrorText(EACCES),
	                "a file without execute permission");
	CheckSpawnError(temp.Path(), SpawnErrorKind::NotExecutable, EACCES, "not a regular file",
	                "a directory");
}

void TestRealSpawnFailures() {
	const TempDir temp("kyty_emulator_process");
	const auto    before = OpenDescriptors().size();

	// The working directory does not exist: posix_spawn reports the child's chdir failure.
	const auto missing_cwd = temp.Path() / "missing-data-directory";
	auto       result      = SpawnEmulator(SpawnRequest {FakeEmulator(), {}, missing_cwd});
	Check(!result.has_value() && result.error().kind == SpawnErrorKind::SpawnFailed &&
	          result.error().error_number == ENOENT,
	      "a missing working directory fails the spawn");
	Check(result.error().reason ==
	          ErrorText(ENOENT) + " (working directory " + missing_cwd.string() + ")",
	      "the reason names the working directory: " + result.error().reason);
	Check(result.error().cause == kyty::core::SpawnCause::Spawn &&
	          result.error().cwd == missing_cwd,
	      "the spawn cause with the working directory");

	// An executable file that is not a program.
	const auto        garbage = temp.Path() / "not-a-program";
	const std::string bytes   = "\x01\x02\x03\x04 not a program\n";
	const int         fd = open(garbage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
	Check(fd >= 0, "create the file");
	Check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()),
	      "write the file");
	close(fd);
	result = SpawnEmulator(SpawnRequest {garbage, {}, ""});
	Check(!result.has_value() && result.error().kind == SpawnErrorKind::SpawnFailed &&
	          result.error().error_number == ENOEXEC && result.error().reason == ErrorText(ENOEXEC),
	      "a file in no executable format: " +
	          (result.has_value() ? std::string("started") : result.error().message));

	// An argument with a NUL byte cannot be passed intact.
	result = SpawnEmulator(SpawnRequest {ArgvEcho(), {"ok", std::string("a\0b", 3)}, temp.Path()});
	Check(!result.has_value() && result.error().kind == SpawnErrorKind::SpawnFailed &&
	          result.error().error_number == EINVAL &&
	          result.error().reason == "argument 2 contains a NUL byte" &&
	          result.error().cause == kyty::core::SpawnCause::NulArgument &&
	          result.error().argument == 2,
	      "an argument with a NUL byte is refused");
	Check(OpenDescriptors().size() == before, "no descriptor is left after a failed spawn");
}

struct SpawnFault {
	SpawnStep              step;
	int                    occurrence; // 1 = the first time the step is asked
	int                    error_number;
	const char*            reason_prefix;
	kyty::core::SpawnCause cause;
};

void TestInjectedSpawnFaults() {
	using kyty::core::SpawnCause;
	const TempDir    temp("kyty_emulator_process");
	const SpawnFault faults[] = {
	    {SpawnStep::Pipe, 1, EMFILE, "cannot create a pipe: ", SpawnCause::Pipe},
	    {SpawnStep::Pipe, 2, ENFILE, "cannot create a pipe: ", SpawnCause::Pipe},
	    {SpawnStep::FileActions, 1, ENOMEM,
	     "cannot prepare the spawn file actions: ", SpawnCause::FileActions},
	    {SpawnStep::Attributes, 1, EINVAL,
	     "cannot prepare the spawn attributes: ", SpawnCause::Attributes},
	    {SpawnStep::Spawn, 1, EAGAIN, "", SpawnCause::Spawn},
	};
	for (const auto& fault: faults) {
		const auto               before = OpenDescriptors();
		std::map<SpawnStep, int> calls;
		kyty::core::SetSpawnFaultForTesting([&](SpawnStep step) {
			return ++calls[step] == fault.occurrence && step == fault.step ? fault.error_number : 0;
		});
		auto result = SpawnEmulator(SpawnRequest {ArgvEcho(), {"x"}, temp.Path()});
		kyty::core::SetSpawnFaultForTesting({});
		const std::string what = "fault " + std::to_string(static_cast<int>(fault.step)) + "/" +
		                         std::to_string(fault.occurrence) + ": ";
		Check(!result.has_value(), what + "fails");
		const auto& error = result.error();
		Check(error.kind == SpawnErrorKind::SpawnFailed, what + "SpawnFailed");
		Check(error.error_number == fault.error_number, what + "errno");
		std::string reason = fault.reason_prefix + ErrorText(fault.error_number);
		if (fault.step == SpawnStep::Spawn) {
			reason += " (working directory " + temp.Path().string() + ")";
		}
		Check(error.reason == reason, what + "reason '" + error.reason + "'");
		Check(error.message == "Cannot start " + ArgvEcho().string() + ": " + reason,
		      what + "message '" + error.message + "'");
		// Phase 9 V1: the reason's kind and, for the spawn, the working directory.
		Check(error.cause == fault.cause &&
		          error.cwd == (fault.step == SpawnStep::Spawn ? temp.Path() : fs::path()),
		      what + "the cause and the working directory");
		Check(OpenDescriptors() == before, what + "every descriptor was closed");
		int status = 0;
		Check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, what + "no child started");
	}
	Check(Run(ArgvEcho(), {"after"}, temp.Path()).out == NulTerminated({"after"}),
	      "spawning works again without the hook");
}

std::string ReadFile(const fs::path& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

void TestTerminate() {
	const TempDir temp("kyty_emulator_process");
	Child         child(Spawn(FakeEmulator(), {"--stdout", "ready", "--wait"}, temp.Path()));
	child.WaitForStdout("ready\n");
	Check(child.Running(), "fake_emulator waits");
	Check(kyty::core::SendTerminate(child.Pid()), "SendTerminate");
	child.ReadToEnd();
	const int status = child.Wait();
	Check(KilledBy(status, SIGTERM), "SIGTERM ends a normal fake_emulator");
	Check(kyty::core::DescribeWaitStatus(status).text == "signal SIGTERM", "signal SIGTERM");
}

void TestKillAfterIgnoredTerm() {
	const TempDir temp("kyty_emulator_process");
	const auto    record = temp.Path() / "signals.txt";
	Child child(Spawn(FakeEmulator(),
	                  {"--ignore-term", "--record-signals", record.string(), "--stdout", "ready",
	                   "--sleep-ms", "60000"},
	                  temp.Path()));
	child.WaitForStdout("ready\n");
	Check(kyty::core::SendTerminate(child.Pid()), "SendTerminate");
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (ReadFile(record) != "15\n") {
		Check(std::chrono::steady_clock::now() < deadline, "SIGTERM is recorded within 5 s");
		std::this_thread::sleep_for(10ms);
	}
	std::this_thread::sleep_for(200ms);
	Check(child.Running(), "SIGTERM leaves fake_emulator --ignore-term running");
	Check(kyty::core::SendKill(child.Pid()), "SendKill");
	child.ReadToEnd();
	const int status = child.Wait();
	Check(KilledBy(status, SIGKILL), "SIGKILL ends it");
	Check(kyty::core::DescribeWaitStatus(status).text == "signal SIGKILL", "signal SIGKILL");
	Check(ReadFile(record) == "15\n", "only SIGTERM was recorded");

	// Ignored without a recorder.
	Child ignoring(
	    Spawn(FakeEmulator(), {"--ignore-term", "--stdout", "ready", "--wait"}, temp.Path()));
	ignoring.WaitForStdout("ready\n");
	Check(kyty::core::SendTerminate(ignoring.Pid()), "SendTerminate");
	std::this_thread::sleep_for(200ms);
	Check(ignoring.Running(), "an ignored SIGTERM leaves it running");
	Check(kyty::core::SendKill(ignoring.Pid()), "SendKill");
	ignoring.ReadToEnd();
	Check(KilledBy(ignoring.Wait(), SIGKILL), "SIGKILL ends it");
}

void TestExitStatusThroughSpawn() {
	const TempDir temp("kyty_emulator_process");
	auto          result = Run(FakeEmulator(), {"--exit", "3"}, temp.Path());
	Check(kyty::core::DescribeWaitStatus(result.status).text == "exit code 3", "exit code 3");
	result = Run(FakeEmulator(), {"--raise", "1"}, temp.Path());
	Check(kyty::core::DescribeWaitStatus(result.status).text == "signal SIGHUP", "signal SIGHUP");
}

void TestSendToInvalidProcesses() {
	Check(!kyty::core::SendTerminate(0), "pid 0 (the process group) is refused");
	Check(!kyty::core::SendTerminate(-1), "pid -1 (every process) is refused");
	Check(!kyty::core::SendKill(0), "SendKill pid 0 is refused");
	Check(!kyty::core::SendKill(-12345), "a negative pid is refused");
	// Above the kernel's largest pid (4,194,304), so no such process exists.
	Check(!kyty::core::SendTerminate(std::numeric_limits<pid_t>::max()), "no such process");
	Check(!kyty::core::SendKill(std::numeric_limits<pid_t>::max()), "no such process");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("EmulatorProcessTests");
	TestArgumentBytes();
	TestReturnedDescriptors();
	TestWorkingDirectory();
	TestStandardStreams();
	TestDescriptorsClosed();
	TestSignalDefaults();
	TestMissingAndNotExecutable();
	TestRealSpawnFailures();
	TestInjectedSpawnFaults();
	TestTerminate();
	TestKillAfterIgnoredTerm();
	TestExitStatusThroughSpawn();
	TestSendToInvalidProcesses();
	std::printf("EmulatorProcessTests: all tests passed\n");
	return 0;
}
