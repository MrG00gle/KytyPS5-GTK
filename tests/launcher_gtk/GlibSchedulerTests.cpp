// Example tests of the GLib scheduler (src/launcher/gtk/services/glibScheduler.h, task 19.x; plan
// D20, HC-12) against the core::Scheduler contract (src/launcher/core/scheduler.h): timers with
// equal delays run in the order of their After() calls; zero and negative delays run later, never
// inside After(); Cancel() before firing prevents the call, after firing and for unknown ids or 0
// it does nothing; ids are never 0; callbacks may schedule, cancel, and destroy the scheduler; the
// destructor cancels every pending timer. Real GLib timers on the default main context; no display.

#include "launcher/gtk/services/glibScheduler.h"
#include "support/noDisplay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <glib.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using kyty::core::TimerId;
using kyty::gtk::GlibScheduler;
using namespace std::chrono_literals;

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "GlibSchedulerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

// Iterates the default main context until done() or limit.
bool RunUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) {
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

// Iterates the default main context for duration.
void RunFor(std::chrono::milliseconds duration) {
	RunUntil([] { return false; }, duration);
}

void TestOrderOfEqualDelays() {
	GlibScheduler    scheduler;
	std::vector<int> order;
	for (int i = 0; i < 8; i++) {
		scheduler.After(20ms, [&order, i] { order.push_back(i); });
	}
	Check(scheduler.PendingCount() == 8, "eight timers are pending");
	Check(RunUntil([&] { return order.size() == 8; }), "every timer fires");
	Check(order == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}),
	      "timers with equal delays run in the order of their After() calls");
	Check(scheduler.PendingCount() == 0, "fired timers are no longer pending");

	std::vector<int> mixed;
	scheduler.After(60ms, [&] { mixed.push_back(60); });
	scheduler.After(10ms, [&] { mixed.push_back(10); });
	scheduler.After(30ms, [&] { mixed.push_back(30); });
	Check(RunUntil([&] { return mixed.size() == 3; }), "the mixed timers fire");
	Check(mixed == std::vector<int>({10, 30, 60}), "shorter delays run first");
}

void TestZeroAndNegativeDelays() {
	GlibScheduler scheduler;
	int           zero     = 0;
	int           negative = 0;
	const auto    start    = std::chrono::steady_clock::now();
	scheduler.After(0ms, [&] { zero++; });
	scheduler.After(-250ms, [&] { negative++; });
	Check(zero == 0 && negative == 0, "zero and negative delays never run inside After()");
	Check(RunUntil([&] { return zero == 1 && negative == 1; }, 1s),
	      "zero and negative delays run later");
	Check(std::chrono::steady_clock::now() - start < 500ms,
	      "a negative delay counts as 0, not as a long wait");
	RunFor(50ms);
	Check(zero == 1 && negative == 1, "each timer runs exactly once");

	// A zero timer scheduled from a callback runs in a later iteration, not inside the callback.
	bool inner = false;
	bool outer = false;
	scheduler.After(0ms, [&] {
		outer = true;
		scheduler.After(0ms, [&] { inner = true; });
		Check(!inner, "a timer scheduled from a callback does not run inside that callback");
	});
	Check(RunUntil([&] { return inner; }, 1s), "the nested zero timer runs");
	Check(outer, "the outer timer ran first");
}

void TestCancel() {
	GlibScheduler scheduler;
	int           cancelled = 0;
	int           kept      = 0;
	const TimerId first     = scheduler.After(20ms, [&] { cancelled++; });
	const TimerId second    = scheduler.After(20ms, [&] { kept++; });
	Check(first != 0 && second != 0 && first != second, "ids are distinct and never 0");
	scheduler.Cancel(first);
	Check(scheduler.PendingCount() == 1, "Cancel removes the pending timer");
	Check(RunUntil([&] { return kept == 1; }), "the other timer still fires");
	RunFor(60ms);
	Check(cancelled == 0, "a cancelled timer never runs");

	// After firing, Cancel does nothing (and does not affect a newer timer).
	int           later = 0;
	const TimerId third = scheduler.After(30ms, [&] { later++; });
	scheduler.Cancel(second);
	scheduler.Cancel(first);
	scheduler.Cancel(0);
	scheduler.Cancel(third + 1000);
	Check(scheduler.PendingCount() == 1, "Cancel of fired, unknown, or 0 ids changes nothing");
	Check(RunUntil([&] { return later == 1; }), "the pending timer is unaffected");

	// A callback cancels another pending timer.
	int     victim_runs = 0;
	TimerId victim      = 0;
	bool    killer_ran  = false;
	scheduler.After(10ms, [&] {
		killer_ran = true;
		scheduler.Cancel(victim);
	});
	victim = scheduler.After(40ms, [&] { victim_runs++; });
	Check(RunUntil([&] { return killer_ran; }), "the cancelling timer runs");
	RunFor(80ms);
	Check(victim_runs == 0, "a timer cancelled from another callback never runs");
	Check(scheduler.PendingCount() == 0, "nothing is left pending");

	// A callback cancels its own id (already fired): nothing happens.
	TimerId self_id = 0;
	int     self    = 0;
	self_id         = scheduler.After(0ms, [&] {
		self++;
		scheduler.Cancel(self_id);
	});
	Check(RunUntil([&] { return self == 1; }, 1s), "the self-cancelling timer runs once");
}

void TestIdsNeverZero() {
	GlibScheduler     scheduler;
	std::set<TimerId> ids;
	for (int i = 0; i < 1000; i++) {
		const TimerId id = scheduler.After(10s, [] {});
		Check(id != 0, "After() never returns 0");
		ids.insert(id);
	}
	Check(ids.size() == 1000, "ids are never reused while pending");
	for (const TimerId id: ids) {
		scheduler.Cancel(id);
	}
	Check(scheduler.PendingCount() == 0, "every timer is cancelled");
}

void TestDestructorCancels() {
	int runs = 0;
	{
		GlibScheduler scheduler;
		scheduler.After(0ms, [&] { runs++; });
		scheduler.After(20ms, [&] { runs++; });
	}
	RunFor(80ms);
	Check(runs == 0, "the destructor cancels every pending timer");

	// A callback destroys the scheduler: its other timers never run.
	auto scheduler = std::make_unique<GlibScheduler>();
	int  others    = 0;
	bool destroyed = false;
	scheduler->After(10ms, [&] {
		scheduler.reset();
		destroyed = true;
	});
	scheduler->After(30ms, [&] { others++; });
	Check(RunUntil([&] { return destroyed; }), "the destroying timer runs");
	RunFor(80ms);
	Check(others == 0, "the timers of a destroyed scheduler never run");
}

void TestDelayIsHonored() {
	GlibScheduler scheduler;
	bool          fired = false;
	const auto    start = std::chrono::steady_clock::now();
	auto          end   = start;
	scheduler.After(150ms, [&] {
		fired = true;
		end   = std::chrono::steady_clock::now();
	});
	Check(RunUntil([&] { return fired; }), "the timer fires");
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	std::printf("GlibSchedulerTests: a 150 ms timer fired after %lld ms\n",
	            static_cast<long long>(elapsed.count()));
	Check(elapsed >= 150ms, "the timer does not fire early");
	Check(elapsed < 2s, "the timer fires close to its delay");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("GlibSchedulerTests");
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestOrderOfEqualDelays();
	TestZeroAndNegativeDelays();
	TestCancel();
	TestIdsNeverZero();
	TestDestructorCancels();
	TestDelayIsHonored();
	std::printf("GlibSchedulerTests: all tests passed\n");
	return 0;
}
