#pragma once

// Timers for Launcher_Core and the Updater (design.md, "Launcher_Core interfaces"). The GTK layer
// implements Scheduler on the GLib main loop; tests use KytyTests::FakeScheduler, a virtual clock.
//
// Contract (every implementation keeps it):
// - After(delay, fn) runs fn exactly once, later, on the main thread, and never inside After()
//   itself, even when delay is zero or negative.
// - Cancel(id) before fn has run means fn never runs. Cancel() of a timer that has already fired,
// of
//   an unknown id, or of 0 does nothing.
// - TimerId values are never 0; 0 means "no timer".

#include <chrono>
#include <cstdint>
#include <functional>

namespace kyty::core {

using TimerId = uint64_t;

class Scheduler {
public:
	virtual ~Scheduler() = default;

	virtual TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
	virtual void    Cancel(TimerId id)                                               = 0;
};

} // namespace kyty::core
