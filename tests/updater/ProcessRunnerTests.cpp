// Example tests of kyty::updater::MakePosixProcessRunner (src/updater/core/processRunner.h, task
// 17.1) through the process helper fake_emulator (task 15.6) and /bin/sh: exit codes, signals, and
// standard error capture (the 4,096-byte limit, a process that writes far more), the working
// directory and a relative program, standard input and output from /dev/null, closed descriptors,
// the signal disposition and mask, TAR_OPTIONS removed from the environment, spawn failures, a
// detached start in a new session with its log file, and a real /usr/bin/tar -czf and -xzf round
// trip.

#include "support/helperPaths.h"
#include "support/tempDir.h"
#include "support/updaterGenerators.h"
#include "updater/core/processRunner.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <system_error>
#include <vector>

namespace {

namespace fs         = std::filesystem;
namespace UpdaterGen = KytyTests::UpdaterGen;
using kyty::updater::MakePosixProcessRunner;
using kyty::updater::ProcessResult;
using KytyTests::HelperPath;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ProcessRunnerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ErrorText(int error_number) {
	return std::generic_category().message(error_number);
}

fs::path FakeEmulator() {
	return HelperPath("fake_emulator");
}

std::string Hex(std::string_view bytes) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string           text;
	for (const char c: bytes) {
		const auto byte = static_cast<unsigned char>(c);
		text += kHex[byte >> 4u];
		text += kHex[byte & 0xFu];
	}
	return text;
}

void CheckExit(const ProcessResult& result, int code, const std::string& name) {
	Check(result.error.empty(), name + ": started (" + result.error + ")");
	Check(WIFEXITED(result.wait_status) && WEXITSTATUS(result.wait_status) == code,
	      name + ": exit code " + std::to_string(code) + " (status " +
	          std::to_string(result.wait_status) + ")");
}

void CheckNotStarted(const ProcessResult& result, const std::string& error, const std::string& name,
                     kyty::updater::ProcessErrorKind kind, int error_number) {
	Check(result.error == error, name + ": error '" + result.error + "', expected '" + error + "'");
	// Phase 9 V1: the same error with its kind and parts.
	Check(result.failure.message == error && result.failure.kind == kind &&
	          result.failure.error_number == error_number && result.failure.program.is_absolute(),
	      name + ": the failure's kind, errno, and program");
	Check(result.wait_status == -1 && !WIFEXITED(result.wait_status) &&
	          !WIFSIGNALED(result.wait_status),
	      name + ": no wait status");
	Check(result.standard_error.empty(), name + ": no standard error");
}

void TestExitAndStandardError() {
	const auto runner = MakePosixProcessRunner();
	auto result = runner->Run(FakeEmulator(),
	                          {"--stdout", "not captured", "--stderr", "hello", "--exit", "3"}, {});
	CheckExit(result, 3, "exit 3");
	Check(result.standard_error == "hello\n",
	      "standard error is captured: '" + result.standard_error + "'");

	CheckExit(runner->Run(FakeEmulator(), {}, {}), 0, "exit 0");
	result = runner->Run(FakeEmulator(), {"--stderr-hex", "00ff0a41"}, {});
	CheckExit(result, 0, "raw bytes");
	Check(result.standard_error == std::string("\0\xff\nA", 4), "raw standard error bytes");
	result = runner->Run(FakeEmulator(), {"--bogus"}, {});
	CheckExit(result, 64, "usage error");
	Check(result.standard_error == "fake_emulator: unknown argument '--bogus'\n", "usage text");
	result = runner->Run(FakeEmulator(), {"--stderr", "dying", "--raise", "15"}, {});
	Check(result.error.empty() && WIFSIGNALED(result.wait_status) &&
	          WTERMSIG(result.wait_status) == SIGTERM,
	      "a signal death");
	Check(result.standard_error == "dying\n", "standard error before a signal death");

	// Arguments reach the process unchanged: no shell, no splitting.
	result =
	    runner->Run(FakeEmulator(), {"--stderr", "a b  'c' \"d\" $HOME ; e", "--stderr", ""}, {});
	CheckExit(result, 0, "arguments");
	Check(result.standard_error == "a b  'c' \"d\" $HOME ; e\n\n",
	      "arguments are passed as they are");
}

void TestStandardErrorLimit() {
	const auto runner = MakePosixProcessRunner();
	for (const size_t size: {size_t {4095}, size_t {4096}, size_t {4097}, size_t {5000}}) {
		const std::string bytes  = UpdaterGen::PatternBytes(size, static_cast<uint32_t>(size));
		const auto        result = runner->Run(FakeEmulator(), {"--stderr-hex", Hex(bytes)}, {});
		CheckExit(result, 0, std::to_string(size) + " bytes");
		Check(result.standard_error == bytes.substr(0, 4096),
		      std::to_string(size) + " bytes: the first 4096 are kept");
	}
	// Far more than a pipe holds: the runner keeps reading, so the process finishes.
	std::vector<std::string> args;
	std::string              all;
	for (uint32_t i = 0; i < 4; i++) {
		const std::string part = UpdaterGen::PatternBytes(60000, 100 + i);
		all += part;
		args.push_back("--stderr-hex");
		args.push_back(Hex(part));
	}
	args.push_back("--exit");
	args.push_back("7");
	const auto result = runner->Run(FakeEmulator(), args, {});
	CheckExit(result, 7, "240,000 bytes of standard error");
	Check(result.standard_error == all.substr(0, 4096), "240,000 bytes: the first 4096 are kept");
}

// The child's standard streams, descriptors, signals, and environment, through /bin/sh: the shell
// reports its own standard input and output, then becomes fake_emulator with standard output on
// the captured standard error.
void TestChildState() {
	const auto runner = MakePosixProcessRunner();
	Check(setenv("TAR_OPTIONS", "--no-such-option", 1) == 0, "setenv TAR_OPTIONS");
	Check(setenv("KYTY_PROCESS_RUNNER_TEST", "kept", 1) == 0, "setenv");
	const auto previous_pipe = std::signal(SIGPIPE, SIG_IGN);
	sigset_t   block;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigset_t previous_mask;
	Check(sigprocmask(SIG_BLOCK, &block, &previous_mask) == 0, "sigprocmask");

	const std::string script =
	    "printf '%s|%s|%s|%s\\n' \"${TAR_OPTIONS-unset}\" \"$KYTY_PROCESS_RUNNER_TEST\" "
	    "\"$(readlink /proc/$$/fd/0)\" \"$(readlink /proc/$$/fd/1)\" >&2; "
	    "exec \"$0\" --list-fds --describe-stdio --print-signal-state 1>&2";
	const auto result = runner->Run("/bin/sh", {"-c", script, FakeEmulator().string()}, {});

	sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
	std::signal(SIGPIPE, previous_pipe);
	unsetenv("KYTY_PROCESS_RUNNER_TEST");
	unsetenv("TAR_OPTIONS");

	CheckExit(result, 0, "child state");
	Check(result.standard_error == "unset|kept|/dev/null|/dev/null\n"
	                               "0 1 2\n"
	                               "stdin dev-null rdonly\n"
	                               "stdout blocking\n"
	                               "stderr blocking\n"
	                               "SIGPIPE default\n"
	                               "SIGTERM default\n"
	                               "blocked none\n",
	      "child state: '" + result.standard_error + "'");
}

void TestWorkingDirectory() {
	const auto    runner = MakePosixProcessRunner();
	const TempDir temp("kyty_runner");
	const auto    work = temp.MakeDirectories("work dir");
	CheckExit(runner->Run(FakeEmulator(), {"--record-signals", "created-here"}, work), 0, "cwd");
	std::error_code ec;
	Check(fs::exists(work / "created-here", ec), "the process runs in cwd");

	// A relative program is resolved against this process's directory, not against cwd.
	const auto relative = FakeEmulator().lexically_relative(fs::current_path(ec));
	Check(!relative.empty() && relative.is_relative(), "a relative path of fake_emulator");
	CheckExit(runner->Run(relative, {"--record-signals", "relative"}, work), 0, "relative program");
	Check(fs::exists(work / "relative", ec), "the relative program ran in cwd");

	const auto missing = temp.Path() / "missing";
	const auto result  = runner->Run(FakeEmulator(), {}, missing);
	CheckNotStarted(result,
	                "Cannot start " + FakeEmulator().string() + ": " + ErrorText(ENOENT) +
	                    " (working directory " + missing.string() + ")",
	                "a missing working directory", kyty::updater::ProcessErrorKind::Spawn, ENOENT);
	Check(result.failure.path == missing && result.failure.program == FakeEmulator(),
	      "the failure names the working directory and the program");
}

void TestSpawnFailures() {
	const auto    runner = MakePosixProcessRunner();
	const TempDir temp("kyty_runner");
	const auto    missing = temp.Path() / "missing";
	using kyty::updater::ProcessErrorKind;
	CheckNotStarted(runner->Run(missing, {}, {}),
	                "Cannot start " + missing.string() + ": " + ErrorText(ENOENT),
	                "a missing program", ProcessErrorKind::Spawn, ENOENT);
	const auto plain = temp.WriteFile("plain", "#!/bin/sh\nexit 0\n");
	CheckNotStarted(runner->Run(plain, {}, {}),
	                "Cannot start " + plain.string() + ": " + ErrorText(EACCES),
	                "a file without execute permission", ProcessErrorKind::Spawn, EACCES);
	CheckNotStarted(runner->Run(temp.Path(), {}, {}),
	                "Cannot start " + temp.Path().string() + ": " + ErrorText(EACCES),
	                "a directory", ProcessErrorKind::Spawn, EACCES);
	const auto nul = runner->Run(FakeEmulator(), {"--stderr", std::string("a\0b", 3)}, {});
	CheckNotStarted(nul,
	                "Cannot start " + FakeEmulator().string() + ": argument 2 contains a NUL byte",
	                "a NUL byte", ProcessErrorKind::NulArgument, EINVAL);
	Check(nul.failure.argument == 2, "the NUL byte's argument number");

	// No descriptor left for the pipe (or the log file).
	rlimit saved {};
	Check(getrlimit(RLIMIT_NOFILE, &saved) == 0, "getrlimit");
	rlimit limit   = saved;
	limit.rlim_cur = 3;
	Check(setrlimit(RLIMIT_NOFILE, &limit) == 0, "setrlimit");
	const auto no_pipe = runner->Run(FakeEmulator(), {}, {});
	const auto no_log  = runner->StartDetached(FakeEmulator(), {}, {}, temp.Path() / "log");
	Check(setrlimit(RLIMIT_NOFILE, &saved) == 0, "restore the descriptor limit");
	CheckNotStarted(no_pipe,
	                "Cannot start " + FakeEmulator().string() +
	                    ": cannot create a pipe: " + ErrorText(EMFILE),
	                "no descriptors", ProcessErrorKind::Pipe, EMFILE);
	Check(!no_log && no_log.error().message ==
	                     "Cannot start " + FakeEmulator().string() + ": cannot open the log file " +
	                         (temp.Path() / "log").string() + ": " + ErrorText(EMFILE),
	      "no descriptor for the log file: " + (no_log ? std::string() : no_log.error().message));
	Check(no_log.error().kind == ProcessErrorKind::OpenLog &&
	          no_log.error().error_number == EMFILE && no_log.error().path == temp.Path() / "log",
	      "the log file failure's parts");
}

void TestDetached() {
	const auto    runner = MakePosixProcessRunner();
	const TempDir temp("kyty_runner");
	const auto    work = temp.MakeDirectories("workspace");
	const auto    log  = temp.WriteFile("workspace/helper.log", "old content that must go\n");
	Check(setenv("TAR_OPTIONS", "--no-such-option", 1) == 0, "setenv TAR_OPTIONS");
	const std::string script =
	    "set -- $(cat /proc/$$/stat); printf 'pid=%s session=%s\\n' \"$1\" \"$6\"; "
	    "printf 'tar=%s\\n' \"${TAR_OPTIONS-unset}\"; "
	    "exec \"$0\" --print-cwd --describe-stdio --stderr done";
	const auto started =
	    runner->StartDetached("/bin/sh", {"-c", script, FakeEmulator().string()}, work, log);
	unsetenv("TAR_OPTIONS");
	Check(started.has_value(), "started: " + (started ? std::string() : started.error().message));

	// The detached process is still a child of this one: wait for it, then read its log.
	int         status = 0;
	const pid_t pid    = waitpid(-1, &status, 0);
	Check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the detached process exits 0");
	const auto text     = UpdaterGen::ReadFile(log).value_or("");
	const auto pid_text = std::to_string(pid);
	Check(text == "pid=" + pid_text + " session=" + pid_text +
	                  "\n"
	                  "tar=unset\n" +
	                  work.string() +
	                  "\n"
	                  "stdin dev-null rdonly\n"
	                  "stdout blocking\n"
	                  "stderr blocking\n"
	                  "done\n",
	      "the log of the detached process (a session leader in cwd): '" + text + "'");

	const auto missing_log = temp.Path() / "missing" / "helper.log";
	const auto no_log      = runner->StartDetached(FakeEmulator(), {}, work, missing_log);
	Check(!no_log && no_log.error().message == "Cannot start " + FakeEmulator().string() +
	                                               ": cannot open the log file " +
	                                               missing_log.string() + ": " + ErrorText(ENOENT),
	      "a log file that cannot be created");
	const auto missing     = temp.Path() / "missing-program";
	const auto not_started = runner->StartDetached(missing, {}, work, log);
	Check(!not_started && not_started.error().message ==
	                          "Cannot start " + missing.string() + ": " + ErrorText(ENOENT) +
	                              " (working directory " + work.string() + ")",
	      "a missing program: " + (not_started ? std::string() : not_started.error().message));
	Check(not_started.error().kind == kyty::updater::ProcessErrorKind::Spawn &&
	          not_started.error().error_number == ENOENT && not_started.error().path == work &&
	          not_started.error().program == missing,
	      "a missing program's parts");
	const auto nul = runner->StartDetached(FakeEmulator(), {std::string("\0", 1)}, work, log);
	Check(!nul &&
	          nul.error().message ==
	              "Cannot start " + FakeEmulator().string() + ": argument 1 contains a NUL byte" &&
	          nul.error().kind == kyty::updater::ProcessErrorKind::NulArgument &&
	          nul.error().argument == 1,
	      "a NUL byte");
	Check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "no other process was started");
}

// The update flow's extraction: a package made with tar -czf comes back with tar -xzf into an
// empty payload directory, byte for byte; TAR_OPTIONS (set to an invalid option here) does not
// reach tar.
void TestTarRoundTrip() {
	const fs::path tar    = "/usr/bin/tar";
	const auto     runner = MakePosixProcessRunner();
	const TempDir  temp("kyty_runner");
	temp.WriteFile("source/kyty_launcher", "launcher");
	temp.WriteFile("source/assets/sounds/trophy-unlock.wav", UpdaterGen::PatternBytes(70000, 5));
	temp.WriteFile("source/assets/cursors/a b.png", UpdaterGen::PatternBytes(3000, 6));
	temp.WriteFile("source/\xC3\xA9t\xC3\xA9.txt", "");
	temp.MakeDirectories("source/empty");
	const auto source  = temp.Path() / "source";
	const auto package = temp.Path() / "package-Linux-x86_64.tar.gz";
	const auto payload = temp.MakeDirectories("payload");
	Check(setenv("TAR_OPTIONS", "--no-such-option", 1) == 0, "setenv TAR_OPTIONS");

	auto result =
	    runner->Run(tar, {"-czf", package.string(), "-C", source.string(), "."}, temp.Path());
	CheckExit(result, 0, "tar -czf");
	Check(result.standard_error.empty(), "tar -czf: no standard error: " + result.standard_error);
	result = runner->Run(tar,
	                     {"-xzf", package.string(), "-C", payload.string(), "--no-same-owner",
	                      "--no-same-permissions"},
	                     temp.Path());
	CheckExit(result, 0, "tar -xzf");
	Check(result.standard_error.empty(), "tar -xzf: no standard error: " + result.standard_error);
	Check(UpdaterGen::SnapshotTree(payload) == UpdaterGen::SnapshotTree(source),
	      "the payload equals the source tree");

	// A package that is not gzip data: tar fails and says why on standard error.
	const auto broken = temp.WriteFile("broken.tar.gz", "this is not a gzip stream");
	result = runner->Run(tar, {"-xzf", broken.string(), "-C", payload.string()}, temp.Path());
	unsetenv("TAR_OPTIONS");
	Check(result.error.empty() && WIFEXITED(result.wait_status) &&
	          WEXITSTATUS(result.wait_status) != 0,
	      "tar fails on a broken package");
	Check(!result.standard_error.empty(), "tar explains the failure on standard error");
}

} // namespace

int main() {
	TestExitAndStandardError();
	TestStandardErrorLimit();
	TestChildState();
	TestWorkingDirectory();
	TestSpawnFailures();
	TestDetached();
	TestTarRoundTrip();
	std::printf("ProcessRunnerTests: all tests passed\n");
	return 0;
}
