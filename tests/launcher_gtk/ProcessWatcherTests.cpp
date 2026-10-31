// Example tests of the process watcher (src/launcher/gtk/services/processWatcher.h, task 19.4; plan
// D21, HC-13; Requirements 10.5, 10.7, 10.11) with children started by core::SpawnEmulator: the
// fake_emulator helper (tests/helpers/FakeEmulator.cpp) and /bin/sh. The lines of both streams
// reach the LogCollector in order (also invalid UTF-8 and a large output); an unterminated last
// line is flushed before on_exit; exit codes and signals arrive as waitpid() statuses; Terminate()
// ends a normal child with SIGTERM, and a child that ignores SIGTERM gets SIGKILL after kill_delay
// (a short delay and the default 5 s, both measured); Kill() sends SIGKILL; a grandchild that keeps
// standard output open makes the watcher finish after eof_wait (the default 1 s), not when the
// grandchild ends; a destroyed watcher calls nothing and the child is still reaped. No display.

#include "launcher/core/emulatorProcess.h"
#include "launcher/core/logBuffer.h"
#include "launcher/gtk/services/processWatcher.h"
#include "support/helperPaths.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <glib.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

namespace {

using kyty::core::LogCollector;
using kyty::gtk::ProcessWatcher;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "ProcessWatcherTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

long long Ms(Clock::duration duration) {
	return static_cast<long long>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

bool RunUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 20s) {
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

std::vector<std::string> Lines(const LogCollector& log) {
	std::vector<std::string> lines;
	const auto&              buffer = log.Lines();
	for (std::size_t i = 0; i < buffer.Size(); i++) {
		lines.push_back(buffer[i]);
	}
	return lines;
}

bool Contains(const std::vector<std::string>& lines, const std::string& line) {
	for (const auto& candidate: lines) {
		if (candidate == line) {
			return true;
		}
	}
	return false;
}

// One watched child: the log, the watcher, and what the callbacks saw.
struct Run {
	LogCollector                    log;
	std::unique_ptr<ProcessWatcher> watcher;
	int                             outputs = 0;
	int                             exits   = 0;
	int                             status  = 0;
	std::vector<std::string>        lines_at_exit;
	Clock::time_point               started;
	Clock::time_point               exited;

	// The watcher's default delays: kill_delay 5 s, eof_wait 1 s.
	Run() { watcher = std::make_unique<ProcessWatcher>(log, MakeCallbacks()); }

	Run(std::chrono::milliseconds kill_delay, std::chrono::milliseconds eof_wait) {
		watcher = std::make_unique<ProcessWatcher>(log, MakeCallbacks(), kill_delay, eof_wait);
	}

	Run(const Run&)            = delete;
	Run& operator=(const Run&) = delete;

	ProcessWatcher::Callbacks MakeCallbacks() {
		return {.on_output = [this] { outputs++; },
		        .on_exit =
		            [this](int wait_status) {
			            exits++;
			            status        = wait_status;
			            exited        = Clock::now();
			            lines_at_exit = Lines(log);
		            }};
	}

	void Start(const std::filesystem::path& executable, std::vector<std::string> args) {
		auto spawned = kyty::core::SpawnEmulator({executable, std::move(args), {}});
		Check(spawned.has_value(),
		      "the child starts: " + (spawned ? std::string() : spawned.error().message));
		started = Clock::now();
		watcher->Watch(*spawned);
		Check(watcher->Running(), "the watcher runs after Watch()");
	}

	void StartFake(std::vector<std::string> args) {
		Start(KytyTests::HelperPath("fake_emulator"), std::move(args));
	}

	void WaitForExit(const std::string& what, std::chrono::milliseconds limit = 20s) {
		Check(RunUntil([&] { return exits > 0; }, limit), "on_exit runs: " + what);
		RunFor(20ms);
		Check(exits == 1, "on_exit runs once: " + what);
		Check(!watcher->Running(), "the watcher no longer runs: " + what);
	}

	void WaitForLine(const std::string& line) {
		Check(RunUntil([&] { return Contains(Lines(log), line); }, 10s),
		      "the line arrives: " + line);
	}
};

void TestStreamsAndStatus() {
	// Interleaved lines of both streams keep each stream's order.
	std::vector<std::string> args;
	for (int i = 0; i < 300; i++) {
		args.push_back("--stdout");
		args.push_back("out " + std::to_string(i));
		args.push_back("--stderr");
		args.push_back("err " + std::to_string(i));
	}
	args.push_back("--stdout-hex");
	args.push_back("ff0a"); // invalid UTF-8 line
	args.push_back("--exit");
	args.push_back("3");
	Run run(5s, 1s);
	run.StartFake(args);
	run.WaitForExit("interleaved output");
	Check(WIFEXITED(run.status) && WEXITSTATUS(run.status) == 3, "exit code 3 is reported");
	Check(run.outputs > 0, "on_output runs");
	std::vector<std::string> out;
	std::vector<std::string> err;
	for (const auto& line: run.lines_at_exit) {
		if (line.rfind("out ", 0) == 0) {
			out.push_back(line);
		} else if (line.rfind("err ", 0) == 0) {
			err.push_back(line);
		}
	}
	Check(out.size() == 300 && err.size() == 300, "every line of both streams arrives");
	for (int i = 0; i < 300; i++) {
		Check(out[i] == "out " + std::to_string(i) && err[i] == "err " + std::to_string(i),
		      "each stream keeps its order");
	}
	Check(Contains(run.lines_at_exit, "\xEF\xBF\xBD"), "invalid UTF-8 becomes U+FFFD");

	// A large output (4 MiB: 65536 lines of 64 bytes) is read completely.
	const std::string line(63, 'a');
	Run               large(5s, 1s);
	large.Start("/bin/sh", {"-c", "yes " + line + " | head -n 65536"});
	large.WaitForExit("large output");
	Check(WIFEXITED(large.status) && WEXITSTATUS(large.status) == 0, "the large output ends");
	Check(large.lines_at_exit.size() == 65536 && large.lines_at_exit.front() == line &&
	          large.lines_at_exit.back() == line,
	      "4 MiB of output arrive as 65536 lines");

	// An unterminated last line is flushed before on_exit.
	Run partial(5s, 1s);
	partial.StartFake({"--stdout", "first", "--stdout-hex", "6c617374", "--stderr-hex", "657272"});
	partial.WaitForExit("unterminated lines");
	Check(Contains(partial.lines_at_exit, "first") && Contains(partial.lines_at_exit, "last") &&
	          Contains(partial.lines_at_exit, "err"),
	      "the unterminated last lines of both streams are in the log at on_exit");

	// A signal death.
	Run signalled(5s, 1s);
	signalled.StartFake({"--stdout", "raising", "--raise", std::to_string(SIGUSR1)});
	signalled.WaitForExit("raised signal");
	Check(WIFSIGNALED(signalled.status) && WTERMSIG(signalled.status) == SIGUSR1,
	      "a signal death is reported as such");
}

void TestTerminateAndKill() {
	// A normal child ends with SIGTERM; no SIGKILL follows.
	Run normal(3s, 1s);
	normal.StartFake({"--stdout", "ready", "--sleep-ms", "30000"});
	normal.WaitForLine("ready");
	const auto term_sent = Clock::now();
	normal.watcher->Terminate();
	normal.WaitForExit("SIGTERM");
	std::printf("ProcessWatcherTests: SIGTERM ended the child after %lld ms\n",
	            Ms(normal.exited - term_sent));
	Check(WIFSIGNALED(normal.status) && WTERMSIG(normal.status) == SIGTERM,
	      "Terminate() ends a normal child with SIGTERM");
	Check(normal.exited - term_sent < 2s, "the child ends before kill_delay");

	// A child that ignores SIGTERM gets SIGKILL after kill_delay.
	KytyTests::TempDir dir("kyty_watcher");
	const auto         record = dir.Path() / "signals";
	for (const auto delay: {std::chrono::milliseconds(400), std::chrono::milliseconds(5000)}) {
		// 5000 ms: the watcher's default kill_delay, without passing it.
		const bool default_delay = delay == 5000ms;
		auto stubborn = default_delay ? std::make_unique<Run>() : std::make_unique<Run>(delay, 1s);
		stubborn->StartFake({"--record-signals", record.string(), "--ignore-term", "--stdout",
		                     "ready", "--sleep-ms", "30000"});
		stubborn->WaitForLine("ready");
		const auto sent = Clock::now();
		stubborn->watcher->Terminate();
		stubborn->watcher->Terminate(); // a second call keeps the first deadline
		stubborn->WaitForExit("SIGKILL after SIGTERM", 20s);
		const auto waited = stubborn->exited - sent;
		std::printf("ProcessWatcherTests: SIGKILL followed SIGTERM after %lld ms (kill_delay %lld "
		            "ms%s)\n",
		            Ms(waited), static_cast<long long>(delay.count()),
		            default_delay ? ", the default" : "");
		Check(WIFSIGNALED(stubborn->status) && WTERMSIG(stubborn->status) == SIGKILL,
		      "a child that ignores SIGTERM ends with SIGKILL");
		Check(waited >= delay && waited < delay + 1500ms, "SIGKILL comes kill_delay after SIGTERM");
		std::ifstream     file(record);
		std::stringstream received;
		received << file.rdbuf();
		Check(received.str().rfind("15\n", 0) == 0, "the child received SIGTERM first");
		std::error_code ec;
		std::filesystem::remove(record, ec);
	}

	Run killed(5s, 1s);
	killed.StartFake({"--stdout", "ready", "--sleep-ms", "30000"});
	killed.WaitForLine("ready");
	killed.watcher->Kill();
	killed.WaitForExit("SIGKILL");
	Check(WIFSIGNALED(killed.status) && WTERMSIG(killed.status) == SIGKILL, "Kill() sends SIGKILL");
	killed.watcher->Terminate();
	killed.watcher->Kill();
	RunFor(50ms);
	Check(killed.exits == 1, "Terminate() and Kill() after the exit do nothing");
}

void TestEofWait() {
	// sh exits at once; the background sleep keeps both pipes open for 3 s. The default eof_wait.
	Run run;
	run.Start("/bin/sh", {"-c", "echo before; sleep 3 & exit 0"});
	Check(RunUntil([&] { return run.watcher->Exited(); }, 10s), "the shell exits");
	const auto exit_seen = Clock::now();
	run.WaitForExit("grandchild holding the pipes", 10s);
	const auto waited = run.exited - exit_seen;
	std::printf("ProcessWatcherTests: finished %lld ms after the exit (eof_wait 1000 ms) while a "
	            "grandchild held the pipes\n",
	            Ms(waited));
	Check(WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0, "the shell's status is reported");
	Check(waited >= 900ms && waited < 2500ms, "the watcher finishes after eof_wait, not after 3 s");
	Check(Contains(run.lines_at_exit, "before"), "the output before the exit is in the log");

	// Without a grandchild, EOF ends the wait at once.
	Run quick(5s, 1s);
	quick.Start("/bin/sh", {"-c", "echo done"});
	quick.WaitForExit("plain shell");
	Check(quick.exited - quick.started < 900ms, "EOF on both pipes ends the run without the wait");
}

void TestDestroyedWatcher() {
	auto run = std::make_unique<Run>(5s, 1s);
	run->StartFake({"--stdout", "ready", "--sleep-ms", "30000"});
	run->WaitForLine("ready");
	const pid_t pid   = run->watcher->Pid();
	int*        exits = &run->exits;
	run->watcher.reset();
	Check(*exits == 0, "destroying the watcher calls nothing");
	Check(kyty::core::SendKill(pid), "the child still exists");
	// The background reaper collects the child: a zombie still accepts kill(pid, 0), a reaped child
	// is gone (ESRCH). waitpid() here would compete with GLib's child watch.
	Check(RunUntil(
	          [&] {
		          errno = 0;
		          return kill(pid, 0) == -1 && errno == ESRCH;
	          },
	          5s),
	      "the child of a destroyed watcher is reaped");
	Check(*exits == 0, "no callback after the watcher is gone");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("ProcessWatcherTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestStreamsAndStatus();
	TestTerminateAndKill();
	TestEofWait();
	TestDestroyedWatcher();
	std::printf("ProcessWatcherTests: all tests passed\n");
	return 0;
}
