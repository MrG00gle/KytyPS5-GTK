#pragma once

// The background work of the game library (task 20.4; plan D28, D29; Requirements 7.1-7.4, 7.7,
// 7.10, 7.16, 8.1-8.9; LFI-08, LFI-09, LFI-21, LFI-23, LFI-39; HC-14, HC-21, HC-22, HC-23). It
// fills LauncherContext::library, which the main window's views show.
// - Scans: core::NormalizeGameFolders and core::ScanGameFolders of the Settings_Store's
//   Game_Folders run in a GTask thread. Every scan has a generation; a new one cancels the
//   previous one's CancelToken, and only the latest generation's result is applied, on the main
//   thread: the entries replace the library in one splice (D28: the sort, the search text, and the
//   selected path stay, 7.4), the statuses come from LauncherContext::compatibility, and the icons
//   and sizes of games that were listed before stay until the new ones arrive. After every applied
//   scan, core::MigrateLegacyGamePaths, and an automatic save when it changed something (HC-22).
// - The details of each scan, in one GTask thread per generation, delivered to the main thread in
//   batches: the icons (core::ReadIcon0; a PNG decoded with gdk_texture_new_from_bytes, anything
//   else shows the placeholder, 7.16, HC-23), then the sizes (the .zar file, or the regular files
//   below the directory without following symbolic links, as the Qt launcher, LFI-39). An icon or
//   art PNG whose IHDR size would need more than 256 MiB decoded (4 bytes per pixel, 8 for 16-bit
//   PNGs) is not decoded (Qt 6's default QImageReader allocation limit, which the Qt launcher had).
// - Compatibility (HC-21), once in Start: core::CompatibilityLoader on the Data_Directory, applied
//   through ApplyLoadResult to the listed games without a rescan (8.5); its warnings are logged
//   (g_warning for the ones Requirements 8.4 and 8.6 ask for, g_message for the others). With
//   --local: core::LoadLocalCompatibility of ./compatibility_db.json and no download; the list
//   view's Status and Comment edits go to every game with the same title ID and are saved with
//   core::SaveLocalCompatibility (LFI-21), unless the file could not be loaded (then it is kept as
//   it is and the edits stay in memory).
// - The selection: the title sound player follows it (HC-14, 8.7, 8.8), the configured lightbar
//   color is set again (LFI-23), and the selected game's background art (core::ReadPic0, a PNG) is
//   decoded in a GTask thread (LFI-08).
// - SettingsSaved (the global settings window wrote the Settings_Store): a changed Game_Folder list
//   rescans (7.1, 7.2), and the lightbar shows the saved color.
// Destroying the controller cancels the pending work; no result and no callback arrives after it.
// Main thread only.

#include "launcher/core/cancelToken.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/gameScanner.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/launcherContext.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kyty::gtk {

class LibraryController {
public:
	struct Callbacks {
		// A scan's result was applied; warnings is empty when every folder could be read (7.10).
		std::function<void(const std::vector<core::ScanWarning>& warnings)> scanned;
		// The selected game changed, also to none (nullptr), and after a rescan that kept it.
		std::function<void(KytyGame* game)> selection_changed;
		// The selected game's background art, or nullptr (no selection, no usable pic0.png).
		std::function<void(GdkTexture* art)> background_art;
		// A failure for the user (a failed automatic save, a --local file that failed): a short
		// translated heading and the core's message with the path and the reason.
		std::function<void(const std::string& heading, const std::string& details)> notify;
	};

	// What the harness and the UI tests look at.
	struct Statistics {
		uint64_t scans_started = 0;
		uint64_t scans_applied = 0;
		uint64_t details_done  = 0; // details runs that delivered everything
		// Compatibility: the source of the applied database ("download", "cache", "local"), or ""
		// while none is applied; when the loader finished and when its statuses were on the games
		// (g_get_monotonic_time() in microseconds).
		std::string compatibility_source;
		int64_t     compatibility_finished_us = 0;
		int64_t     compatibility_applied_us  = 0;
		bool        compatibility_finished    = false;
		// --local: the file could not be loaded, so edits are not saved.
		bool local_load_failed = false;
	};

	LibraryController(LauncherContext& context, Callbacks callbacks);
	~LibraryController();

	LibraryController(const LibraryController&)            = delete;
	LibraryController& operator=(const LibraryController&) = delete;

	// The compatibility database, the lightbar color, and the first scan. Only the first call does
	// anything.
	void Start();
	// Scans every Game_Folder again (F5, 7.4).
	void Rescan();
	// After the global settings were saved.
	void SettingsSaved();

	// --local edits of the list view, which already shows them on game.
	void StatusEdited(KytyGame* game, core::CompatibilityStatus status);
	void CommentEdited(KytyGame* game, const std::string& comment);

	[[nodiscard]] bool                                  Scanning() const { return m_scanning; }
	[[nodiscard]] const std::vector<core::ScanWarning>& Warnings() const { return m_warnings; }
	// The Game_Folders of the last applied scan, as stored.
	[[nodiscard]] const std::vector<std::string>& ScannedFolders() const {
		return m_scanned_folders;
	}
	[[nodiscard]] const Statistics& Stats() const { return m_stats; }

private:
	struct ScanTask;
	struct DetailsTask;
	struct ArtTask;
	struct Batch;

	static void     RunScan(GTask* task, gpointer source, gpointer task_data,
	                        GCancellable* cancellable);
	static void     OnScanned(GObject* source, GAsyncResult* result, gpointer data);
	static void     RunDetails(GTask* task, gpointer source, gpointer task_data,
	                           GCancellable* cancellable);
	static void     OnDetailsDone(GObject* source, GAsyncResult* result, gpointer data);
	static gboolean ApplyBatch(gpointer data);
	static void RunArt(GTask* task, gpointer source, gpointer task_data, GCancellable* cancellable);
	static void OnArt(GObject* source, GAsyncResult* result, gpointer data);
	static void OnSelectionChanged(GObject* selection, GParamSpec* pspec, gpointer data);

	void ApplyScan(ScanTask& scan);
	void StartDetails();
	void StartCompatibility();
	void ApplyCompatibility(const core::CompatibilityLoadResult& result);
	void ApplyStatuses();
	void SaveLocal();
	void HandleSelection();
	void LoadArt(KytyGame* game);
	void ApplyLightbar();
	void Notify(const std::string& heading, const std::string& details);

	LauncherContext& m_context;
	Callbacks        m_callbacks;
	// Read on the main thread by results and batches that arrive after the controller is gone.
	std::shared_ptr<bool> m_alive      = std::make_shared<bool>(true);
	bool                  m_started    = false;
	bool                  m_scanning   = false;
	bool                  m_replacing  = false; // the library's splice briefly clears the selection
	uint64_t              m_generation = 0;     // of the latest scan
	uint64_t              m_art_generation = 0;
	std::shared_ptr<core::CancelToken> m_scan_cancel;
	std::shared_ptr<core::CancelToken> m_details_cancel;
	std::shared_ptr<core::CancelToken> m_art_cancel;
	std::vector<std::string>           m_requested_folders; // of the latest scan
	std::vector<std::string>           m_scanned_folders;
	std::vector<core::ScanWarning>     m_warnings;
	// The listed games by their native path (the store holds their references).
	std::unordered_map<std::string, KytyGame*> m_games;
	std::filesystem::path                      m_selected_path;
	std::unique_ptr<core::CompatibilityLoader> m_loader;
	std::filesystem::path                      m_local_path; // --local
	SignalScope                                m_selection_signal;
	Statistics                                 m_stats;
};

} // namespace kyty::gtk
