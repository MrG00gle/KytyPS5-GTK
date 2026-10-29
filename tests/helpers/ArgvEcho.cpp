// argv_echo (task 15.6): writes the bytes of every argument after argv[0] to standard output, each
// followed by one NUL byte, and exits 0. No arguments give no output, and an empty argument gives a
// single NUL, so the output identifies the argument vector exactly. Spawn tests and Property 22 run
// it in place of the Emulator; argv_echo_selftest (tests/cmake/ProcessHelperSelfTest.cmake) checks
// it. Standard library and POSIX only. Exits 1 when standard output cannot be written.

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

bool WriteAll(int fd, const char* data, size_t size) {
	while (size > 0) {
		const ssize_t written = ::write(fd, data, size);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		data += written;
		size -= static_cast<size_t>(written);
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	std::string output;
	for (int i = 1; i < argc; i++) {
		output.append(argv[i], std::strlen(argv[i]));
		output.push_back('\0');
	}
	return WriteAll(STDOUT_FILENO, output.data(), output.size()) ? 0 : 1;
}
