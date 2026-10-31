#include "launcher/gtk/services/glibScheduler.h"

#include "launcher/gtk/gobjectPtr.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace kyty::gtk {

struct GlibScheduler::Timer {
	GlibScheduler*        owner = nullptr;
	core::TimerId         id    = 0;
	std::function<void()> fn;
	SourceId              source;
};

GlibScheduler::GlibScheduler() = default;

// Destroying the timers removes their sources.
GlibScheduler::~GlibScheduler() = default;

core::TimerId GlibScheduler::After(std::chrono::milliseconds delay, std::function<void()> fn) {
	const core::TimerId id    = m_next_id++;
	auto                timer = std::make_unique<Timer>();
	timer->owner              = this;
	timer->id                 = id;
	timer->fn                 = std::move(fn);
	const auto interval =
	    static_cast<guint>(std::clamp<int64_t>(delay.count(), 0, static_cast<int64_t>(G_MAXUINT)));
	timer->source = SourceId(
	    g_timeout_add_full(G_PRIORITY_DEFAULT, interval, &OnTimeout, timer.get(), nullptr));
	m_timers.emplace(id, std::move(timer));
	return id;
}

void GlibScheduler::Cancel(core::TimerId id) {
	// Erasing the timer removes its source (an unknown id or 0 finds nothing).
	m_timers.erase(id);
}

gboolean GlibScheduler::OnTimeout(gpointer data) {
	auto*      timer = static_cast<Timer*>(data);
	auto*      owner = timer->owner;
	const auto found = owner->m_timers.find(timer->id);
	// The timer leaves the map before fn runs: fn may schedule, cancel, or destroy the scheduler.
	std::unique_ptr<Timer> fired = std::move(found->second);
	owner->m_timers.erase(found);
	std::function<void()> fn = std::move(fired->fn);
	fired.reset();
	if (fn) {
		fn();
	}
	return G_SOURCE_REMOVE;
}

} // namespace kyty::gtk
