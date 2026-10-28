#include "support/fakeScheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace KytyTests {

namespace {

[[noreturn]] void Fail(const char* message) {
	std::fprintf(stderr, "FakeScheduler: %s\n", message);
	std::abort();
}

} // namespace

kyty::core::TimerId FakeScheduler::After(std::chrono::milliseconds delay,
                                         std::function<void()>     fn) {
	m_requested_delays.push_back(delay);
	return AfterUnrecorded(delay, std::move(fn));
}

kyty::core::TimerId FakeScheduler::AfterUnrecorded(std::chrono::milliseconds delay,
                                                   std::function<void()>     fn) {
	const auto id = m_next_id++;
	m_timers.push_back(
	    Timer {m_now + std::max(delay, std::chrono::milliseconds {0}), id, std::move(fn)});
	return id;
}

void FakeScheduler::Cancel(kyty::core::TimerId id) {
	std::erase_if(m_timers, [id](const Timer& timer) { return timer.id == id; });
}

bool FakeScheduler::RunNext(std::chrono::milliseconds limit) {
	const auto next =
	    std::min_element(m_timers.begin(), m_timers.end(), [](const Timer& a, const Timer& b) {
		    return a.due != b.due ? a.due < b.due : a.id < b.id;
	    });
	if (next == m_timers.end() || next->due > limit) {
		return false;
	}
	auto fn = std::move(next->fn);
	m_now   = std::max(m_now, next->due);
	m_timers.erase(next);
	if (fn) {
		fn();
	}
	return true;
}

void FakeScheduler::AdvanceBy(std::chrono::milliseconds duration) {
	if (m_running) {
		Fail("AdvanceBy() called from a timer callback");
	}
	m_running         = true;
	const auto target = m_now + std::max(duration, std::chrono::milliseconds {0});
	while (RunNext(target)) {
	}
	m_now     = target;
	m_running = false;
}

void FakeScheduler::RunUntilIdle() {
	if (m_running) {
		Fail("RunUntilIdle() called from a timer callback");
	}
	m_running = true;
	for (int count = 0; RunNext(std::chrono::milliseconds::max()); count++) {
		if (count >= 1'000'000) {
			Fail("RunUntilIdle() ran a million timers; a callback keeps rescheduling itself");
		}
	}
	m_running = false;
}

} // namespace KytyTests
