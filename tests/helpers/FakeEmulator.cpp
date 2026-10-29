// fake_emulator (task 15.6): a configurable stand-in for the Emulator in spawn tests. Arguments are
// processed in order:
//
//   --stdout <text>, --stderr <text>   write text and "\n" to standard output or standard error
//   --stdout-hex <hex>, --stderr-hex <hex>
//                                      write raw bytes (two hexadecimal digits each): invalid
//                                      UTF-8, an unterminated last line
//   --sleep-ms <n>                     sleep n milliseconds (a caught signal does not shorten it)
//   --ignore-term                      SIGTERM no longer ends the process (ignored, or only
//   recorded
//                                      after --record-signals)
//   --record-signals <file>            catch SIGTERM, SIGINT, SIGHUP, and SIGUSR1 and append each
//                                      received signal number as a line to file; a caught SIGTERM
//                                      still ends the process (default action) unless --ignore-term
//                                      is given
//   --wait                             sleep until a signal is caught (one not consumed by an
//                                      earlier --wait; signals caught before --wait count), or
//                                      until a signal ends the process
//   --exit <code>                      exit with code (0-255) now
//   --raise <signal number>            raise the signal (1-64) with its current disposition
//   --print-cwd                        write the working directory and "\n" to standard output
//   --list-fds                         write the open descriptors, ascending and space-separated,
//                                      and "\n" to standard output
//   --describe-stdio                   write "stdin <dev-null|other|closed> <rdonly|wronly|rdwr>",
//                                      "stdout <blocking|nonblocking|closed>", and "stderr ..."
//                                      lines
//   --print-signal-state               write "SIGPIPE <default|ignored|handler>", "SIGTERM ...",
//   and
//                                      "blocked <none|signal numbers>" lines
//
// After the last argument it exits 0. An unknown argument, a missing value, or an invalid value
// exits 64 with a message on standard error. Writes go straight to the descriptors (no stdio
// buffering), so the order of the options is the order of the bytes. fake_emulator_selftest
// (tests/cmake/ProcessHelperSelfTest.cmake) checks every option. Standard library and POSIX only.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int kUsageError = 64;

volatile sig_atomic_t g_caught      = 0; // signals caught so far
volatile sig_atomic_t g_ignore_term = 0;
int                   g_record_fd   = -1;

bool WriteAll(int fd, std::string_view bytes) {
	while (!bytes.empty()) {
		const ssize_t written = ::write(fd, bytes.data(), bytes.size());
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		bytes.remove_prefix(static_cast<size_t>(written));
	}
	return true;
}

// Async-signal-safe: write(), signal(), and raise() only.
void OnSignal(int signal_number) {
	const int saved_errno = errno;
	char      line[8];
	size_t    length = 0;
	char      digits[4];
	size_t    count = 0;
	for (int value = signal_number; value > 0 && count < sizeof(digits); value /= 10) {
		digits[count++] = static_cast<char>('0' + value % 10);
	}
	while (count > 0) {
		line[length++] = digits[--count];
	}
	line[length++] = '\n';
	if (g_record_fd >= 0) {
		[[maybe_unused]] const ssize_t written = ::write(g_record_fd, line, length);
	}
	g_caught = g_caught + 1;
	if (signal_number == SIGTERM && g_ignore_term == 0) {
		::signal(SIGTERM, SIG_DFL);
		::raise(SIGTERM); // pending until this handler returns, then the default action
	}
	errno = saved_errno;
}

constexpr int kRecordedSignals[] = {SIGTERM, SIGINT, SIGHUP, SIGUSR1};

sigset_t RecordedSignalSet() {
	sigset_t set;
	sigemptyset(&set);
	for (const int signal_number: kRecordedSignals) {
		sigaddset(&set, signal_number);
	}
	return set;
}

std::optional<long> ParseNumber(std::string_view text, long min, long max) {
	if (text.empty() || text.size() > 10) {
		return std::nullopt;
	}
	long value = 0;
	for (const char c: text) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		value = value * 10 + (c - '0');
	}
	if (value < min || value > max) {
		return std::nullopt;
	}
	return value;
}

std::optional<std::string> ParseHex(std::string_view text) {
	if (text.size() % 2 != 0) {
		return std::nullopt;
	}
	const auto digit = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	};
	std::string bytes;
	for (size_t i = 0; i < text.size(); i += 2) {
		const int high = digit(text[i]);
		const int low  = digit(text[i + 1]);
		if (high < 0 || low < 0) {
			return std::nullopt;
		}
		bytes.push_back(static_cast<char>(high * 16 + low));
	}
	return bytes;
}

void SleepMilliseconds(long milliseconds) {
	timespec remaining {};
	remaining.tv_sec  = milliseconds / 1000;
	remaining.tv_nsec = (milliseconds % 1000) * 1000000L;
	while (::nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
	}
}

bool InstallRecorder(const char* file) {
	g_record_fd = ::open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (g_record_fd < 0) {
		return false;
	}
	struct sigaction action {};
	action.sa_handler = OnSignal;
	action.sa_mask    = RecordedSignalSet();
	action.sa_flags   = 0;
	for (const int signal_number: kRecordedSignals) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			return false;
		}
	}
	return true;
}

// Returns once g_caught exceeds consumed; the signals stay blocked except inside sigsuspend(), so
// a signal that arrives just before the check is not lost.
void WaitForSignal(sig_atomic_t& consumed) {
	const sigset_t block = RecordedSignalSet();
	sigset_t       previous;
	::sigprocmask(SIG_BLOCK, &block, &previous);
	while (g_caught <= consumed) {
		::sigsuspend(&previous);
	}
	consumed = g_caught;
	::sigprocmask(SIG_SETMASK, &previous, nullptr);
}

std::string ListDescriptors() {
	std::vector<int> descriptors;
	DIR*             directory = ::opendir("/proc/self/fd");
	if (directory == nullptr) {
		return "error\n";
	}
	const int own = ::dirfd(directory);
	while (const dirent* entry = ::readdir(directory)) {
		const auto number = ParseNumber(entry->d_name, 0, 1L << 30);
		if (number && *number != own) {
			descriptors.push_back(static_cast<int>(*number));
		}
	}
	::closedir(directory);
	std::sort(descriptors.begin(), descriptors.end());
	std::string text;
	for (const int descriptor: descriptors) {
		text += (text.empty() ? "" : " ") + std::to_string(descriptor);
	}
	return text + "\n";
}

std::string DescribeStdio() {
	std::string text = "stdin ";
	struct stat in {};
	struct stat null {};
	if (::fstat(STDIN_FILENO, &in) != 0) {
		text += "closed\n";
	} else {
		const bool dev_null =
		    ::stat("/dev/null", &null) == 0 && S_ISCHR(in.st_mode) && in.st_rdev == null.st_rdev;
		text += dev_null ? "dev-null " : "other ";
		const int mode = ::fcntl(STDIN_FILENO, F_GETFL) & O_ACCMODE;
		text += mode == O_RDONLY ? "rdonly\n" : mode == O_WRONLY ? "wronly\n" : "rdwr\n";
	}
	for (const auto& [fd, name]:
	     {std::pair {STDOUT_FILENO, "stdout"}, std::pair {STDERR_FILENO, "stderr"}}) {
		const int flags = ::fcntl(fd, F_GETFL);
		text += std::string(name) + (flags < 0                   ? " closed\n"
		                             : (flags & O_NONBLOCK) != 0 ? " nonblocking\n"
		                                                         : " blocking\n");
	}
	return text;
}

std::string DescribeSignals() {
	std::string text;
	for (const auto& [signal_number, name]:
	     {std::pair {SIGPIPE, "SIGPIPE"}, std::pair {SIGTERM, "SIGTERM"}}) {
		struct sigaction action {};
		::sigaction(signal_number, nullptr, &action);
		text += std::string(name) + (action.sa_handler == SIG_DFL   ? " default\n"
		                             : action.sa_handler == SIG_IGN ? " ignored\n"
		                                                            : " handler\n");
	}
	sigset_t mask;
	::sigprocmask(SIG_SETMASK, nullptr, &mask);
	std::string blocked;
	for (int signal_number = 1; signal_number < NSIG; signal_number++) {
		if (sigismember(&mask, signal_number) == 1) {
			blocked += " " + std::to_string(signal_number);
		}
	}
	return text + "blocked" + (blocked.empty() ? " none" : blocked) + "\n";
}

int UsageError(const std::string& message) {
	const std::string text = "fake_emulator: " + message + "\n";
	WriteAll(STDERR_FILENO, text);
	return kUsageError;
}

int Run(int argc, char** argv) {
	sig_atomic_t consumed  = 0;
	bool         recording = false;
	for (int i = 1; i < argc; i++) {
		const std::string_view option    = argv[i];
		const bool             has_value = i + 1 < argc;
		const auto             value     = [&]() { return std::string_view(argv[++i]); };
		const bool takes_value           = option == "--stdout" || option == "--stderr" ||
		                                   option == "--stdout-hex" || option == "--stderr-hex" ||
		                                   option == "--sleep-ms" || option == "--record-signals" ||
		                                   option == "--exit" || option == "--raise";
		if (takes_value && !has_value) {
			return UsageError(std::string(option) + " needs a value");
		}
		if (option == "--stdout" || option == "--stderr") {
			const int fd = option == "--stdout" ? STDOUT_FILENO : STDERR_FILENO;
			if (!WriteAll(fd, std::string(value()) + "\n")) {
				return 1;
			}
		} else if (option == "--stdout-hex" || option == "--stderr-hex") {
			const int  fd    = option == "--stdout-hex" ? STDOUT_FILENO : STDERR_FILENO;
			const auto bytes = ParseHex(value());
			if (!bytes) {
				return UsageError("invalid hexadecimal bytes '" + std::string(argv[i]) + "'");
			}
			if (!WriteAll(fd, *bytes)) {
				return 1;
			}
		} else if (option == "--sleep-ms") {
			const auto milliseconds = ParseNumber(value(), 0, 3600L * 1000L);
			if (!milliseconds) {
				return UsageError("invalid --sleep-ms value '" + std::string(argv[i]) + "'");
			}
			SleepMilliseconds(*milliseconds);
		} else if (option == "--ignore-term") {
			g_ignore_term = 1;
			if (!recording) {
				::signal(SIGTERM, SIG_IGN);
			}
		} else if (option == "--record-signals") {
			const std::string file(value());
			if (!InstallRecorder(file.c_str())) {
				return UsageError("cannot record signals in '" + file +
				                  "': " + std::strerror(errno));
			}
			recording = true;
		} else if (option == "--wait") {
			WaitForSignal(consumed);
		} else if (option == "--exit") {
			const auto code = ParseNumber(value(), 0, 255);
			if (!code) {
				return UsageError("invalid --exit value '" + std::string(argv[i]) + "'");
			}
			return static_cast<int>(*code);
		} else if (option == "--raise") {
			const auto signal_number = ParseNumber(value(), 1, NSIG - 1);
			if (!signal_number) {
				return UsageError("invalid --raise value '" + std::string(argv[i]) + "'");
			}
			::raise(static_cast<int>(*signal_number));
		} else if (option == "--print-cwd") {
			std::vector<char> buffer(65536);
			if (::getcwd(buffer.data(), buffer.size()) == nullptr ||
			    !WriteAll(STDOUT_FILENO, std::string(buffer.data()) + "\n")) {
				return 1;
			}
		} else if (option == "--list-fds") {
			if (!WriteAll(STDOUT_FILENO, ListDescriptors())) {
				return 1;
			}
		} else if (option == "--describe-stdio") {
			if (!WriteAll(STDOUT_FILENO, DescribeStdio())) {
				return 1;
			}
		} else if (option == "--print-signal-state") {
			if (!WriteAll(STDOUT_FILENO, DescribeSignals())) {
				return 1;
			}
		} else {
			return UsageError("unknown argument '" + std::string(option) + "'");
		}
	}
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	return Run(argc, argv);
}
