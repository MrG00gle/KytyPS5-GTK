#include "launcher/gtk/backgroundTask.h"

#include <gio/gio.h>
#include <utility>

namespace kyty::gtk {

namespace {

struct BackgroundTask {
	std::function<void()> work;
	std::function<void()> done;
};

void RunWork(GTask* task, gpointer /*source*/, gpointer task_data, GCancellable* /*cancellable*/) {
	if (auto* background = static_cast<BackgroundTask*>(task_data); background->work) {
		background->work();
	}
	g_task_return_boolean(task, TRUE);
}

void OnDone(GObject* /*source*/, GAsyncResult* result, gpointer /*data*/) {
	auto* background = static_cast<BackgroundTask*>(g_task_get_task_data(G_TASK(result)));
	if (background->done) {
		background->done();
	}
}

} // namespace

void RunInBackground(std::function<void()> work, std::function<void()> done) {
	auto*  background = new BackgroundTask {std::move(work), std::move(done)};
	GTask* task       = g_task_new(nullptr, nullptr, &OnDone, nullptr);
	g_task_set_task_data(task, background,
	                     [](gpointer data) { delete static_cast<BackgroundTask*>(data); });
	g_task_run_in_thread(task, &RunWork);
	g_object_unref(task);
}

} // namespace kyty::gtk
