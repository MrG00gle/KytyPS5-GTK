// Example tests of the library's background work (src/launcher/gtk/libraryController.h; phase 7
// plan D3): the icons and sizes of a scan reach the games on the main thread. The details worker
// posts its batches as idle sources of the default main context, which run only where that context
// is iterated, also while no thread owns it: this test iterates it with
// g_main_context_iteration(nullptr, FALSE) and sleeps between iterations, so most of the time no
// thread owns it, and records the thread of every notify::texture and notify::size of every game.
// A batch posted with g_main_context_invoke_full would run in the worker then. The compatibility
// database comes from a FakeHttpClient on a FakeScheduler (never the network) and is applied
// without a rescan. GTK's list models and textures need no display: no gtk_init.

#include "launcher/core/compatibility.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"
#include "launcher/gtk/libraryController.h"
#include "support/fakeHttpClient.h"
#include "support/fakeScheduler.h"
#include "support/gtk/testServices.h"
#include "support/noDisplay.h"
#include "support/tempDir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtk/gtk.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;

constexpr int    kGames          = 60;
constexpr int    kIconSize       = 32;
constexpr gint64 kTimeoutUs      = 60 * G_USEC_PER_SEC;
constexpr gulong kSleepUs        = 2000;
constexpr char   kFirstTitleId[] = "PPSA10000";

void Check(bool value, const std::string& message) {
	if (!value) {
		std::fprintf(stderr, "LibraryControllerTests: failed: %s\n", message.c_str());
		std::abort();
	}
}

std::string ParamJson(const std::string& title, const std::string& title_id) {
	return "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"" +
	       title + "\"}},\"titleId\":\"" + title_id +
	       "\",\"appVersion\":\"01.000\",\"requiredSystemSoftwareVersion\":\"0x0150000000000000\"}";
}

// A kIconSize x kIconSize PNG in a color of its own, written through GdkMemoryTexture.
std::string IconPng(int index) {
	std::vector<guint8> pixels(static_cast<std::size_t>(kIconSize) * kIconSize * 4);
	for (std::size_t i = 0; i < pixels.size(); i += 4) {
		pixels[i]     = static_cast<guint8>(index * 4);
		pixels[i + 1] = 128;
		pixels[i + 2] = static_cast<guint8>(255 - index * 4);
		pixels[i + 3] = 255;
	}
	GBytes* bytes   = g_bytes_new(pixels.data(), pixels.size());
	auto    texture = GObjectPtr<GdkTexture>::Adopt(gdk_memory_texture_new(
	    kIconSize, kIconSize, GDK_MEMORY_R8G8B8A8, bytes, static_cast<gsize>(kIconSize) * 4));
	g_bytes_unref(bytes);
	GBytes*     png  = gdk_texture_save_to_png_bytes(texture.get());
	gsize       size = 0;
	const auto* data = static_cast<const char*>(g_bytes_get_data(png, &size));
	std::string result(data, size);
	g_bytes_unref(png);
	return result;
}

// kGames directory games below <root>/games, each with an eboot.bin, a param.json, and an
// icon0.png.
fs::path WriteGames(const KytyTests::TempDir& root) {
	for (int i = 0; i < kGames; i++) {
		char name[32];
		std::snprintf(name, sizeof(name), "Game %02d", i);
		char title_id[16];
		std::snprintf(title_id, sizeof(title_id), "PPSA1%04d", i);
		const fs::path game = fs::path("games") / name;
		root.WriteFile(game / "eboot.bin", "fake eboot\n");
		root.WriteFile(game / "sce_sys" / "param.json", ParamJson(name, title_id));
		root.WriteFile(game / "sce_sys" / "icon0.png", IconPng(i));
	}
	return root.Path() / "games";
}

// Every texture and size notification of the listed games, with the thread it ran on. The
// notifications may come from another thread (that is the defect the test finds), so the counts
// are guarded.
struct Observer {
	GThread*                 main_thread = nullptr;
	std::vector<SignalScope> connections; // main thread only
	std::mutex               mutex;
	int                      textures  = 0;
	int                      sizes     = 0;
	int                      elsewhere = 0; // notifications on another thread

	bool Complete() {
		const std::lock_guard<std::mutex> lock(mutex);
		return textures >= kGames && sizes >= kGames;
	}
};

void OnDetail(GObject* /*game*/, GParamSpec* pspec, gpointer data) {
	auto*                             observer = static_cast<Observer*>(data);
	const std::lock_guard<std::mutex> lock(observer->mutex);
	if (g_thread_self() != observer->main_thread) {
		observer->elsewhere++;
	}
	if (std::string_view(g_param_spec_get_name(pspec)) == "texture") {
		observer->textures++;
	} else {
		observer->sizes++;
	}
}

// The library's store emits items-changed while the scan is applied, before the details worker
// starts: every game is observed from its first notification on.
void OnGamesChanged(GListModel* games, guint position, guint /*removed*/, guint added,
                    gpointer data) {
	auto* observer = static_cast<Observer*>(data);
	for (guint i = position; i < position + added; i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		for (const char* signal: {"notify::texture", "notify::size"}) {
			observer->connections.push_back(
			    SignalScope::Connect(game.get(), signal, G_CALLBACK(OnDetail), observer));
		}
	}
}

// The default main context as a waiting main loop sees it: one non-blocking iteration, then a
// sleep in which no thread owns the context.
void IterateAndSleep() {
	g_main_context_iteration(nullptr, FALSE);
	g_usleep(kSleepUs);
}

void TestDetailsArriveOnTheMainThread() {
	const KytyTests::TempDir root("kyty_library_controller");
	const fs::path           games = WriteGames(root);
	root.MakeDirectories("app");
	root.MakeDirectories("cwd");

	KytyTests::FakeScheduler  clock;
	KytyTests::FakeHttpClient http(clock);
	http.AddResponse(kyty::core::kCompatibilityUrl,
	                 {.status = 200,
	                  .body   = std::string("{\"") + kFirstTitleId +
	                            "\": {\"status\": \"InGame\", \"comment\": \"Runs well\"}}"});
	KytyTests::TestServices services(http, &clock);
	services.environment.Set("HOME", root.Path().string());
	services.environment.Set("XDG_CONFIG_HOME", (root.Path() / "config").string());
	auto context = kyty::gtk::CreateLauncherContext(
	    {.working_directory = root.Path() / "cwd", .launcher_directory = root.Path() / "app"},
	    services.Get());
	context->settings.loaded.settings.game_folders = {games.string()};

	Observer observer;
	observer.main_thread = g_thread_self();
	const auto changed   = SignalScope::Connect(context->library.Games(), "items-changed",
	                                            G_CALLBACK(OnGamesChanged), &observer);

	kyty::gtk::LibraryController controller(*context, {});
	controller.Start();
	const gint64 start    = g_get_monotonic_time();
	const gint64 deadline = start + kTimeoutUs;
	while (!observer.Complete() && g_get_monotonic_time() < deadline) {
		IterateAndSleep();
	}
	// Whatever is still queued (the last batch) runs now.
	for (int i = 0; i < 50; i++) {
		IterateAndSleep();
	}
	{
		const std::lock_guard<std::mutex> lock(observer.mutex);
		std::printf("LibraryControllerTests: %d texture and %d size notification(s) in %.0f ms, "
		            "%d on another thread\n",
		            observer.textures, observer.sizes,
		            static_cast<double>(g_get_monotonic_time() - start) / 1000.0,
		            observer.elsewhere);
		Check(observer.elsewhere == 0,
		      std::to_string(observer.elsewhere) +
		          " icon or size notification(s) ran on another thread than the main thread");
		Check(observer.textures == kGames, "one texture notification per game");
		Check(observer.sizes == kGames, "one size notification per game");
	}
	Check(controller.Stats().scans_applied == 1, "one scan applied");
	Check(controller.Stats().details_done == 1, "the details of the scan were all delivered");

	GListModel* listed = context->library.Games();
	Check(g_list_model_get_n_items(listed) == kGames, "every fixture game is listed");
	for (guint i = 0; i < g_list_model_get_n_items(listed); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(listed, i)));
		const std::string& path    = kyty_game_get_entry(game.get()).path.native();
		GdkTexture*        texture = kyty_game_get_texture(game.get());
		Check(texture != nullptr, path + " has its icon");
		Check(gdk_texture_get_width(texture) == kIconSize &&
		          gdk_texture_get_height(texture) == kIconSize,
		      path + " has its icon's size");
		Check(kyty_game_get_size(game.get()).has_value() && *kyty_game_get_size(game.get()) > 0,
		      path + " has its size");
	}

	// The compatibility database: one request through the fake, applied without a rescan.
	Check(http.Requests().size() == 1 && http.Requests()[0].url == kyty::core::kCompatibilityUrl,
	      "the compatibility database is requested once, through the HTTP fake");
	clock.RunUntilIdle();
	Check(controller.Stats().compatibility_source == "download",
	      "the downloaded compatibility database is applied");
	Check(controller.Stats().scans_applied == 1, "the statuses are applied without a rescan");
	int in_game = 0;
	for (guint i = 0; i < g_list_model_get_n_items(listed); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(listed, i)));
		const bool first  = kyty_game_get_entry(game.get()).title_id == kFirstTitleId;
		const auto status = kyty_game_get_status(game.get());
		Check(first == (status == kyty::core::CompatibilityStatus::InGame),
		      "only " + std::string(kFirstTitleId) + " is In game");
		in_game += first ? 1 : 0;
		if (first) {
			Check(kyty_game_get_comment(game.get()) == "Runs well", "its comment is applied");
		}
	}
	Check(in_game == 1, std::string(kFirstTitleId) + " is listed once");
	Check(services.controller.lightbar_calls.size() == 1 &&
	          !services.controller.lightbar_calls[0].color.has_value(),
	      "Start sets the lightbar once, to no color (none configured)");
}

} // namespace

int main() {
	KytyTests::CheckNoDisplay("LibraryControllerTests");
	// A GLib critical or warning fails the test.
	g_log_set_always_fatal(GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));

	TestDetailsArriveOnTheMainThread();
	std::printf("LibraryControllerTests: all tests passed\n");
	return 0;
}
