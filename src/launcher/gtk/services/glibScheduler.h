#pragma once

// core::Scheduler on the GLib main loop (task 19.4; plan D20): the timers of Launcher_Core and the
// Updater (compatibility retries, update session steps) run as g_timeout_add_full sources of the
// default main context. It keeps the contract of launcher/core/scheduler.h:
// - After(delay, fn) runs fn once from the main loop, never inside After(); a zero or negative
//   delay becomes 0, which still runs later. Timers with equal delays run in the order of their
//   After() calls.
// - Cancel(id) before fn has run removes the source, so fn never runs; Cancel() of a timer that has
//   fired, of an unknown id, or of 0 does nothing.
// - Ids come from a 64-bit counter that starts at 1, so they are never 0 and never reused.
// - The destructor removes every pending timer; fn may destroy the scheduler.

#include "launcher/core/scheduler.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <glib.h>
#include <map>
#include <memory>

namespace kyty::gtk {

class GlibScheduler final: public core::Scheduler {
public:
	GlibScheduler();
	~GlibScheduler() override;

	GlibScheduler(const GlibScheduler&)            = delete;
	GlibScheduler& operator=(const GlibScheduler&) = delete;

	core::TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) override;
	void          Cancel(core::TimerId id) override;

	// Timers that have neither fired nor been cancelled.
	[[nodiscard]] std::size_t PendingCount() const { return m_timers.size(); }

private:
	struct Timer;

	static gboolean OnTimeout(gpointer data);

	core::TimerId                                   m_next_id = 1;
	std::map<core::TimerId, std::unique_ptr<Timer>> m_timers;
};

} // namespace kyty::gtk
