#pragma once

// A Scheduler on a virtual clock (milliseconds since construction). Timers run only inside
// AdvanceBy() and RunUntilIdle(), never inside After(), so callbacks are never re-entrant. Calling
// AdvanceBy() or RunUntilIdle() from a timer callback aborts.

#include "launcher/core/scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace KytyTests {

class FakeScheduler final: public kyty::core::Scheduler {
public:
	FakeScheduler()           = default;
	~FakeScheduler() override = default;

	FakeScheduler(const FakeScheduler&)            = delete;
	FakeScheduler& operator=(const FakeScheduler&) = delete;

	// Schedules fn at Now() + delay (a negative delay counts as zero) and records delay.
	kyty::core::TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) override;
	void                Cancel(kyty::core::TimerId id) override;

	// After() without recording the delay: the fakes (FakeHttpClient) schedule their own events
	// with it, so RequestedDelays() lists only the delays that the code under test asked for.
	kyty::core::TimerId AfterUnrecorded(std::chrono::milliseconds delay, std::function<void()> fn);

	[[nodiscard]] std::chrono::milliseconds Now() const { return m_now; }

	// Moves the clock forward by duration, running every timer that falls due on the way in time
	// order (FIFO among equal times), including timers that those callbacks schedule inside the
	// window. Now() is the due time of each timer while it runs, and Now() + duration afterwards.
	void AdvanceBy(std::chrono::milliseconds duration);

	// Runs timers in time order, moving the clock to each, until none is pending. Aborts after a
	// million timers (a callback that keeps rescheduling itself).
	void RunUntilIdle();

	// Every delay passed to After(), in call order.
	[[nodiscard]] const std::vector<std::chrono::milliseconds>& RequestedDelays() const {
		return m_requested_delays;
	}

	[[nodiscard]] size_t PendingCount() const { return m_timers.size(); }

private:
	struct Timer {
		std::chrono::milliseconds due;
		kyty::core::TimerId       id; // increasing, so it also orders equal due times
		std::function<void()>     fn;
	};

	// Runs the earliest timer due at or before limit; false when there is none.
	bool RunNext(std::chrono::milliseconds limit);

	std::chrono::milliseconds              m_now {0};
	kyty::core::TimerId                    m_next_id = 1;
	std::vector<Timer>                     m_timers;
	std::vector<std::chrono::milliseconds> m_requested_delays;
	bool                                   m_running = false;
};

} // namespace KytyTests
