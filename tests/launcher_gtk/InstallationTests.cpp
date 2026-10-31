// Example tests of the installation helpers (src/launcher/gtk/installation.h, task 19.3; plan D15;
// review p6-u2-4): EmulatorVersionFromOutput (the exe_name line first, CR/LF, empty lines, one line
// only, invalid UTF-8), LocateEmulator (<dir>/kyty_emulator, <dir>/../kyty_emulator, neither), and
// EmulatorVersionProbe with scripts written at test time (a two-line answer, a sleeping script cut
// off at the limit, no callback after the probe is destroyed, an executable that cannot start). No
// display.

#include "launcher/gtk/installation.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <glib.h>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using kyty::gtk::EmulatorVersionFromOutput;
using kyty::gtk::EmulatorVersionProbe;
using kyty::gtk::LocateEmulator;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "InstallationTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

bool RunUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s) {
	const gint64 deadline = g_get_monotonic_time() + limit.count() * 1000;
	while (!done()) {
		if (g_get_monotonic_time() >= deadline) {
			return false;
		}
		g_main_context_iteration(nullptr, FALSE);
		g_usleep(1000);
	}
	return true;
}

void RunFor(std::chrono::milliseconds duration) {
	RunUntil([] { return false; }, duration);
}

// A script with mode 0755 from the start (no chmod).
std::filesystem::path WriteScript(const std::filesystem::path& path, std::string_view text) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	Check(fd >= 0, "the script can be created: " + path.string());
	const ssize_t written = ::write(fd, text.data(), text.size());
	Check(written == static_cast<ssize_t>(text.size()), "the script is written");
	Check(::close(fd) == 0, "the script is closed");
	return path;
}

void TestVersionFromOutput() {
	const std::string kReplacement = "\xEF\xBF\xBD";
	Check(EmulatorVersionFromOutput("exe_name: kyty_emulator\nv1.2.3\n") == "v1.2.3",
	      "the line after the exe_name line");
	Check(EmulatorVersionFromOutput("exe_name: kyty_emulator\r\nv2.0\r\n") == "v2.0",
	      "CR/LF line ends");
	Check(EmulatorVersionFromOutput("\n\nexe_name: x\n\n\r\nv3\n\nignored\n") == "v3",
	      "empty lines are skipped");
	Check(EmulatorVersionFromOutput("KytyPS5 0.4") == "KytyPS5 0.4",
	      "one line without exe_name is the version");
	Check(EmulatorVersionFromOutput("exe_name: only\n").empty(),
	      "an exe_name line alone gives no version");
	Check(EmulatorVersionFromOutput("first\nsecond\n") == "first",
	      "without exe_name the first line is the version");
	Check(EmulatorVersionFromOutput("").empty() && EmulatorVersionFromOutput("\r\n\n").empty(),
	      "no line gives no version");
	Check(EmulatorVersionFromOutput("exe_name\nv\xff"
	                                "1\n") == "v" + kReplacement + "1",
	      "an invalid byte becomes U+FFFD");
	Check(EmulatorVersionFromOutput("v\xc3") == "v" + kReplacement,
	      "a truncated sequence becomes U+FFFD");
}

void TestLocateEmulator() {
	KytyTests::TempDir dir("kyty_installation");
	const auto         launcher = dir.MakeDirectories("install/bin");

	const auto none = LocateEmulator(launcher);
	Check(!none.found && none.executable == (launcher / "kyty_emulator").lexically_normal(),
	      "neither candidate: the first one, not found");

	dir.WriteFile("install/kyty_emulator", "parent");
	const auto parent = LocateEmulator(launcher);
	Check(parent.found &&
	          parent.executable == (dir.Path() / "install/kyty_emulator").lexically_normal(),
	      "<dir>/../kyty_emulator when only it exists (lexically normal)");

	dir.WriteFile("install/bin/kyty_emulator", "same");
	const auto same = LocateEmulator(launcher);
	Check(same.found && same.executable == (launcher / "kyty_emulator").lexically_normal(),
	      "<dir>/kyty_emulator comes first");

	const auto trailing = LocateEmulator(launcher / "");
	Check(trailing.found && trailing.executable == same.executable,
	      "a trailing separator changes nothing");
}

void TestVersionProbe() {
	KytyTests::TempDir dir("kyty_probe");

	// A two-line answer.
	const auto answer = WriteScript(dir.Path() / "answer",
	                                "#!/bin/sh\necho 'exe_name: kyty_emulator'\necho 'v9.8.7'\n");
	std::optional<std::string> version;
	auto probe = EmulatorVersionProbe::Start(answer, [&](std::string text) { version = text; });
	Check(probe != nullptr, "the probe starts");
	Check(!version.has_value(), "the callback never runs inside Start()");
	Check(RunUntil([&] { return version.has_value(); }), "the probe reports");
	Check(*version == "v9.8.7", "the version is the second line");
	probe.reset();

	// A script that sleeps past the limit gives "" at the limit.
	const auto sleeper = WriteScript(dir.Path() / "sleeper", "#!/bin/sh\nexec sleep 30\n");
	std::optional<std::string> late;
	const auto                 start = Clock::now();
	auto slow = EmulatorVersionProbe::Start(sleeper, [&](std::string text) { late = text; }, 300ms);
	Check(slow != nullptr, "the sleeping probe starts");
	Check(RunUntil([&] { return late.has_value(); }, 5s), "the limit ends the probe");
	const auto waited = Clock::now() - start;
	std::printf("InstallationTests: the probe ended %lld ms after its start (limit 300 ms)\n",
	            static_cast<long long>(
	                std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()));
	Check(late->empty(), "a probe cut off at the limit reports \"\"");
	Check(waited >= 300ms && waited < 3s, "the probe ends at its limit, not when the script ends");
	slow.reset();

	// Destroying the probe kills the process; the callback never runs.
	bool called = false;
	auto gone   = EmulatorVersionProbe::Start(sleeper, [&](std::string) { called = true; });
	Check(gone != nullptr, "the probe to destroy starts");
	gone.reset();
	RunFor(500ms);
	Check(!called, "no callback after the probe is destroyed");

	// The callback may destroy the probe.
	std::unique_ptr<EmulatorVersionProbe> self;
	bool                                  reported = false;
	self = EmulatorVersionProbe::Start(answer, [&](std::string) {
		self.reset();
		reported = true;
	});
	Check(RunUntil([&] { return reported; }), "the self-destroying probe reports");
	Check(self == nullptr, "the probe is gone");

	// An executable that cannot start: no probe.
	Check(EmulatorVersionProbe::Start(dir.Path() / "missing", [](std::string) {}) == nullptr,
	      "a missing executable gives no probe");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("InstallationTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestVersionFromOutput();
	TestLocateEmulator();
	TestVersionProbe();
	std::printf("InstallationTests: all tests passed\n");
	return 0;
}
