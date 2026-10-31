#include "launcher/gtk/services/processWatcher.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <glib-unix.h>
#include <span>
#include <unistd.h>
#include <utility>

namespace kyty::gtk {

namespace {

constexpr std::size_t kReadSize = 64 * 1024;
// Reads per wake-up of a pipe source (1 MiB), so that the main loop keeps running.
constexpr int kReadsPerWakeUp = 16;

guint Milliseconds(std::chrono::milliseconds duration) {
	return static_cast<guint>(
	    std::clamp<int64_t>(duration.count(), 0, static_cast<int64_t>(G_MAXUINT)));
}

// Reaps a child whose watcher is gone.
void ReapInBackground(GPid pid, gint /*wait_status*/, gpointer /*data*/) {
	g_spawn_close_pid(pid);
}

} // namespace

ProcessWatcher::ProcessWatcher(core::LogCollector& log, Callbacks callbacks,
                               std::chrono::milliseconds kill_delay,
                               std::chrono::milliseconds eof_wait)
    : m_log(log), m_callbacks(std::move(callbacks)), m_kill_delay(kill_delay),
      m_eof_wait(eof_wait) {
	m_pipes[0].owner  = this;
	m_pipes[0].stream = core::Stream::Stdout;
	m_pipes[1].owner  = this;
	m_pipes[1].stream = core::Stream::Stderr;
}

ProcessWatcher::~ProcessWatcher() {
	m_kill_timer.Remove();
	m_eof_timer.Remove();
	for (auto& pipe: m_pipes) {
		ClosePipe(pipe);
	}
	if (m_running && !m_exited) {
		m_child_source.Remove();
		g_child_watch_add(m_pid, &ReapInBackground, nullptr);
	}
}

void ProcessWatcher::Watch(core::SpawnedProcess process) {
	g_return_if_fail(!m_running);
	g_return_if_fail(process.pid > 0);
	m_pid         = process.pid;
	m_running     = true;
	m_exited      = false;
	m_wait_status = 0;
	m_pipes[0].fd = process.stdout_fd;
	m_pipes[1].fd = process.stderr_fd;
	for (auto& pipe: m_pipes) {
		if (pipe.fd >= 0) {
			pipe.source = SourceId(g_unix_fd_add(
			    pipe.fd, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), &OnReadable, &pipe));
		}
	}
	m_child_source = SourceId(g_child_watch_add(m_pid, &OnChildExited, this));
}

void ProcessWatcher::Terminate() {
	if (!m_running || m_exited) {
		return;
	}
	if (!core::SendTerminate(m_pid)) {
		g_message("cannot send SIGTERM to process %d: %s", static_cast<int>(m_pid),
		          g_strerror(errno));
	}
	if (!m_kill_timer.Active()) {
		m_kill_timer = SourceId(g_timeout_add(Milliseconds(m_kill_delay), &OnKillTimeout, this));
	}
}

void ProcessWatcher::Kill() {
	if (!m_running || m_exited) {
		return;
	}
	if (!core::SendKill(m_pid)) {
		g_message("cannot send SIGKILL to process %d: %s", static_cast<int>(m_pid),
		          g_strerror(errno));
	}
}

gboolean ProcessWatcher::OnReadable(gint /*fd*/, GIOCondition /*condition*/, gpointer data) {
	auto*      pipe     = static_cast<Pipe*>(data);
	auto*      self     = pipe->owner;
	const bool appended = self->Drain(*pipe);
	const bool open     = pipe->fd >= 0;
	if (appended && self->m_callbacks.on_output) {
		self->m_callbacks.on_output();
	}
	if (!open) {
		// Drain() removed this source with the pipe.
		self->FinishIfDone();
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

void ProcessWatcher::OnChildExited(GPid pid, gint wait_status, gpointer data) {
	auto* self          = static_cast<ProcessWatcher*>(data);
	self->m_exited      = true;
	self->m_wait_status = wait_status;
	g_spawn_close_pid(pid);
	self->m_kill_timer.Remove();
	if (self->m_pipes[0].fd < 0 && self->m_pipes[1].fd < 0) {
		self->Finish();
		return;
	}
	self->m_eof_timer =
	    SourceId(g_timeout_add(Milliseconds(self->m_eof_wait), &OnEofTimeout, self));
}

gboolean ProcessWatcher::OnKillTimeout(gpointer data) {
	auto* self = static_cast<ProcessWatcher*>(data);
	// The source is removed when the child is reaped, so the pid still belongs to it here.
	self->Kill();
	return G_SOURCE_REMOVE;
}

gboolean ProcessWatcher::OnEofTimeout(gpointer data) {
	auto* self = static_cast<ProcessWatcher*>(data);
	// What is still in the pipes counts; a grandchild may keep writing, but the watcher stops here.
	for (auto& pipe: self->m_pipes) {
		if (pipe.fd >= 0) {
			self->Drain(pipe);
			self->ClosePipe(pipe);
		}
	}
	self->Finish();
	return G_SOURCE_REMOVE;
}

bool ProcessWatcher::Drain(Pipe& pipe) {
	std::array<std::byte, kReadSize> buffer {};
	bool                             appended = false;
	for (int reads = 0; reads < kReadsPerWakeUp && pipe.fd >= 0;) {
		const ssize_t count = ::read(pipe.fd, buffer.data(), buffer.size());
		if (count > 0) {
			m_log.Append(pipe.stream, std::span(buffer.data(), static_cast<std::size_t>(count)));
			appended = true;
			reads++;
			continue;
		}
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		if (count < 0) {
			g_message("cannot read the output of process %d: %s", static_cast<int>(m_pid),
			          g_strerror(errno));
		}
		ClosePipe(pipe); // EOF or a read error
	}
	return appended;
}

void ProcessWatcher::ClosePipe(Pipe& pipe) {
	pipe.source.Remove();
	if (pipe.fd >= 0) {
		::close(pipe.fd);
		pipe.fd = -1;
	}
}

void ProcessWatcher::FinishIfDone() {
	if (m_running && m_exited && m_pipes[0].fd < 0 && m_pipes[1].fd < 0) {
		Finish();
	}
}

void ProcessWatcher::Finish() {
	m_eof_timer.Remove();
	m_kill_timer.Remove();
	m_child_source.Remove();
	for (auto& pipe: m_pipes) {
		ClosePipe(pipe);
	}
	m_log.FlushPartialLines();
	m_running = false;
	// on_exit may destroy the watcher: nothing of it is used afterwards.
	const auto on_exit     = m_callbacks.on_exit;
	const int  wait_status = m_wait_status;
	if (on_exit) {
		on_exit(wait_status);
	}
}

} // namespace kyty::gtk
