// Example tests of the POSIX Environment (src/launcher/core/environment.h) and of the
// FakeEnvironment that later tests use in its place (tests/support/fakeEnvironment.h).

#include "launcher/core/environment.h"
#include "support/fakeEnvironment.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using KytyTests::FakeEnvironment;
using KytyTests::TempDir;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "EnvironmentTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

void TestPosixGet() {
	const auto environment = kyty::core::MakePosixEnvironment();
	Check(environment != nullptr, "MakePosixEnvironment");

	Check(setenv("KYTY_ENVIRONMENT_TEST", "some value", 1) == 0, "setenv");
	Check(environment->Get("KYTY_ENVIRONMENT_TEST") == "some value", "a set variable");
	Check(setenv("KYTY_ENVIRONMENT_TEST", "", 1) == 0, "setenv empty");
	const auto empty = environment->Get("KYTY_ENVIRONMENT_TEST");
	Check(empty.has_value() && empty->empty(), "a set but empty variable is \"\"");
	Check(unsetenv("KYTY_ENVIRONMENT_TEST") == 0, "unsetenv");
	Check(!environment->Get("KYTY_ENVIRONMENT_TEST").has_value(), "an unset variable is nullopt");

	Check(setenv("KYTY_ENVIRONMENT_TEST", "prefix", 1) == 0, "setenv prefix");
	const std::string_view longer = "KYTY_ENVIRONMENT_TEST_LONGER";
	Check(environment->Get(longer.substr(0, 21)) == "prefix",
	      "a name view that is not NUL-terminated");
	Check(unsetenv("KYTY_ENVIRONMENT_TEST") == 0, "unsetenv prefix");

	Check(!environment->Get("").has_value(), "an empty name is never set");
	Check(!environment->Get("A=B").has_value(), "a name with '=' is never set");
	Check(setenv("KYTY_NUL", "x", 1) == 0, "setenv KYTY_NUL");
	Check(!environment->Get(std::string_view("KYTY_NUL\0", 9)).has_value(),
	      "a name with a NUL byte is never set");
	Check(unsetenv("KYTY_NUL") == 0, "unsetenv KYTY_NUL");

	// CTest unsets the display variables for these tests (Requirement 6.7).
	Check(!environment->Get("WAYLAND_DISPLAY").has_value() &&
	          !environment->Get("DISPLAY").has_value(),
	      "the display variables are unset");
}

void TestPosixFileExists() {
	const auto    environment = kyty::core::MakePosixEnvironment();
	const TempDir dir;
	const auto    file = dir.WriteFile("file.txt", "x");
	Check(environment->FileExists(file), "a file exists");
	Check(environment->FileExists(dir.Path()), "a directory exists");
	Check(!environment->FileExists(dir.Path() / "missing"), "a missing path does not exist");
	Check(!environment->FileExists(file / "child"), "a path below a file does not exist");
	Check(environment->FileExists(dir.MakeSymlink(file, "link.txt")), "a link to a file exists");
	Check(!environment->FileExists(dir.MakeSymlink("nowhere", "dangling")),
	      "a dangling link does not exist");
	Check(!environment->FileExists(""), "the empty path does not exist");
}

void TestFakeEnvironment() {
	FakeEnvironment environment;
	Check(!environment.Get("HOME").has_value(), "a fake starts without variables");
	environment.Set("HOME", "/home/fake");
	environment.Set("XDG_DATA_HOME", "");
	Check(environment.Get("HOME") == "/home/fake", "Set and Get");
	Check(environment.Get("XDG_DATA_HOME") == "", "a set but empty variable");
	environment.Set("HOME", "/home/other");
	Check(environment.Get("HOME") == "/home/other", "Set replaces");
	environment.Unset("HOME");
	environment.Unset("NEVER_SET");
	Check(!environment.Get("HOME").has_value(), "Unset");

	Check(!environment.FileExists("/.flatpak-info"), "no path exists at first");
	environment.AddExistingPath("/app/./bin/../share");
	Check(environment.FileExists("/app/share"), "paths compare in lexically normal form");
	Check(!environment.FileExists("/app"), "only the added path exists");
	environment.RemoveExistingPath("/app/share/");
	Check(environment.FileExists("/app/share"), "a trailing slash is another path");
	environment.RemoveExistingPath("/app/share");
	Check(!environment.FileExists("/app/share"), "RemoveExistingPath");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("EnvironmentTests");
	TestPosixGet();
	TestPosixFileExists();
	TestFakeEnvironment();
	std::printf("EnvironmentTests: all tests passed\n");
	return 0;
}
