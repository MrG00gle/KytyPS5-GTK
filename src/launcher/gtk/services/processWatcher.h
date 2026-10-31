#pragma once

// Watches the Emulator process that core::SpawnEmulator started (task 19.4; plan D21, HC-13;
// design.md "Launch and monitoring flow"; Requirements 10.5, 10.7, 10.11):
// - g_unix_fd_add on both non-blocking pipes: each readable pipe is read until EAGAIN (at most
//   1 MiB per wake-up, so a talkative child cannot starve the main loop) into
//   core::LogCollector::Append, then on_output runs; a pipe is closed at EOF or on a read error.
// - g_child_watch_add reaps the child. After the exit the watcher waits at most eof_wait for EOF on
//   both pipes (a grandchild may keep them open), reads what is left, closes them, calls
//   LogCollector::FlushPartialLines, and then on_exit with the waitpid() status, once.
// - Terminate() sends SIGTERM (core::SendTerminate) and, if the child has not exited kill_delay
//   later, SIGKILL (core::SendKill); Kill() sends SIGKILL at once. No signal is sent after the
//   child was reaped.
// The callbacks run on the main thread; on_output must not destroy the watcher, on_exit may.
// Destroying the watcher while the child runs sends no signal and calls no callback: it closes the
// pipes, and the child is reaped in the background when it exits. core::SpawnEmulator gives the
// child the default SIGPIPE action, so the Emulator is likely to die of SIGPIPE at its next write
// to stdout or stderr; stop it first (Terminate(), then wait for on_exit) to end it cleanly.

#include "launcher/core/emulatorProcess.h"
#include "launcher/core/logBuffer.h"
#include "launcher/gtk/gobjectPtr.h"

#include <array>
#include <chrono>
#include <functional>
#include <glib.h>
#include <sys/types.h>

namespace kyty::gtk {

class ProcessWatcher {
public:
	struct Callbacks {
		std::function<void()>                on_output; // new bytes reached the LogCollector
		std::function<void(int wait_status)> on_exit;   // after FlushPartialLines, once per Watch
	};

	ProcessWatcher(core::LogCollector& log, Callbacks callbacks,
	               std::chrono::milliseconds kill_delay = std::chrono::seconds(5),
	               std::chrono::milliseconds eof_wait   = std::chrono::seconds(1));
	~ProcessWatcher();

	ProcessWatcher(const ProcessWatcher&)            = delete;
	ProcessWatcher& operator=(const ProcessWatcher&) = delete;

	// Starts watching process and takes over its descriptors. Not while Running().
	void Watch(core::SpawnedProcess process);

	// SIGTERM now and SIGKILL after kill_delay unless the child has exited by then (a second call
	// sends SIGTERM again and keeps the first deadline).
	void Terminate();
	// SIGKILL now.
	void Kill();

	// From Watch() until on_exit runs.
	[[nodiscard]] bool Running() const { return m_running; }
	// The child exited (reaped); on_exit follows after the EOF wait.
	[[nodiscard]] bool  Exited() const { return m_exited; }
	[[nodiscard]] pid_t Pid() const { return m_pid; }

private:
	struct Pipe {
		ProcessWatcher* owner  = nullptr;
		core::Stream    stream = core::Stream::Stdout;
		int             fd     = -1;
		SourceId        source;
	};

	static gboolean OnReadable(gint fd, GIOCondition condition, gpointer data);
	static void     OnChildExited(GPid pid, gint wait_status, gpointer data);
	static gboolean OnKillTimeout(gpointer data);
	static gboolean OnEofTimeout(gpointer data);

	// Reads the pipe until EAGAIN, EOF, or the per-wake-up limit; returns whether bytes were
	// appended. At EOF or on a read error the pipe is closed.
	bool Drain(Pipe& pipe);
	void ClosePipe(Pipe& pipe);
	void FinishIfDone();
	void Finish();

	core::LogCollector&       m_log;
	Callbacks                 m_callbacks;
	std::chrono::milliseconds m_kill_delay;
	std::chrono::milliseconds m_eof_wait;

	pid_t               m_pid         = -1;
	bool                m_running     = false;
	bool                m_exited      = false;
	int                 m_wait_status = 0;
	std::array<Pipe, 2> m_pipes;
	SourceId            m_child_source;
	SourceId            m_kill_timer;
	SourceId            m_eof_timer;
};

} // namespace kyty::gtk
