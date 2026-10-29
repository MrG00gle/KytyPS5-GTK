// Feature: linux-gtk-modernization, Property 22: Game path bytes are preserved
//
// Validates Requirement 10.3. Each case draws a length from 1 to 4,095 bytes and creates a
// directory path of exactly that length below a short temporary root (/tmp/kyty-p22-XXXXXX, 20
// bytes), with components of 1-255 bytes (NAME_MAX) made of letters, digits, spaces, single and
// double quotes, backslashes, backticks, the shell metacharacters $ ; & | < > * ? ( ) [ ] { } ! #
// ~, and 2-, 3-, and 4-byte UTF-8 characters. A length below 22 bytes cannot be built below the
// root and is raised to 22; the classification line reports how often (about 0.5% of the draws).
// The path, which exists, is passed as --game <path> to SpawnEmulator, with the argv_echo helper
// standing in for the Emulator; argv_echo must receive exactly the two arguments, byte for byte.

#include "launcher/core/emulatorProcess.h"
#include "support/helperPaths.h"
#include "support/property.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxPathBytes      = 4095; // PATH_MAX without the NUL
constexpr size_t kMaxComponentBytes = 255;  // NAME_MAX

int Choose(int min, int max) {
	return *rc::gen::resize(100, rc::gen::inRange(min, max));
}

// The short temporary root, removed with everything below it.
class ShortRoot {
public:
	ShortRoot() {
		std::string pattern = "/tmp/kyty-p22-XXXXXX";
		if (mkdtemp(pattern.data()) != nullptr) {
			m_path = pattern;
		}
	}
	~ShortRoot() {
		std::error_code ec;
		fs::remove_all(m_path, ec);
	}
	ShortRoot(const ShortRoot&)            = delete;
	ShortRoot& operator=(const ShortRoot&) = delete;

	[[nodiscard]] const std::string& Path() const { return m_path; }

private:
	std::string m_path;
};

// Reaps the child (killing it first if needed) and closes its descriptors on every exit path.
class ChildGuard {
public:
	explicit ChildGuard(const kyty::core::SpawnedProcess& process): m_process(process) {}
	~ChildGuard() {
		CloseOutput();
		if (!m_reaped) {
			kill(m_process.pid, SIGKILL);
			int status = 0;
			waitpid(m_process.pid, &status, 0);
		}
	}
	ChildGuard(const ChildGuard&)            = delete;
	ChildGuard& operator=(const ChildGuard&) = delete;

	// Everything the child writes to standard output; false after 10 s.
	bool ReadStdout(std::string& out) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		std::array<char, 65536> buffer {};
		while (std::chrono::steady_clock::now() < deadline) {
			pollfd fd {m_process.stdout_fd, POLLIN, 0};
			if (poll(&fd, 1, 100) < 0 && errno != EINTR) {
				return false;
			}
			for (;;) {
				const ssize_t count = read(m_process.stdout_fd, buffer.data(), buffer.size());
				if (count > 0) {
					out.append(buffer.data(), static_cast<size_t>(count));
				} else if (count == 0) {
					return true;
				} else if (errno != EINTR) {
					break; // EAGAIN: poll again
				}
			}
		}
		return false;
	}

	int Wait() {
		int status = 0;
		waitpid(m_process.pid, &status, 0);
		m_reaped = true;
		return status;
	}

	void CloseOutput() {
		for (int* fd: {&m_process.stdout_fd, &m_process.stderr_fd}) {
			if (*fd >= 0) {
				close(*fd);
				*fd = -1;
			}
		}
	}

private:
	kyty::core::SpawnedProcess m_process;
	bool                       m_reaped = false;
};

// A component of exactly length bytes (1-255), never "." or "..".
std::string Component(size_t length) {
	static const std::string              kAscii   = "abcXYZ019 '\"\\`$;&|<>*?()[]{}!#~-_.,=+%@^:";
	static const std::vector<std::string> kUnicode = {"\xC3\xA9", "\xD0\x96", "\xE6\x97\xA5",
	                                                  "\xE2\x82\xAC", "\xF0\x9F\x8E\xAE"};
	std::string                           text;
	while (text.size() < length) {
		const size_t room = length - text.size();
		if (Choose(0, 4) == 0) {
			const auto& character = kUnicode[static_cast<size_t>(Choose(0, 5))];
			if (character.size() <= room) {
				text += character;
				continue;
			}
		}
		text += kAscii[static_cast<size_t>(Choose(0, static_cast<int>(kAscii.size())))];
	}
	if (text == "." || text == "..") {
		text[0] = 'x';
	}
	return text;
}

} // namespace

KYTY_PROPERTY(22, "Game path bytes are preserved") {
	return rc::check(name, []() {
		const ShortRoot root;
		RC_ASSERT(!root.Path().empty());
		const size_t minimum = root.Path().size() + 2; // root + "/" + one byte
		size_t       length  = static_cast<size_t>(Choose(1, static_cast<int>(kMaxPathBytes) + 1));
		const bool   raised  = length < minimum;
		if (raised) {
			length = minimum;
		}

		// "/" + component pieces that fill the length exactly.
		std::string path      = root.Path();
		size_t      remaining = length - path.size();
		while (remaining > 0) {
			size_t component = 0;
			if (remaining - 1 <= kMaxComponentBytes) {
				component = remaining - 1;
			} else {
				component =
				    static_cast<size_t>(Choose(1, static_cast<int>(kMaxComponentBytes) + 1));
				if (remaining - (component + 1) == 1) {
					component = component == kMaxComponentBytes ? component - 1 : component + 1;
				}
			}
			path += "/" + Component(component);
			remaining -= component + 1;
		}
		RC_ASSERT(path.size() == length);

		std::error_code ec;
		fs::create_directories(path, ec);
		RC_ASSERT(!ec);
		RC_ASSERT(fs::is_directory(path, ec));

		const std::vector<std::string> args    = {"--game", path};
		auto                           spawned = kyty::core::SpawnEmulator(
		    kyty::core::SpawnRequest {KytyTests::HelperPath("argv_echo"), args, root.Path()});
		RC_ASSERT(spawned.has_value());
		ChildGuard  child(*spawned);
		std::string out;
		RC_ASSERT(child.ReadStdout(out));
		const int status = child.Wait();
		RC_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

		std::string expected = "--game";
		expected.push_back('\0');
		expected += path;
		expected.push_back('\0');
		RC_ASSERT(out == expected);

		RC_CLASSIFY(raised, "length raised to the root minimum");
		RC_TAG(length < 256    ? "1-255 bytes"
		       : length < 1024 ? "256-1023 bytes"
		       : length < 2048 ? "1024-2047 bytes"
		                       : "2048-4095 bytes");
	});
}
