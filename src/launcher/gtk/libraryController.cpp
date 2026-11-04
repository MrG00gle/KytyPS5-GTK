#include "launcher/gtk/libraryController.h"

#include "launcher/core/gameContent.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/scanFileSystem.h"
#include "launcher/core/settingsStore.h"
#include "launcher/core/titleId.h"
#include "launcher/gtk/gameViewSupport.h"
#include "launcher/gtk/i18n.h"
#include "launcher/gtk/imageDecoding.h"
#include "launcher/gtk/services/controllerService.h"
#include "launcher/gtk/services/titleSoundPlayer.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <system_error>
#include <utility>

namespace kyty::gtk {

namespace {

namespace fs = std::filesystem;

// A batch of details goes to the main thread after this many results or this time.
constexpr std::size_t kBatchItems        = 64;
constexpr gint64      kBatchMicroseconds = 100000;

double MillisecondsSince(gint64 start_us) {
	return static_cast<double>(g_get_monotonic_time() - start_us) / 1000.0;
}

// The size of an entry as the Qt launcher computed it (LFI-39): the .zar file's size, or the sum of
// the regular files below the directory (symbolic links are neither counted nor followed, and
// unreadable directories are skipped); nullopt when the entry is gone or the work is cancelled.
std::optional<uint64_t> EntrySize(const fs::path& path, core::GameEntry::Kind kind,
                                  const core::CancelToken& cancel) {
	std::error_code ec;
	if (kind == core::GameEntry::Kind::Archive) {
		const auto size = fs::file_size(path, ec);
		return ec ? std::nullopt : std::optional<uint64_t>(size);
	}
	if (!fs::is_directory(path, ec)) {
		return std::nullopt;
	}
	uint64_t total = 0;
	for (fs::recursive_directory_iterator
	         it(path, fs::directory_options::skip_permission_denied, ec),
	     end;
	     !ec && it != end; it.increment(ec)) {
		if (cancel.IsCancelled()) {
			return std::nullopt;
		}
		std::error_code entry_ec;
		if (fs::is_regular_file(it->symlink_status(entry_ec)) && !entry_ec) {
			const auto size = it->file_size(entry_ec);
			if (!entry_ec) {
				total += size;
			}
		}
	}
	return total;
}

// The largest scale factor of the display's monitors (1 without a display).
int LargestScaleFactor() {
	int         scale   = 1;
	GdkDisplay* display = gdk_display_get_default();
	if (display == nullptr) {
		return scale;
	}
	GListModel* monitors = gdk_display_get_monitors(display);
	for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
		auto monitor = GObjectPtr<GdkMonitor>::Adopt(
		    static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i)));
		scale = std::max(scale, gdk_monitor_get_scale_factor(monitor.get()));
	}
	return scale;
}

// The configured lightbar color ("#rrggbb"), or none: the game controls the LED.
std::optional<LightbarColor> ConfiguredLightbar(const std::string& color) {
	GdkRGBA rgba {};
	if (color.empty() || gdk_rgba_parse(&rgba, color.c_str()) == FALSE) {
		return std::nullopt;
	}
	const auto channel = [](float value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
	};
	return LightbarColor {channel(rgba.red), channel(rgba.green), channel(rgba.blue)};
}

const char* SourceName(core::CompatibilityLoadResult::Source source) {
	switch (source) {
		case core::CompatibilityLoadResult::Source::Download: return "download";
		case core::CompatibilityLoadResult::Source::Cache: return "cache";
		case core::CompatibilityLoadResult::Source::None: break;
	}
	return "none";
}

// Requirements 8.4 and 8.6 ask for a warning when no database is available at all and when
// downloaded or cached content does not parse; the loader's other messages (a failed attempt that
// is retried, a cache write failure, the cache fallback) are logged as messages.
void LogCompatibilityWarning(const std::string& text) {
	if (text.find("does not parse") != std::string::npos ||
	    text.rfind("No compatibility database is available", 0) == 0) {
		g_warning("%s", text.c_str());
	} else {
		g_message("%s", text.c_str());
	}
}

} // namespace

struct LibraryController::ScanTask {
	std::shared_ptr<bool>              alive;
	LibraryController*                 controller = nullptr;
	uint64_t                           generation = 0;
	std::vector<std::string>           folders; // as stored
	std::shared_ptr<core::CancelToken> cancel;
	core::ScanResult                   result;
	gint64                             started_us = 0;
	gint64                             scanned_us = 0;
};

struct LibraryController::DetailsTask {
	std::shared_ptr<bool>                                   alive;
	LibraryController*                                      controller = nullptr;
	uint64_t                                                generation = 0;
	std::vector<std::pair<fs::path, core::GameEntry::Kind>> entries;
	std::shared_ptr<core::CancelToken>                      cancel;
	gint64                                                  started_us = 0;
	int                                                     icon_size  = kLargestGameIconSize;
};

// Details for the main thread; the last batch of a run marks it done.
struct LibraryController::Batch {
	std::shared_ptr<bool>                                        alive;
	LibraryController*                                           controller = nullptr;
	uint64_t                                                     generation = 0;
	std::vector<std::pair<std::string, GObjectPtr<GdkTexture>>>  icons;
	std::vector<std::pair<std::string, std::optional<uint64_t>>> sizes;
	bool                                                         last       = false;
	gint64                                                       started_us = 0;
	std::size_t                                                  count      = 0;
};

struct LibraryController::ArtTask {
	std::shared_ptr<bool>              alive;
	LibraryController*                 controller = nullptr;
	uint64_t                           generation = 0;
	fs::path                           path;
	core::GameEntry::Kind              kind = core::GameEntry::Kind::Directory;
	std::shared_ptr<core::CancelToken> cancel;
	GObjectPtr<GdkTexture>             art;
};

LibraryController::LibraryController(LauncherContext& context, Callbacks callbacks)
    : m_context(context), m_callbacks(std::move(callbacks)) {}

LibraryController::~LibraryController() {
	*m_alive = false;
	for (const auto& cancel: {m_scan_cancel, m_details_cancel, m_art_cancel}) {
		if (cancel) {
			cancel->Cancel();
		}
	}
	m_selection_signal.Disconnect();
	m_loader.reset();
	if (m_started) {
		m_context.services.title_sound.Select(nullptr);
	}
}

void LibraryController::Start() {
	if (m_started) {
		return;
	}
	m_started = true;
	m_selection_signal =
	    SignalScope::Connect(m_context.library.Selection(), "notify::selected-item",
	                         G_CALLBACK(&OnSelectionChanged), this);
	ApplyLightbar();
	StartCompatibility();
	Rescan();
}

void LibraryController::Rescan() {
	for (auto* cancel: {&m_scan_cancel, &m_details_cancel}) {
		if (*cancel) {
			(*cancel)->Cancel();
			cancel->reset();
		}
	}
	m_scan_cancel = std::make_shared<core::CancelToken>();
	m_generation++;
	m_stats.scans_started++;
	m_requested_folders = m_context.settings.loaded.settings.game_folders;
	m_scanning          = true;

	auto* scan       = new ScanTask;
	scan->alive      = m_alive;
	scan->controller = this;
	scan->generation = m_generation;
	scan->folders    = m_requested_folders;
	scan->cancel     = m_scan_cancel;
	scan->started_us = g_get_monotonic_time();
	g_debug("scan %" G_GUINT64_FORMAT " started: %zu game folder(s)", m_generation,
	        scan->folders.size());
	// An unowned GTask, a recorded exception (p6-u9-6): the alive flag drops a late result.
	GTask* task = g_task_new(nullptr, nullptr, &OnScanned, nullptr);
	g_task_set_task_data(task, scan, [](gpointer data) { delete static_cast<ScanTask*>(data); });
	g_task_run_in_thread(task, &RunScan);
	g_object_unref(task);
}

void LibraryController::RunScan(GTask* task, gpointer /*source*/, gpointer task_data,
                                GCancellable* /*cancellable*/) {
	auto*      scan        = static_cast<ScanTask*>(task_data);
	auto       file_system = core::MakeHostScanFileSystem();
	const auto folders     = core::NormalizeGameFolders(scan->folders, *file_system);
	const auto reader      = core::MakeHostMetadataReader();
	scan->result           = core::ScanGameFolders(folders, *file_system, *reader, *scan->cancel);
	scan->scanned_us       = g_get_monotonic_time();
	g_task_return_boolean(task, TRUE);
}

void LibraryController::OnScanned(GObject* /*source*/, GAsyncResult* result, gpointer /*data*/) {
	auto* scan = static_cast<ScanTask*>(g_task_get_task_data(G_TASK(result)));
	if (!*scan->alive) {
		return;
	}
	auto* self = scan->controller;
	if (scan->generation != self->m_generation || scan->result.cancelled) {
		g_debug("scan %" G_GUINT64_FORMAT " dropped: a later scan replaces it", scan->generation);
		return;
	}
	self->ApplyScan(*scan);
}

void LibraryController::ApplyScan(ScanTask& scan) {
	const gint64 apply_start = g_get_monotonic_time();
	m_scanning               = false;
	m_scanned_folders        = scan.folders;
	m_warnings               = std::move(scan.result.warnings);
	for (const auto& warning: m_warnings) {
		g_message("scan: cannot read %s: %s", warning.path.c_str(), warning.reason.c_str());
	}
	std::vector<core::GameEntry> entries = std::move(scan.result.entries);

	// HC-22: the Qt launcher's relative game paths match the scanned entries from now on. After
	// every applied scan, as Qt's scan matched them (FindCustomInfo): a folder that was missing
	// at startup or was added later has its legacy elements migrated too. Idempotent.
	if (auto next = m_context.settings.loaded.settings;
	    core::MigrateLegacyGamePaths(next, entries)) {
		g_debug("scan %" G_GUINT64_FORMAT ": migrated legacy game paths", scan.generation);
		if (const auto error = SaveSettingsAutomatically(m_context, std::move(next))) {
			Notify(_("The settings could not be saved"), error->message);
		}
	}

	// The icons and sizes of games that stay listed are kept until their details arrive.
	struct Kept {
		GObjectPtr<GdkTexture>  texture;
		std::optional<uint64_t> size;
	};
	std::unordered_map<std::string, Kept> kept;
	for (const auto& [path, game]: m_games) {
		kept[path] = {GObjectPtr<GdkTexture>::Ref(kyty_game_get_texture(game)),
		              kyty_game_get_size(game)};
	}
	auto&             library = m_context.library;
	const std::size_t count   = entries.size();
	m_replacing               = true;
	library.Replace(std::move(entries));
	m_replacing = false;
	m_games.clear();
	GListModel* games = library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto holder =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		auto*              game = holder.get(); // the store keeps its reference
		const std::string& path = kyty_game_get_entry(game).path.native();
		m_games[path]           = game;
		if (const auto found = kept.find(path); found != kept.end()) {
			kyty_game_set_texture(game, found->second.texture.get());
			kyty_game_set_size(game, found->second.size);
		}
	}
	ApplyStatuses();
	library.ColumnChanged(core::Column::Size);
	m_stats.scans_applied++;
	g_debug("scan %" G_GUINT64_FORMAT ": %zu game(s), %zu warning(s); scanned in %.0f ms, "
	        "applied in %.1f ms",
	        scan.generation, count, m_warnings.size(),
	        static_cast<double>(scan.scanned_us - scan.started_us) / 1000.0,
	        MillisecondsSince(apply_start));
	if (m_callbacks.scanned) {
		m_callbacks.scanned(m_warnings);
	}
	HandleSelection();
	StartDetails();
}

void LibraryController::StartDetails() {
	if (m_details_cancel) {
		m_details_cancel->Cancel();
	}
	m_details_cancel    = std::make_shared<core::CancelToken>();
	auto* details       = new DetailsTask;
	details->alive      = m_alive;
	details->controller = this;
	details->generation = m_generation;
	details->cancel     = m_details_cancel;
	details->started_us = g_get_monotonic_time();
	details->icon_size  = kLargestGameIconSize * LargestScaleFactor();
	GListModel* games   = m_context.library.Games();
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		const auto& entry = kyty_game_get_entry(game.get());
		details->entries.emplace_back(entry.path, entry.kind);
	}
	// An unowned GTask, a recorded exception (p6-u9-6): its batches carry the alive flag.
	GTask* task = g_task_new(nullptr, nullptr, nullptr, nullptr);
	g_task_set_task_data(task, details,
	                     [](gpointer data) { delete static_cast<DetailsTask*>(data); });
	g_task_run_in_thread(task, &RunDetails);
	g_object_unref(task);
}

void LibraryController::RunDetails(GTask* task, gpointer /*source*/, gpointer task_data,
                                   GCancellable* /*cancellable*/) {
	auto*      details   = static_cast<DetailsTask*>(task_data);
	const auto new_batch = [details]() {
		auto* batch       = new Batch;
		batch->alive      = details->alive;
		batch->controller = details->controller;
		batch->generation = details->generation;
		batch->started_us = details->started_us;
		return batch;
	};
	Batch*      batch     = new_batch();
	gint64      last_post = g_get_monotonic_time();
	std::size_t delivered = 0;
	const auto  post      = [&](bool last) {
		const std::size_t items = batch->icons.size() + batch->sizes.size();
		if (!last && items < kBatchItems &&
		    g_get_monotonic_time() - last_post < kBatchMicroseconds) {
			return;
		}
		if (!last && items == 0) {
			return;
		}
		delivered += items;
		batch->last  = last;
		batch->count = delivered;
		// An idle source of the default main context: ApplyBatch runs on the thread that iterates
		// the context, the main thread, also when no thread owns the context at this moment (an
		// invoke of the context would then run it here, in the worker). An unowned idle, a
		// recorded exception (p6-u9-6): the alive flag and the generation drop a late batch.
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &ApplyBatch, batch,
		                [](gpointer data) { delete static_cast<Batch*>(data); });
		batch     = last ? nullptr : new_batch();
		last_post = g_get_monotonic_time();
	};
	const core::CancelToken& cancel = *details->cancel;
	for (const auto& [path, kind]: details->entries) {
		if (cancel.IsCancelled()) {
			break;
		}
		const auto decoded = GObjectPtr<GdkTexture>::Adopt(
		    DecodePng(core::ReadIcon0(path, kind).value_or(""), path, "icon0.png"));
		// Once, at the largest size the views show (the 512 x 512 icons would be scaled at every
		// frame otherwise).
		batch->icons.emplace_back(path.native(), GObjectPtr<GdkTexture>::Adopt(FitTexture(
		                                             decoded.get(), details->icon_size)));
		post(false);
	}
	for (const auto& [path, kind]: details->entries) {
		if (cancel.IsCancelled()) {
			break;
		}
		batch->sizes.emplace_back(path.native(), EntrySize(path, kind, cancel));
		post(false);
	}
	post(true);
	g_task_return_boolean(task, TRUE);
}

gboolean LibraryController::ApplyBatch(gpointer data) {
	auto* batch = static_cast<Batch*>(data);
	if (!*batch->alive || batch->generation != batch->controller->m_generation) {
		return G_SOURCE_REMOVE;
	}
	auto* self  = batch->controller;
	bool  sizes = false;
	for (const auto& [path, texture]: batch->icons) {
		if (const auto found = self->m_games.find(path); found != self->m_games.end()) {
			kyty_game_set_texture(found->second, texture.get());
		}
	}
	for (const auto& [path, size]: batch->sizes) {
		if (const auto found = self->m_games.find(path); found != self->m_games.end()) {
			kyty_game_set_size(found->second, size);
			sizes = true;
		}
	}
	if (sizes) {
		self->m_context.library.ColumnChanged(core::Column::Size);
	}
	if (batch->last) {
		self->m_stats.details_done++;
		g_debug("details of scan %" G_GUINT64_FORMAT ": %zu result(s) in %.0f ms",
		        batch->generation, batch->count, MillisecondsSince(batch->started_us));
	}
	return G_SOURCE_REMOVE;
}

void LibraryController::StartCompatibility() {
	if (!m_context.local) {
		core::CompatibilityLoader::Callbacks callbacks;
		callbacks.on_warning  = [](const std::string& text) { LogCompatibilityWarning(text); };
		callbacks.on_finished = [this](core::CompatibilityLoadResult result) {
			ApplyCompatibility(result);
		};
		m_loader = std::make_unique<core::CompatibilityLoader>(
		    m_context.services.http, m_context.services.scheduler, m_context.paths.data_dir,
		    std::move(callbacks));
		g_debug("compatibility database: downloading, cache %s", m_loader->CachePath().c_str());
		m_loader->Start();
		return;
	}
	// --local (LFI-21): ./compatibility_db.json, nothing is downloaded.
	std::error_code ec;
	const fs::path  directory = fs::current_path(ec);
	m_local_path = (ec ? fs::path(".") : directory) / core::kLocalCompatibilityFileName;
	auto loaded  = core::LoadLocalCompatibility(m_local_path);
	m_stats.compatibility_finished    = true;
	m_stats.compatibility_finished_us = g_get_monotonic_time();
	if (!loaded) {
		m_stats.local_load_failed = true;
		g_message("%s", loaded.error().c_str());
		Notify(_("The compatibility list could not be loaded; edits are not saved"),
		       loaded.error());
		return;
	}
	g_debug("compatibility database from %s: %zu entries", m_local_path.c_str(),
	        loaded->entries.size());
	m_context.compatibility.Apply(std::move(*loaded));
	m_stats.compatibility_source     = "local";
	m_stats.compatibility_applied_us = g_get_monotonic_time();
}

void LibraryController::ApplyCompatibility(const core::CompatibilityLoadResult& result) {
	const gint64 start                = g_get_monotonic_time();
	m_stats.compatibility_finished    = true;
	m_stats.compatibility_finished_us = start;
	if (!m_context.compatibility.ApplyLoadResult(result)) {
		g_debug("compatibility database: none after %d attempt(s); the statuses stay",
		        result.attempts);
		return;
	}
	ApplyStatuses();
	m_stats.compatibility_source     = SourceName(result.source);
	m_stats.compatibility_applied_us = g_get_monotonic_time();
	g_debug("compatibility database from the %s: %zu entries, applied to %zu game(s) in %.1f ms "
	        "without a rescan",
	        SourceName(result.source), m_context.compatibility.Database().entries.size(),
	        m_games.size(), MillisecondsSince(start));
}

void LibraryController::ApplyStatuses() {
	for (const auto& [path, game]: m_games) {
		auto entry = m_context.compatibility.StatusFor(kyty_game_get_entry(game).title_id);
		kyty_game_set_status(game, entry.status);
		kyty_game_set_comment(game, std::move(entry.comment));
	}
	m_context.library.ColumnChanged(core::Column::Status);
	m_context.library.ColumnChanged(core::Column::Comment);
}

void LibraryController::StatusEdited(KytyGame* game, core::CompatibilityStatus status) {
	const std::string& title_id = kyty_game_get_entry(game).title_id;
	if (!m_context.local || !m_context.compatibility.SetStatus(title_id, status)) {
		return;
	}
	const std::string key = core::NormalizeTitleId(title_id);
	for (const auto& [path, other]: m_games) {
		if (core::NormalizeTitleId(kyty_game_get_entry(other).title_id) == key) {
			kyty_game_set_status(other, status);
		}
	}
	m_context.library.ColumnChanged(core::Column::Status);
	SaveLocal();
}

void LibraryController::CommentEdited(KytyGame* game, const std::string& comment) {
	const std::string& title_id = kyty_game_get_entry(game).title_id;
	if (!m_context.local || !m_context.compatibility.SetComment(title_id, comment)) {
		return;
	}
	const std::string key = core::NormalizeTitleId(title_id);
	for (const auto& [path, other]: m_games) {
		if (core::NormalizeTitleId(kyty_game_get_entry(other).title_id) == key) {
			kyty_game_set_comment(other, comment);
		}
	}
	m_context.library.ColumnChanged(core::Column::Comment);
	SaveLocal();
}

void LibraryController::SaveLocal() {
	if (m_stats.local_load_failed) {
		g_message("not saving %s: it could not be loaded", m_local_path.c_str());
		return;
	}
	if (auto saved = core::SaveLocalCompatibility(m_local_path, m_context.compatibility.Database());
	    !saved) {
		g_message("%s", saved.error().message.c_str());
		Notify(_("The compatibility list could not be saved"), saved.error().message);
	}
}

void LibraryController::SettingsSaved() {
	ApplyLightbar();
	if (m_context.settings.loaded.settings.game_folders != m_requested_folders) {
		g_debug("the game folders changed: rescanning");
		Rescan();
	}
}

void LibraryController::OnSelectionChanged(GObject* /*selection*/, GParamSpec* /*pspec*/,
                                           gpointer data) {
	auto* self = static_cast<LibraryController*>(data);
	if (!self->m_replacing) {
		self->HandleSelection();
	}
}

void LibraryController::HandleSelection() {
	KytyGame*      game = m_context.library.Selected();
	const fs::path path = game != nullptr ? kyty_game_get_entry(game).path : fs::path();
	if (path != m_selected_path) {
		m_selected_path = path;
		// HC-14 (8.7, 8.8): the player stops the previous sound at once and plays this one.
		m_context.services.title_sound.Select(game != nullptr ? &kyty_game_get_entry(game)
		                                                      : nullptr);
		LoadArt(game);
		ApplyLightbar();
	}
	if (m_callbacks.selection_changed) {
		m_callbacks.selection_changed(game);
	}
}

void LibraryController::LoadArt(KytyGame* game) {
	m_art_generation++;
	if (m_art_cancel) {
		m_art_cancel->Cancel();
		m_art_cancel.reset();
	}
	if (game == nullptr) {
		if (m_callbacks.background_art) {
			m_callbacks.background_art(nullptr);
		}
		return;
	}
	m_art_cancel    = std::make_shared<core::CancelToken>();
	auto* art       = new ArtTask;
	art->alive      = m_alive;
	art->controller = this;
	art->generation = m_art_generation;
	art->path       = kyty_game_get_entry(game).path;
	art->kind       = kyty_game_get_entry(game).kind;
	art->cancel     = m_art_cancel;
	// An unowned GTask, a recorded exception (p6-u9-6): the alive flag drops a late result.
	GTask* task = g_task_new(nullptr, nullptr, &OnArt, nullptr);
	g_task_set_task_data(task, art, [](gpointer data) { delete static_cast<ArtTask*>(data); });
	g_task_run_in_thread(task, &RunArt);
	g_object_unref(task);
}

void LibraryController::RunArt(GTask* task, gpointer /*source*/, gpointer task_data,
                               GCancellable* /*cancellable*/) {
	auto* art = static_cast<ArtTask*>(task_data);
	if (!art->cancel->IsCancelled()) {
		art->art = GObjectPtr<GdkTexture>::Adopt(
		    DecodePng(core::ReadPic0(art->path, art->kind).value_or(""), art->path, "pic0.png"));
	}
	g_task_return_boolean(task, TRUE);
}

void LibraryController::OnArt(GObject* /*source*/, GAsyncResult* result, gpointer /*data*/) {
	auto* art = static_cast<ArtTask*>(g_task_get_task_data(G_TASK(result)));
	if (!*art->alive || art->generation != art->controller->m_art_generation) {
		return;
	}
	auto* self = art->controller;
	self->m_art_cancel.reset();
	if (self->m_callbacks.background_art) {
		self->m_callbacks.background_art(art->art.get());
	}
}

void LibraryController::ApplyLightbar() {
	m_context.services.controller.SetLightbarColor(
	    ConfiguredLightbar(m_context.settings.loaded.settings.controller.color));
}

void LibraryController::Notify(const std::string& heading, const std::string& details) {
	if (m_callbacks.notify) {
		m_callbacks.notify(heading, details);
	}
}

} // namespace kyty::gtk
