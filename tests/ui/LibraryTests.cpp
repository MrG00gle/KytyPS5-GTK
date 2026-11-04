// UI tests of the game library (task 22.4; Requirements 7.3, 7.4, 7.7, 7.11, 7.13, 7.16, 7.17,
// 18.3; phase 7 plan D15, D24, D26), each on Wayland and on X11. Most cases run the real
// KytyApplication (UiContext::RunApplication) over the fixture games (uiFixtures.h) and look at
// its main window's list and grid views.
//
// library_search_timing (W1, Requirement 7.13): 1,000 Game_Entries (the 5 fixture games and 995
// synthetic ones without icons) in a 1280x800 main window; for the list view and then the grid
// view (each shown, the other one unmapped), the queries "clear to all" (from a search with one
// result), "all to 'game 1'" (111 games), "all to one title ID", and "g", "ga", "gam" typed one
// character at a time. The clock starts right before gtk_editable_set_text on the main window's
// search entry (search-delay 0) and stops at the first after-paint of the window's frame clock
// after the entry's search-changed handlers, which filter the library (the filter model emits its
// items-changed inside them). 2 warm-up runs, then 9 measured runs; min, median, and max are
// printed for every query and view, and the median must be at most 100 ms where
// KYTY_UI_STRICT_FRAME_TIMING is defined (every build type but Debug, plan D15). The shown games
// are checked against core::FilterIndices in every build type. library_search_calibration runs
// the same measurement over a column view of 8 plain GtkLabel columns (no launcher view code) and
// only prints it: GTK's own cost on this machine.
//
// The other cases check what the views draw (their KytyGameCell widgets, gameCell.h):
// library_list_columns (7.8), library_grid_matches_list and library_grid_icon_width (7.11),
// library_view_persisted (7.17), library_sort_columns (7.12), library_refresh_keeps_sort_search
// (7.4), library_search_semantics (7.13), library_scan_responsive (7.7: a 10 ms probe and a key
// during a rescan of 5,000 games), library_placeholder_icon (7.16), library_metadata_unavailable
// (7.9, and the emblems at the icon's corner, W4-07), library_local_edit (--local, W4-06).

#include "launcher/core/cancelToken.h"
#include "launcher/core/compatibility.h"
#include "launcher/core/gameLibrary.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/core/gameScanner.h"
#include "launcher/core/scanFileSystem.h"
#include "launcher/gtk/application.h"
#include "launcher/gtk/byteSize.h"
#include "launcher/gtk/gameCell.h"
#include "launcher/gtk/gameListModel.h"
#include "launcher/gtk/gameObject.h"
#include "launcher/gtk/gameViewSupport.h"
#include "launcher/gtk/gobjectPtr.h"
#include "launcher/gtk/libraryController.h"
#include "launcher/gtk/mainWindow.h"
#include "ui/support/uiContext.h"
#include "ui/support/uiDriver.h"
#include "ui/support/uiFixtures.h"
#include "ui/support/uiHarness.h"
#include "ui/support/uiLibrary.h"

#include <adwaita.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <gtk/gtk.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs   = std::filesystem;
namespace core = kyty::core;
using kyty::gtk::GObjectPtr;
using kyty::gtk::SignalScope;
using KytyUi::Check;

// ---- W1: the painted frame after a search (plan D24, D15)

constexpr int         kSyntheticGames = 995; // with the 5 fixture games: 1,000 entries
constexpr int         kWarmupRuns     = 2;
constexpr int         kMeasuredRuns   = 9;
constexpr double      kFrameLimitMs   = 100.0; // Requirement 7.13
constexpr int         kSettleMs       = 60;    // between two searches: the next frame callback
constexpr const char* kOneTitleId     = "PPSA20500"; // "Synthetic Game 500"

#ifdef KYTY_UI_STRICT_FRAME_TIMING
constexpr bool kStrictFrameTiming = true;
#else
constexpr bool kStrictFrameTiming = false;
#endif

const char* Backend() {
	const char* backend = g_getenv("GDK_BACKEND");
	return backend != nullptr ? backend : "default";
}

double Milliseconds(gint64 from_us, gint64 to_us) {
	return static_cast<double>(to_us - from_us) / 1000.0;
}

// One timed search; every time in milliseconds after the start (-1: did not happen).
struct SearchSample {
	double changed_ms = -1; // the search entry's search-changed emission ended (filtered)
	double items_ms   = -1; // the filter model's last items-changed ended
	double layout_ms  = -1; // the layout phase of the measured frame ended
	double painted_ms = -1; // the measured frame's after-paint
	guint  shown      = 0;  // the games shown afterwards
};

// Times searches in window's search entry: Measure() sets the text and waits for the first
// after-paint of the window's frame clock after the entry's search-changed handlers ran.
class SearchClock {
public:
	SearchClock(GtkWidget* window, GtkWidget* search, GListModel* filtered, GListModel* shown)
	    : m_search(search), m_shown(shown) {
		GdkFrameClock* clock = gtk_widget_get_frame_clock(window);
		m_signals.push_back(SignalScope::Connect(
		    search, "search-changed", G_CALLBACK(&OnSearchChanged), this, G_CONNECT_AFTER));
		m_signals.push_back(SignalScope::Connect(
		    filtered, "items-changed", G_CALLBACK(&OnItemsChanged), this, G_CONNECT_AFTER));
		m_signals.push_back(
		    SignalScope::Connect(clock, "layout", G_CALLBACK(&OnLayout), this, G_CONNECT_AFTER));
		m_signals.push_back(
		    SignalScope::Connect(clock, "after-paint", G_CALLBACK(&OnAfterPaint), this));
	}

	SearchClock(const SearchClock&)            = delete;
	SearchClock& operator=(const SearchClock&) = delete;

	// The sample of one search for text; nullopt after a Fail.
	std::optional<SearchSample> Measure(const std::string& text, const std::string& what) {
		m_changed = m_items = m_layout = m_painted = -1;
		m_armed                                    = true;
		m_start                                    = g_get_monotonic_time();
		gtk_editable_set_text(GTK_EDITABLE(m_search), text.c_str());
		const bool painted = KytyUi::WaitFor([this]() { return m_painted >= 0; }, 10000,
		                                     "a painted frame after " + what);
		m_armed            = false;
		if (!painted) {
			return std::nullopt;
		}
		SearchSample sample;
		sample.changed_ms = Milliseconds(m_start, m_changed);
		sample.items_ms   = m_items >= 0 ? Milliseconds(m_start, m_items) : -1;
		sample.layout_ms  = m_layout >= 0 ? Milliseconds(m_start, m_layout) : -1;
		sample.painted_ms = Milliseconds(m_start, m_painted);
		sample.shown      = g_list_model_get_n_items(m_shown);
		// The next frame callback, and whatever the frame queued, before the next search.
		KytyUi::Pump(kSettleMs);
		return sample;
	}

private:
	static void OnSearchChanged(GtkSearchEntry* /*entry*/, gpointer data) {
		auto* self = static_cast<SearchClock*>(data);
		if (self->m_armed && self->m_changed < 0) {
			self->m_changed = g_get_monotonic_time();
		}
	}
	static void OnItemsChanged(GListModel* /*model*/, guint /*position*/, guint /*removed*/,
	                           guint /*added*/, gpointer data) {
		auto* self = static_cast<SearchClock*>(data);
		if (self->m_armed && self->m_painted < 0) {
			self->m_items = g_get_monotonic_time();
		}
	}
	static void OnLayout(GdkFrameClock* /*clock*/, gpointer data) {
		auto* self = static_cast<SearchClock*>(data);
		if (self->m_armed && self->m_changed >= 0 && self->m_painted < 0) {
			self->m_layout = g_get_monotonic_time();
		}
	}
	static void OnAfterPaint(GdkFrameClock* /*clock*/, gpointer data) {
		auto* self = static_cast<SearchClock*>(data);
		if (self->m_armed && self->m_changed >= 0 && self->m_painted < 0) {
			self->m_painted = g_get_monotonic_time();
		}
	}

	GtkWidget*               m_search = nullptr;
	GListModel*              m_shown  = nullptr;
	std::vector<SignalScope> m_signals;
	bool                     m_armed   = false;
	gint64                   m_start   = 0;
	gint64                   m_changed = -1;
	gint64                   m_items   = -1;
	gint64                   m_layout  = -1;
	gint64                   m_painted = -1;
};

double Median(std::vector<double> values) {
	if (values.empty()) {
		return -1;
	}
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

// The measured samples of one query in one view.
struct QueryTimes {
	std::string               view;
	std::string               query;
	guint                     shown = 0;
	std::vector<SearchSample> samples;
};

// The queries of plan D24 (each from the state the previous step left; "" first): the expected
// number of shown games comes from core::FilterIndices over the library's entries.
struct Step {
	const char* name;     // the query's name in the output, empty for a reset
	const char* text;     // the search text
	bool        measured; // false: only restores the state for the next query
};

constexpr Step kSteps[] = {
    {"all to 'game 1'", "game 1", true},
    {"", "", false},
    {"all to one title ID", kOneTitleId, true},
    {"clear to all", "", true},
    {"typed 'g'", "g", true},
    {"typed 'ga'", "ga", true},
    {"typed 'gam'", "gam", true},
    {"", "", false},
};

// Runs the warm-ups and the measured runs of every step in view; adds the measured samples to
// times (one QueryTimes per measured step). False after a Fail.
bool TimeSearches(SearchClock& clock, const std::vector<core::GameEntry>& entries,
                  const std::string& view, std::vector<QueryTimes>& times) {
	std::map<std::string, std::size_t> index;
	for (const Step& step: kSteps) {
		if (step.measured) {
			index[step.name] = times.size();
			times.push_back({view, step.name, 0, {}});
		}
	}
	for (int run = 0; run < kWarmupRuns + kMeasuredRuns; run++) {
		for (const Step& step: kSteps) {
			const std::string what   = view + ", \"" + step.text + "\" (" +
			                           (step.measured ? step.name : "reset") + ", run " +
			                           std::to_string(run + 1) + ")";
			const auto        sample = clock.Measure(step.text, what);
			if (!sample) {
				return false;
			}
			const auto expected = core::FilterIndices(entries, step.text).size();
			if (!Check(sample->shown == expected, what + ": " + std::to_string(sample->shown) +
			                                          " games shown, FilterIndices gives " +
			                                          std::to_string(expected))) {
				return false;
			}
			if (step.measured && run >= kWarmupRuns) {
				QueryTimes& query = times[index[step.name]];
				query.shown       = sample->shown;
				query.samples.push_back(*sample);
			}
		}
	}
	return true;
}

// Prints every query's numbers; with limit, the median of each must be at most 100 ms where
// KYTY_UI_STRICT_FRAME_TIMING is defined (plan D15), and it is printed only otherwise.
void ReportTimes(const char* case_name, const std::vector<QueryTimes>& times, bool limit) {
	const bool strict = limit && kStrictFrameTiming;
	for (const QueryTimes& query: times) {
		std::vector<double> painted;
		std::vector<double> changed;
		std::vector<double> layout;
		for (const SearchSample& sample: query.samples) {
			painted.push_back(sample.painted_ms);
			changed.push_back(sample.changed_ms);
			layout.push_back(sample.layout_ms);
		}
		const double median = Median(painted);
		const double min    = *std::min_element(painted.begin(), painted.end());
		const double max    = *std::max_element(painted.begin(), painted.end());
		std::printf("%s: %s %s, %s (%u shown): painted frame min %.1f, median %.1f, max %.1f ms "
		            "(n=%zu); filtered (search-changed end) median %.1f ms, layout end median "
		            "%.1f ms\n",
		            case_name, Backend(), query.view.c_str(), query.query.c_str(), query.shown, min,
		            median, max, painted.size(), Median(changed), Median(layout));
		std::printf("TIMING %s %s %s \"%s\" shown=%u min=%.1f median=%.1f max=%.1f n=%zu "
		            "filtered=%.1f layout=%.1f\n",
		            case_name, Backend(), query.view.c_str(), query.query.c_str(), query.shown, min,
		            median, max, painted.size(), Median(changed), Median(layout));
		if (strict) {
			Check(median <= kFrameLimitMs,
			      query.view + ", " + query.query + ": the median painted frame (" +
			          std::to_string(median) + " ms) is at most 100 ms (Requirement 7.13)");
		}
	}
	if (limit && !strict) {
		std::printf("%s: Debug build: the 100 ms limit of Requirement 7.13 is not asserted (plan "
		            "D15)\n",
		            case_name);
	}
}

// The 1,000 games: the fixture games and kSyntheticGames synthetic ones in two Game_Folders.
std::vector<fs::path> WriteThousandGames(KytyUi::UiContext& ui) {
	const fs::path games = ui.WriteFixtureGames();
	if (!Check(!games.empty(), "the fixture games are written")) {
		return {};
	}
	const fs::path synthetic = ui.Root() / "synthetic";
	if (!Check(KytyUi::WriteSyntheticGames(synthetic, kSyntheticGames) == kSyntheticGames,
	           "995 synthetic games are written")) {
		return {};
	}
	return {games, synthetic};
}

std::vector<core::GameEntry> Entries(GListModel* games) {
	std::vector<core::GameEntry> entries;
	for (guint i = 0; i < g_list_model_get_n_items(games); i++) {
		auto game =
		    GObjectPtr<KytyGame>::Adopt(static_cast<KytyGame*>(g_list_model_get_item(games, i)));
		entries.push_back(kyty_game_get_entry(game.get()));
	}
	return entries;
}

// ---- The calibration: 8 plain GtkLabel columns

constexpr int kCalibrationWidths[] = {320, 110, 120, 150, 100, 320, 150, 240};

const std::string& RowValue(const core::GameRow& row, int column) {
	static const std::string kNone;
	switch (column) {
		case 0: return row.title;
		case 1: return row.title_id;
		case 2: return row.game_version;
		case 3: return row.firmware_version;
		case 5: return row.path;
		case 6: return row.status;
		case 7: return row.comment;
		default: return kNone;
	}
}

void OnCalibrationSetup(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer /*data*/) {
	GtkWidget* label = gtk_label_new(nullptr);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
	gtk_column_view_cell_set_child(GTK_COLUMN_VIEW_CELL(object), label);
}

void OnCalibrationBind(GtkSignalListItemFactory* /*factory*/, GObject* object, gpointer data) {
	auto*       cell   = GTK_COLUMN_VIEW_CELL(object);
	auto*       game   = KYTY_GAME(gtk_column_view_cell_get_item(cell));
	const auto& row    = kyty_game_get_row(game);
	const int   column = GPOINTER_TO_INT(data);
	std::string text   = RowValue(row, column);
	if (column == 4) {
		text = row.size_bytes ? std::to_string(*row.size_bytes) : "\u2014";
	}
	gtk_label_set_text(GTK_LABEL(gtk_column_view_cell_get_child(cell)), text.c_str());
}

void OnCalibrationSearch(GtkSearchEntry* entry, gpointer data) {
	static_cast<kyty::gtk::GameListModel*>(data)->SetSearchText(
	    gtk_editable_get_text(GTK_EDITABLE(entry)));
}

} // namespace

// ---- W1 (plan D24): the painted frame after a search over 1,000 games, list and grid

KYTY_UI_CASE(library_search_timing) {
	KytyUi::UiContext ui;
	const auto        folders = WriteThousandGames(ui);
	if (folders.empty() || !ui.StoreGameFolders(folders) ||
	    !KytyUi::StoreSettings(
	        ui, [](core::LauncherSettings& settings) { settings.window = {1280, 800, false}; })) {
		return;
	}
	ui.ServeCompatibility();
	std::vector<QueryTimes> times;
	const int status = ui.RunApplication([&](KytyApplication* application, KytyMainWindow* window) {
		auto& library = KytyUi::ContextOf(application).library;
		if (KytyUi::WaitForLibrary(window, 30000) &&
		    Check(g_list_model_get_n_items(library.Games()) == 1000, "1,000 games are listed")) {
			GtkWidget* search = KytyUi::FindByBuildableId(GTK_WIDGET(window), "search_entry");
			Check(search != nullptr && GTK_IS_SEARCH_ENTRY(search) &&
			          gtk_search_entry_get_search_delay(GTK_SEARCH_ENTRY(search)) == 0,
			      "the search entry searches at once (search-delay 0)");
			std::printf("library_search_timing: %s, main window %dx%d\n", Backend(),
			            gtk_widget_get_width(GTK_WIDGET(window)),
			            gtk_widget_get_height(GTK_WIDGET(window)));
			const auto entries = Entries(library.Games());
			for (const bool grid: {false, true}) {
				if (search == nullptr || !KytyUi::ShowGridView(application, window, grid)) {
					break;
				}
				SearchClock clock(GTK_WIDGET(window), search, library.Filtered(), library.Sorted());
				if (!TimeSearches(clock, entries, grid ? "grid" : "list", times)) {
					break;
				}
			}
		}
		KytyUi::QuitApplication(application);
	});
	Check(status == 0, "the application ends with status 0");
	if (KytyUi::Failures() == 0) {
		ReportTimes("library_search_timing", times, true);
	}
}

// GTK's own cost (printed only): the same 1,000 games and searches in a 1280x800 window with a
// search entry and a column view of 8 plain GtkLabel columns of the list view's widths.
KYTY_UI_CASE(library_search_calibration) {
	KytyUi::UiContext ui;
	const auto        folders = WriteThousandGames(ui);
	if (folders.empty()) {
		return;
	}
	std::vector<std::string> stored;
	for (const auto& folder: folders) {
		stored.push_back(folder.string());
	}
	const auto        file_system = core::MakeHostScanFileSystem();
	const auto        reader      = core::MakeHostMetadataReader();
	core::CancelToken cancel;
	auto result = core::ScanGameFolders(core::NormalizeGameFolders(stored, *file_system),
	                                    *file_system, *reader, cancel);
	kyty::gtk::GameListModel model;
	model.Replace(std::move(result.entries));
	if (!Check(g_list_model_get_n_items(model.Games()) == 1000, "1,000 games are scanned")) {
		return;
	}

	GtkWidget* view =
	    gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(G_OBJECT(model.Selection()))));
	for (int column = 0; column < 8; column++) {
		GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
		g_signal_connect(factory, "setup", G_CALLBACK(&OnCalibrationSetup), nullptr);
		g_signal_connect(factory, "bind", G_CALLBACK(&OnCalibrationBind), GINT_TO_POINTER(column));
		GtkColumnViewColumn* view_column = gtk_column_view_column_new("Column", factory);
		gtk_column_view_column_set_fixed_width(view_column, kCalibrationWidths[column]);
		gtk_column_view_column_set_expand(view_column, column == 7 ? TRUE : FALSE);
		gtk_column_view_append_column(GTK_COLUMN_VIEW(view), view_column);
		g_object_unref(view_column);
	}
	GtkWidget* scrolled = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
	GtkWidget* search = gtk_search_entry_new();
	gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(search), 0);
	const auto search_signal =
	    SignalScope::Connect(search, "search-changed", G_CALLBACK(&OnCalibrationSearch), &model);
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), search);
	gtk_box_append(GTK_BOX(box), scrolled);
	auto* window = GTK_WINDOW(adw_window_new());
	gtk_window_set_title(window, "library_search_calibration");
	gtk_window_set_default_size(window, 1280, 800);
	adw_window_set_content(ADW_WINDOW(window), box);
	gtk_window_present(window);
	std::vector<QueryTimes> times;
	if (KytyUi::WaitFor([window]() { return gtk_widget_get_mapped(GTK_WIDGET(window)) != FALSE; },
	                    5000, "the calibration window to be mapped") &&
	    KytyUi::WaitForPaintedFrame(GTK_WIDGET(window))) {
		KytyUi::Pump(200);
		SearchClock clock(GTK_WIDGET(window), search, model.Filtered(), model.Sorted());
		TimeSearches(clock, Entries(model.Games()), "8 GtkLabel columns", times);
	}
	gtk_window_destroy(window);
	KytyUi::Pump(100);
	if (KytyUi::Failures() == 0) {
		ReportTimes("library_search_calibration", times, false);
	}
}

// ---- The library's views (Requirements 7.3, 7.4, 7.8, 7.9, 7.11, 7.12, 7.13, 7.16, 7.17)

namespace {

using KytyUi::kAlphaTitle;
using KytyUi::kBetaTitle;
constexpr const char* kDash = "\u2014";

std::string CellText(GtkWidget* cell) {
	return cell != nullptr && KYTY_IS_GAME_CELL(cell)
	           ? kyty_game_cell_get_text(KYTY_GAME_CELL(cell))
	           : std::string();
}

std::string OrDash(const std::string& value) {
	return value.empty() ? std::string(kDash) : value;
}

// What a row of the list shows for game, column by column (LFI-01's columns).
std::vector<std::string> ExpectedRow(KytyGame* game) {
	const core::GameRow& row  = kyty_game_get_row(game);
	const auto           size = kyty_game_get_size(game);
	return {row.title,
	        row.title_id,
	        OrDash(row.game_version),
	        OrDash(row.firmware_version),
	        size ? kyty::gtk::FormatByteSize(*size) : std::string(kDash),
	        row.path,
	        kyty::gtk::StatusText(kyty_game_get_status(game)),
	        kyty_game_get_comment(game)};
}

std::vector<std::string> Texts(const std::vector<GtkWidget*>& cells) {
	std::vector<std::string> texts;
	for (GtkWidget* cell: cells) {
		texts.push_back(CellText(cell));
	}
	return texts;
}

std::string Join(const std::vector<std::string>& values) {
	std::string text;
	for (const auto& value: values) {
		text += (text.empty() ? "" : " | ") + value;
	}
	return text;
}

// Clicks the header of the column with id (ascending, or descending).
void SortByHeader(GtkColumnView* view, std::string_view id, bool descending) {
	gtk_column_view_sort_by_column(view, KytyUi::ColumnById(view, id),
	                               descending ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

std::size_t IndexOf(const std::vector<std::string>& titles, std::string_view title) {
	return static_cast<std::size_t>(std::find(titles.begin(), titles.end(), title) -
	                                titles.begin());
}

// The bounds of widget in the coordinates of ancestor (empty after a Fail).
graphene_rect_t BoundsIn(GtkWidget* widget, GtkWidget* ancestor) {
	graphene_rect_t bounds {};
	if (!Check(widget != nullptr && gtk_widget_compute_bounds(widget, ancestor, &bounds) != FALSE,
	           "the bounds of a widget in its ancestor")) {
		return GRAPHENE_RECT_INIT(0, 0, 0, 0);
	}
	return bounds;
}

// The center of widget in the coordinates of ancestor.
graphene_point_t CenterIn(GtkWidget* widget, GtkWidget* ancestor) {
	const graphene_rect_t bounds = BoundsIn(widget, ancestor);
	return GRAPHENE_POINT_INIT(bounds.origin.x + bounds.size.width / 2,
	                           bounds.origin.y + bounds.size.height / 2);
}

double Distance(const graphene_point_t& a, const graphene_point_t& b) {
	return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y));
}

// The emblem of the drawn icon of cell sits on the icon's corner (W4-07, plan D26): it covers the
// corner of the texture or placeholder glyph (bottom right for the metadata emblem, top left for
// the running emblem), and its center is at most limit pixels from it.
bool EmblemOnCorner(GtkWidget* cell, bool metadata, double limit, const std::string& what) {
	auto*           game_cell = KYTY_GAME_CELL(cell);
	GtkWidget*      emblem    = metadata ? kyty_game_cell_get_metadata_emblem(game_cell)
	                                     : kyty_game_cell_get_running_emblem(game_cell);
	graphene_rect_t icon {};
	if (!Check(emblem != nullptr && gtk_widget_get_mapped(emblem) != FALSE &&
	               kyty_game_cell_get_icon_bounds(game_cell, &icon),
	           what + ": the emblem is shown over the drawn icon")) {
		return false;
	}
	const graphene_point_t corner = metadata ? GRAPHENE_POINT_INIT(icon.origin.x + icon.size.width,
	                                                               icon.origin.y + icon.size.height)
	                                         : icon.origin;
	const graphene_rect_t  bounds = BoundsIn(emblem, cell);
	graphene_rect_t        covered {};
	graphene_rect_inset_r(&bounds, -1, -1, &covered);
	const double distance = Distance(CenterIn(emblem, cell), corner);
	return Check(graphene_rect_contains_point(&covered, &corner) && distance <= limit,
	             what + ": the emblem covers the icon's corner, its center " +
	                 std::to_string(distance) + " px from it (at most " + std::to_string(limit) +
	                 ")");
}

// Selects the game in the main window and waits for a painted frame.
void ShowSelected(KytyMainWindow* window, kyty::gtk::GameListModel& library, const char* title) {
	KytyUi::SelectTitle(library, title);
	KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
}

} // namespace

// Requirement 7.8, LFI-01: every row shows the title, title ID, game version, the required system
// software version as major.minor or major.minor.patch, the size (a dash while it is unknown), the
// path, the compatibility status with its colored indicator, and the comment.
KYTY_UI_CASE(library_list_columns) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty() ||
	    !KytyUi::WriteGameWithParam(
	        games / "Patch Game", KytyUi::ParamJsonWithFirmware("Patch Game", "PPSA03001", "03.100",
	                                                            "0x0123450000000000"))) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&          library = KytyUi::ContextOf(application).library;
		GtkColumnView* view    = KytyUi::ListViewOf(window);
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		Check(g_list_model_get_n_items(library.Games()) == 6, "6 games are listed");
		for (const std::string& title: KytyUi::ShownTitles(library)) {
			KytyGame*  game  = KytyUi::GameByTitle(library, title);
			const auto shown = Texts(KytyUi::RowCells(view, title));
			Check(shown == ExpectedRow(game), "the row of " + title + " shows [" + Join(shown) +
			                                      "], expected [" + Join(ExpectedRow(game)) + "]");
		}
		Check(kyty_game_get_row(KytyUi::GameByTitle(library, kAlphaTitle)).firmware_version ==
		              "1.50" &&
		          kyty_game_get_row(KytyUi::GameByTitle(library, "Patch Game")).firmware_version ==
		              "1.23.45",
		      "the system software versions read 1.50 and 1.23.45 (major.minor[.patch])");
		// The status indicator has the status's color class.
		const auto alpha = KytyUi::RowCells(view, kAlphaTitle);
		GtkWidget* indicator =
		    alpha.size() == 8 ? kyty_game_cell_get_indicator(KYTY_GAME_CELL(alpha[6])) : nullptr;
		Check(indicator != nullptr &&
		          gtk_widget_has_css_class(indicator, "status-in-game") != FALSE &&
		          gtk_widget_get_mapped(indicator) != FALSE,
		      "Alpha Game's status indicator is shown in the In game color");
		Check(KytyUi::Capture(GTK_WINDOW(window), "list"), "the list view renders");
		// The columns past the window's width (Status, Comments): the list scrolled to its end.
		if (GtkWidget* scrolled =
		        gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_SCROLLED_WINDOW);
		    Check(scrolled != nullptr, "the list is in a scrolled window")) {
			GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
			gtk_adjustment_set_value(h,
			                         gtk_adjustment_get_upper(h) - gtk_adjustment_get_page_size(h));
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
			Check(KytyUi::Capture(GTK_WINDOW(window), "list-end"),
			      "the list's last columns render");
			gtk_adjustment_set_value(h, 0);
		}
		// An unknown size shows a dash (as before the details arrive).
		KytyGame*  beta = KytyUi::GameByTitle(library, kBetaTitle);
		const auto size = kyty_game_get_size(beta);
		kyty_game_set_size(beta, std::nullopt);
		KytyUi::WaitFor(
		    [&]() {
			    const auto cells = KytyUi::RowCells(view, kBetaTitle);
			    return cells.size() == 8 && CellText(cells[4]) == kDash;
		    },
		    2000, "Beta Quest's Size cell to show a dash while its size is unknown");
		kyty_game_set_size(beta, size);
		KytyUi::WaitFor(
		    [&]() {
			    const auto cells = KytyUi::RowCells(view, kBetaTitle);
			    return cells.size() == 8 && size &&
			           CellText(cells[4]) == kyty::gtk::FormatByteSize(*size);
		    },
		    2000, "Beta Quest's Size cell to show the size again");
	});
}

// Requirement 7.11, LFI-02: the grid shows the same games as the list, in the same order, over the
// same selection.
KYTY_UI_CASE(library_grid_matches_list) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&       library = KytyUi::ContextOf(application).library;
		const auto  titles  = KytyUi::ShownTitles(library);
		std::size_t rows    = 0;
		for (const auto& title: titles) {
			rows += KytyUi::RowCells(KytyUi::ListViewOf(window), title).size() == 8 ? 1 : 0;
		}
		Check(rows == titles.size(), "the list shows a row for each of the 5 games");
		KytyUi::SelectTitle(library, kBetaTitle);
		if (!KytyUi::ShowGridView(application, window, true)) {
			return;
		}
		GtkGridView* grid = KytyUi::GridViewOf(window);
		Check(gtk_grid_view_get_model(grid) == GTK_SELECTION_MODEL(library.Selection()),
		      "the grid shows the list's selection model");
		std::vector<std::string> tiles;
		for (const auto& title: titles) {
			if (KytyUi::GridTile(grid, title) != nullptr) {
				tiles.push_back(title);
			}
		}
		Check(tiles == titles, "the grid shows a tile for each game the list shows");
		// The tiles are laid out in the list's order (left to right, top to bottom).
		std::vector<std::pair<float, float>> places;
		for (const auto& title: titles) {
			const graphene_rect_t bounds =
			    BoundsIn(KytyUi::GridTile(grid, title), GTK_WIDGET(grid));
			places.emplace_back(bounds.origin.y, bounds.origin.x);
		}
		Check(std::is_sorted(places.begin(), places.end()),
		      "the tiles are in the list's order, row by row");
		Check(library.Selected() == KytyUi::GameByTitle(library, kBetaTitle),
		      "Beta Quest, selected in the list, is selected in the grid");
		const guint position =
		    KytyUi::ShownPosition(library, KytyUi::GameByTitle(library, kAlphaTitle));
		gtk_selection_model_select_item(gtk_grid_view_get_model(grid), position, TRUE);
		KytyUi::ShowGridView(application, window, false);
		Check(library.Selected() == KytyUi::GameByTitle(library, kAlphaTitle),
		      "Alpha Game, selected in the grid, is selected in the list");
	});
}

// Requirement 7.11, LFI-03: the icon width from 128 to 320 in steps of 16 (default 224): the drawn
// icon is width x 9 / 16 high and wide, the placeholder half of it.
KYTY_UI_CASE(library_grid_icon_width) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&ui](KytyApplication* application, KytyMainWindow* window) {
		if (!KytyUi::ShowGridView(application, window, true)) {
			return;
		}
		GtkWidget* scale = KytyUi::FindByBuildableId(GTK_WIDGET(window), "icon_width_scale");
		if (!Check(scale != nullptr && GTK_IS_RANGE(scale), "the header bar has the slider")) {
			return;
		}
		GtkAdjustment* adjustment = gtk_range_get_adjustment(GTK_RANGE(scale));
		Check(gtk_adjustment_get_lower(adjustment) == 128 &&
		          gtk_adjustment_get_upper(adjustment) == 320 &&
		          gtk_adjustment_get_step_increment(adjustment) == 16 &&
		          gtk_range_get_value(GTK_RANGE(scale)) == 224,
		      "the slider goes from 128 to 320 in steps of 16 and starts at 224");
		GtkGridView* grid = KytyUi::GridViewOf(window);
		for (const int width: {224, 128, 320}) {
			gtk_range_set_value(GTK_RANGE(scale), width);
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
			const int       icon   = width * 9 / 16;
			GtkWidget*      alpha  = KytyUi::GridTile(grid, kAlphaTitle);
			GtkWidget*      broken = KytyUi::GridTile(grid, "Broken Metadata");
			graphene_rect_t a {};
			graphene_rect_t b {};
			const bool      ok = alpha != nullptr && broken != nullptr &&
			                     kyty_game_cell_get_icon_bounds(KYTY_GAME_CELL(alpha), &a) &&
			                     kyty_game_cell_get_icon_bounds(KYTY_GAME_CELL(broken), &b);
			Check(ok && static_cast<int>(a.size.width) == icon &&
			          static_cast<int>(a.size.height) == icon,
			      "at " + std::to_string(width) + " the icons are " + std::to_string(icon) +
			          " px square (" + std::to_string(a.size.width) + ")");
			Check(ok && static_cast<int>(b.size.width) == icon / 2,
			      "at " + std::to_string(width) + " the placeholder is " +
			          std::to_string(icon / 2) + " px (" + std::to_string(b.size.width) + ")");
			Check(ok && gtk_widget_get_width(alpha) >= width,
			      "at " + std::to_string(width) +
			          " the tile is at least as wide as the icon width");
			if (width == 224 || width == 320) {
				Check(KytyUi::Capture(GTK_WINDOW(window), "grid-" + std::to_string(width)),
				      "the grid renders at " + std::to_string(width));
			}
		}
		KytyUi::WaitFor([&ui]() { return KytyUi::StoredSettings(ui).grid_icon_width == 320; }, 3000,
		                "the icon width 320 to be stored");
	});
}

// Requirement 7.17, LFI-04: the view and the icon width come back after a restart.
KYTY_UI_CASE(library_view_persisted) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		KytyUi::ShowGridView(application, window, true);
		GtkWidget* scale = KytyUi::FindByBuildableId(GTK_WIDGET(window), "icon_width_scale");
		gtk_range_set_value(GTK_RANGE(scale), 160);
	});
	const auto stored = KytyUi::StoredSettings(ui);
	Check(stored.grid_view && stored.grid_icon_width == 160,
	      "Kyty.ini holds the grid view and the width 160 after the application quit");
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		GtkWidget* scale = KytyUi::FindByBuildableId(GTK_WIDGET(window), "icon_width_scale");
		Check(gtk_widget_get_mapped(GTK_WIDGET(KytyUi::GridViewOf(window))) != FALSE &&
		          scale != nullptr && gtk_range_get_value(GTK_RANGE(scale)) == 160,
		      "the restarted launcher shows the grid with the width 160");
		graphene_rect_t icon {};
		KytyUi::WaitFor(
		    [&]() {
			    GtkWidget* tile = KytyUi::GridTile(KytyUi::GridViewOf(window), kAlphaTitle);
			    return tile != nullptr &&
			           kyty_game_cell_get_icon_bounds(KYTY_GAME_CELL(tile), &icon) &&
			           static_cast<int>(icon.size.width) == 90;
		    },
		    2000, "the icons to be 90 px (160 x 9 / 16)");
		KytyUi::ShowGridView(application, window, false);
	});
	Check(!KytyUi::StoredSettings(ui).grid_view, "the list view is stored");
	KytyUi::RunWithLibrary(ui, [](KytyApplication* /*application*/, KytyMainWindow* window) {
		Check(gtk_widget_get_mapped(GTK_WIDGET(KytyUi::ListViewOf(window))) != FALSE,
		      "the next start shows the list view");
	});
}

// Requirement 7.12, LFI-06: each column sorts ascending on the first header click and reverses on
// the next, as core::SortRows; empty values first ascending; versions by their numbers.
KYTY_UI_CASE(library_sort_columns) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty() ||
	    !KytyUi::WriteGameWithParam(games / "Nine",
	                                KytyUi::ParamJson("Nine Version", "PPSA03009", "1.9")) ||
	    !KytyUi::WriteGameWithParam(games / "Ten",
	                                KytyUi::ParamJson("Ten Version", "PPSA03010", "1.10"))) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&          library = KytyUi::ContextOf(application).library;
		GtkColumnView* view    = KytyUi::ListViewOf(window);
		const struct {
			const char*  id;
			core::Column column;
		} columns[] = {{"game", core::Column::Title},
		               {"serial", core::Column::TitleId},
		               {"game-version", core::Column::GameVersion},
		               {"firmware-version", core::Column::FirmwareVersion},
		               {"size", core::Column::Size},
		               {"path", core::Column::Path},
		               {"status", core::Column::Status},
		               {"comments", core::Column::Comment}};
		for (const auto& column: columns) {
			for (const bool descending: {false, true}) {
				SortByHeader(view, column.id, descending);
				const std::string what =
				    std::string(column.id) + (descending ? " descending" : " ascending");
				Check(library.SortColumn() == column.column &&
				          library.SortDescending() == descending,
				      "the " + what + " header click sorts the model");
				const auto shown = KytyUi::ShownTitles(library);
				Check(shown == KytyUi::ExpectedTitles(library, column.column, descending),
				      what + ": the order equals core::SortRows: " + Join(shown));
				auto* sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(view));
				Check(gtk_column_view_sorter_get_primary_sort_column(sorter) ==
				          KytyUi::ColumnById(view, column.id),
				      what + ": the header shows the sort");
			}
		}
		SortByHeader(view, "serial", false);
		Check(KytyUi::ShownTitles(library).front() == "Broken Metadata",
		      "ascending, the game without a serial comes first");
		SortByHeader(view, "game-version", false);
		auto shown = KytyUi::ShownTitles(library);
		Check(shown.front() == "Broken Metadata" &&
		          IndexOf(shown, "Nine Version") < IndexOf(shown, "Ten Version"),
		      "versions sort by their numbers (1.9 before 1.10), the empty one first: " +
		          Join(shown));
		SortByHeader(view, "game-version", true);
		shown = KytyUi::ShownTitles(library);
		Check(shown.back() == "Broken Metadata" &&
		          IndexOf(shown, "Ten Version") < IndexOf(shown, "Nine Version"),
		      "descending reverses it");
		// The rows the view shows follow the model.
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		std::vector<std::pair<float, std::string>> rows;
		for (const auto& title: shown) {
			const auto cells = KytyUi::RowCells(view, title);
			if (!cells.empty()) {
				rows.emplace_back(CenterIn(cells[0], GTK_WIDGET(view)).y, title);
			}
		}
		std::sort(rows.begin(), rows.end());
		std::vector<std::string> drawn;
		for (const auto& row: rows) {
			drawn.push_back(row.second);
		}
		Check(drawn == shown, "the rows are drawn in the sorted order: " + Join(drawn));
	});
}

// Requirement 7.4, LFI-07: F5 replaces the list and keeps the sort column, the direction, the
// search, and the selection.
KYTY_UI_CASE(library_refresh_keeps_sort_search) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&games](KytyApplication* application, KytyMainWindow* window) {
		auto&          library    = KytyUi::ContextOf(application).library;
		auto&          controller = kyty::gtk::MainWindowLibrary(window);
		GtkColumnView* view       = KytyUi::ListViewOf(window);
		GtkWidget*     search     = KytyUi::FindByBuildableId(GTK_WIDGET(window), "search_entry");
		SortByHeader(view, "size", true);
		gtk_editable_set_text(GTK_EDITABLE(search), "a");
		KytyUi::SelectTitle(library, kAlphaTitle);
		Check(KytyUi::WriteSyntheticGames(games / "new", 1) == 1, "a new game on disk");
		const auto applied = controller.Stats().scans_applied;
		const auto details = controller.Stats().details_done;
		gtk_widget_grab_focus(GTK_WIDGET(view));
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F5), "F5 is handled");
		KytyUi::WaitFor(
		    [&]() {
			    return controller.Stats().scans_applied > applied &&
			           controller.Stats().details_done > details;
		    },
		    8000, "F5 to rescan and the new details to arrive");
		Check(g_list_model_get_n_items(library.Games()) == 6 &&
		          KytyUi::GameByTitle(library, "Synthetic Game 1") != nullptr,
		      "the replaced list has the new game");
		Check(library.SortColumn() == core::Column::Size && library.SortDescending(),
		      "the sort column and the direction stay");
		Check(std::string(gtk_editable_get_text(GTK_EDITABLE(search))) == "a" &&
		          library.SearchText() == "a",
		      "the search stays");
		std::vector<std::string> expected;
		for (const auto& title: KytyUi::ExpectedTitles(library, core::Column::Size, true)) {
			if (core::MatchesSearch(kyty_game_get_entry(KytyUi::GameByTitle(library, title)),
			                        "a")) {
				expected.push_back(title);
			}
		}
		Check(KytyUi::ShownTitles(library) == expected,
		      "the new list is filtered and sorted as before: " +
		          Join(KytyUi::ShownTitles(library)));
		Check(library.Selected() == KytyUi::GameByTitle(library, kAlphaTitle),
		      "Alpha Game stays selected");
	});
}

// Requirement 7.13's semantics, LFI-05: trimmed, case-insensitive, by name or title ID, in both
// views.
KYTY_UI_CASE(library_search_semantics) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty()) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      library = KytyUi::ContextOf(application).library;
		GtkWidget* search  = KytyUi::FindByBuildableId(GTK_WIDGET(window), "search_entry");
		const auto entries = Entries(library.Games());
		const std::vector<std::pair<std::string, std::vector<std::string>>> queries = {
		    {"  ALPHA game  ", {kAlphaTitle}},
		    {"ppsa01002", {kBetaTitle}},
		    {"\xc3\x9cN\xc3\x8f",
		     {"\xc3\x9cn\xc3\xaf"
		      "c\xc3\xb6"
		      "de \"Odd\" Game"}},
		    {"PPSA0100", {}},
		    {"   ", {}},
		    {"no such game", {}}};
		for (const bool grid: {false, true}) {
			if (!KytyUi::ShowGridView(application, window, grid)) {
				return;
			}
			const char* view = grid ? "grid" : "list";
			for (const auto& [query, titles]: queries) {
				gtk_editable_set_text(GTK_EDITABLE(search), query.c_str());
				KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
				std::vector<std::string> expected;
				for (const std::size_t index: core::FilterIndices(entries, query)) {
					expected.push_back(entries[index].title_name);
				}
				std::sort(expected.begin(), expected.end());
				auto shown = KytyUi::ShownTitles(library);
				std::sort(shown.begin(), shown.end());
				Check(shown == expected,
				      std::string(view) + ", \"" + query +
				          "\": the games equal core::FilterIndices: " + Join(shown));
				if (!titles.empty()) {
					Check(shown == titles,
					      std::string(view) + ", \"" + query + "\" finds " + Join(titles));
				}
				// What the view draws: one row or tile per shown game.
				std::vector<std::string> drawn;
				for (const auto& title: shown) {
					const bool present =
					    grid ? KytyUi::GridTile(KytyUi::GridViewOf(window), title) != nullptr
					         : KytyUi::RowCells(KytyUi::ListViewOf(window), title).size() == 8;
					if (present) {
						drawn.push_back(title);
					}
				}
				Check(drawn == shown,
				      std::string(view) + ", \"" + query + "\": the view draws each shown game");
			}
		}
		Check(core::FilterIndices(entries, "PPSA0100").size() == 4 &&
		          core::FilterIndices(entries, "   ").size() == 5,
		      "a title ID prefix finds the 4 games with one; spaces find every game");
		gtk_editable_set_text(GTK_EDITABLE(search), "");
	});
}

// Requirement 7.7: while the worker scans 5,000 game directories, the main loop dispatches a
// 10 ms periodic source and a key within 100 ms.
KYTY_UI_CASE(library_scan_responsive) {
	KytyUi::UiContext ui;
	const fs::path    folder = ui.Root() / "many";
	if (!Check(KytyUi::WriteSyntheticGames(folder, 5000) == 5000, "5,000 games are written") ||
	    !ui.StoreGameFolders({folder})) {
		return;
	}
	ui.ServeCompatibility();
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto&      controller = kyty::gtk::MainWindowLibrary(window);
		GtkWidget* search     = KytyUi::FindByBuildableId(GTK_WIDGET(window), "search_entry");
		struct Probe {
			kyty::gtk::LibraryController* controller = nullptr;
			gint64                        last       = 0;
			double                        worst_ms   = 0;
			int                           samples    = 0;
		} probe {&controller, 0, 0, 0};
		gtk_widget_grab_focus(GTK_WIDGET(KytyUi::ListViewOf(window)));
		const auto applied = controller.Stats().scans_applied;
		Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_F5), "F5 starts a rescan");
		Check(controller.Scanning(), "the rescan runs in the background");
		probe.last = g_get_monotonic_time();
		const kyty::gtk::SourceId source(g_timeout_add(
		    10,
		    [](gpointer data) -> gboolean {
			    auto*        p   = static_cast<Probe*>(data);
			    const gint64 now = g_get_monotonic_time();
			    // Only while the worker scans (the result is applied afterwards).
			    if (p->controller->Scanning()) {
				    p->worst_ms =
				        std::max(p->worst_ms, static_cast<double>(now - p->last) / 1000.0 - 10.0);
				    p->samples++;
			    }
			    p->last = now;
			    return G_SOURCE_CONTINUE;
		    },
		    &probe));
		bool                      key_sent = false;
		double                    key_ms   = -1;
		KytyUi::WaitFor(
		    [&]() {
			    if (!key_sent && probe.samples >= 3 && controller.Scanning()) {
				    key_sent           = true;
				    const gint64 start = g_get_monotonic_time();
				    Check(KytyUi::KeyPress(GTK_WIDGET(window), GDK_KEY_f, GDK_CONTROL_MASK),
				          "Ctrl+F is handled during the scan");
				    key_ms = static_cast<double>(g_get_monotonic_time() - start) / 1000.0;
			    }
			    return controller.Stats().scans_applied > applied;
		    },
		    60000, "the rescan of 5,000 games to be applied");
		std::printf("library_scan_responsive: %s: %d probes during the scan, worst dispatch "
		            "latency %.1f ms; Ctrl+F handled in %.1f ms\n",
		            Backend(), probe.samples, probe.worst_ms, key_ms);
		Check(probe.samples >= 5, "the scan took long enough to be probed (" +
		                              std::to_string(probe.samples) + " probes)");
		Check(probe.worst_ms <= 100.0,
		      "every probe ran within 100 ms (worst " + std::to_string(probe.worst_ms) + " ms)");
		Check(key_sent && key_ms >= 0 && key_ms <= 100.0,
		      "the key was handled within 100 ms (" + std::to_string(key_ms) + " ms)");
		GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window));
		Check(focus != nullptr && (focus == search || gtk_widget_is_ancestor(focus, search)),
		      "Ctrl+F focused the search entry");
		Check(g_list_model_get_n_items(KytyUi::ContextOf(application).library.Games()) == 5000,
		      "5,000 games are listed");
	});
}

// Requirement 7.16: a missing, oversized (more than 32 MiB), or undecodable icon0.png gives the
// placeholder icon, and the row is otherwise unchanged.
KYTY_UI_CASE(library_placeholder_icon) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty()) {
		return;
	}
	const struct {
		const char* title;
		const char* id;
	} cases[] = {
	    {"No Icon", "PPSA04001"}, {"Huge Icon", "PPSA04002"}, {"Broken Icon", "PPSA04003"}};
	for (const auto& game: cases) {
		if (!KytyUi::WriteGameWithParam(games / game.title,
		                                KytyUi::ParamJson(game.title, game.id, "01.000"))) {
			return;
		}
	}
	const fs::path  huge = games / "Huge Icon" / "sce_sys" / "icon0.png";
	std::error_code ec;
	if (!KytyUi::WriteText(huge, "\x89PNG\r\n\x1a\n")) {
		return;
	}
	fs::resize_file(huge, (uint64_t {32} << 20u) + 1, ec); // sparse
	if (!Check(!ec, "make the 32 MiB + 1 icon") ||
	    !KytyUi::WriteText(games / "Broken Icon" / "sce_sys" / "icon0.png",
	                       std::string("\x89PNG\r\n\x1a\n", 8) + "not really a PNG image")) {
		return;
	}
	KytyUi::RunWithLibrary(ui, [&cases](KytyApplication* application, KytyMainWindow* window) {
		auto& library = KytyUi::ContextOf(application).library;
		KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
		for (const bool grid: {false, true}) {
			if (!KytyUi::ShowGridView(application, window, grid)) {
				return;
			}
			const char* view = grid ? "grid" : "list";
			for (const auto& game: cases) {
				KytyGame*  item = KytyUi::GameByTitle(library, game.title);
				GtkWidget* cell = nullptr;
				if (grid) {
					cell = KytyUi::GridTile(KytyUi::GridViewOf(window), game.title);
				} else if (const auto cells =
				               KytyUi::RowCells(KytyUi::ListViewOf(window), game.title);
				           cells.size() == 8) {
					cell = cells[0];
					Check(Texts(cells) == ExpectedRow(item),
					      std::string(game.title) + ": the rest of the row is as usual");
				}
				Check(item != nullptr && kyty_game_get_texture(item) == nullptr &&
				          cell != nullptr && kyty_game_cell_get_placeholder(KYTY_GAME_CELL(cell)),
				      std::string(view) + ": " + game.title + " shows the placeholder icon");
			}
			GtkWidget* alpha = nullptr;
			if (grid) {
				alpha = KytyUi::GridTile(KytyUi::GridViewOf(window), kAlphaTitle);
			} else if (const auto cells = KytyUi::RowCells(KytyUi::ListViewOf(window), kAlphaTitle);
			           !cells.empty()) {
				alpha = cells[0];
			}
			Check(alpha != nullptr &&
			          kyty_game_cell_get_texture(KYTY_GAME_CELL(alpha)) ==
			              kyty_game_get_texture(KytyUi::GameByTitle(library, kAlphaTitle)) &&
			          !kyty_game_cell_get_placeholder(KYTY_GAME_CELL(alpha)),
			      std::string(view) + ": Alpha Game shows its icon (scaled once to at most " +
			          std::to_string(kyty::gtk::kLargestGameIconSize) + " px)");
		}
		GdkTexture* icon = kyty_game_get_texture(KytyUi::GameByTitle(library, kAlphaTitle));
		Check(icon != nullptr && gdk_texture_get_width(icon) == kyty::gtk::kLargestGameIconSize,
		      "Alpha Game's 512 px icon is kept at 180 px");
	});
}

// Requirement 7.9: a game whose param.json is unusable shows its directory or archive name, empty
// metadata, and the metadata-unavailable emblem; the emblems sit at the icon's corner (W4-07).
KYTY_UI_CASE(library_metadata_unavailable) {
	KytyUi::UiContext ui;
	const auto        games = KytyUi::PrepareLibrary(ui);
	if (games.empty() ||
	    !KytyUi::WriteGameWithParam(games / "Broken Archive", "{\"titleName\": [") ||
	    KytyUi::PackGame(games / "Broken Archive").empty()) {
		return;
	}
	const KytyUi::FakeEmulatorBehavior emulator("--stdout ready --wait");
	KytyUi::RunWithLibrary(ui, [](KytyApplication* application, KytyMainWindow* window) {
		auto& library = KytyUi::ContextOf(application).library;
		for (const char* title: {"Broken Metadata", "Broken Archive"}) {
			KytyGame* game = KytyUi::GameByTitle(library, title);
			if (!Check(game != nullptr, std::string(title) + " is listed by its file name")) {
				continue;
			}
			const auto& row = kyty_game_get_row(game);
			Check(kyty_game_get_metadata_unavailable(game) && row.title_id.empty() &&
			          row.game_version.empty() && row.firmware_version.empty(),
			      std::string(title) + ": no metadata");
			Check(kyty::gtk::GameTooltip(game).find("param.json") != std::string::npos,
			      std::string(title) + ": the tooltip says so");
			const auto cells = KytyUi::RowCells(KytyUi::ListViewOf(window), title);
			if (Check(cells.size() == 8, std::string(title) + " has a row")) {
				Check(CellText(cells[1]).empty() && CellText(cells[2]) == kDash &&
				          CellText(cells[3]) == kDash,
				      std::string(title) + ": empty serial, dashes for the versions");
				GtkWidget* emblem = kyty_game_cell_get_metadata_emblem(KYTY_GAME_CELL(cells[0]));
				Check(emblem != nullptr &&
				          KytyUi::AccessibleName(emblem) == "The metadata is unavailable",
				      std::string(title) + ": the emblem has an accessible name");
				EmblemOnCorner(cells[0], true, 12.0, std::string(title) + " in the list");
			}
		}
		// A running game in the grid as well.
		KytyUi::SelectTitle(library, kBetaTitle);
		g_action_group_activate_action(G_ACTION_GROUP(kyty::gtk::MainWindowGameActions(window)),
		                               "run", nullptr);
		KytyUi::WaitFor([window]() { return kyty::gtk::MainWindowGameRunning(window); }, 5000,
		                "Beta Quest to run");
		if (KytyUi::ShowGridView(application, window, true)) {
			GtkGridView* grid = KytyUi::GridViewOf(window);
			for (const char* title: {"Broken Metadata", "Broken Archive"}) {
				if (GtkWidget* tile = KytyUi::GridTile(grid, title);
				    Check(tile != nullptr, std::string(title) + " has a tile")) {
					EmblemOnCorner(tile, true, 12.0, std::string(title) + " in the grid");
				}
			}
			if (GtkWidget* tile = KytyUi::GridTile(grid, kBetaTitle);
			    Check(tile != nullptr, "Beta Quest has a tile")) {
				// The texture fills the icon square: the emblem stays inside it, in its corner.
				EmblemOnCorner(tile, false, 16.0, "running Beta Quest in the grid");
			}
			ShowSelected(window, library, kAlphaTitle);
			Check(KytyUi::Capture(GTK_WINDOW(window), "grid-emblems"),
			      "the grid with the emblems renders");
			KytyUi::ShowGridView(application, window, false);
		}
		g_action_group_activate_action(G_ACTION_GROUP(kyty::gtk::MainWindowGameActions(window)),
		                               "stop", nullptr);
		KytyUi::WaitFor([window]() { return !kyty::gtk::MainWindowGameRunning(window); }, 8000,
		                "the game to stop");
	});
}

// --local (LFI-21, W4-06, plan D26): the Status drop-downs fit their longest status; a game
// without a title ID gets a dimmed comment entry that takes no input.
KYTY_UI_CASE(library_local_edit) {
	KytyUi::UiContext ui;
	if (KytyUi::PrepareLibrary(ui).empty() ||
	    !KytyUi::WriteText(ui.WorkingDirectory() / core::kLocalCompatibilityFileName,
	                       "{\"PPSA01001\": {\"status\": \"DoesntBoot\", \"comment\": \"Black "
	                       "screen\"}}")) {
		return;
	}
	const fs::path  previous = fs::current_path();
	std::error_code ec;
	fs::current_path(ui.WorkingDirectory(), ec);
	{
		KytyUi::OwnMainWindow session(ui, ui.Services().Get(), true);
		if (session.Ok() && KytyUi::WaitForLibrary(session.Window())) {
			KytyMainWindow* window = session.Window();
			GtkColumnView*  view   = KytyUi::ListViewOf(window);
			KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
			// The widest status as a drop-down shows it: each status alone in a drop-down inside
			// a window that is never shown (without a root, a drop-down's button has no item).
			int widest = 0;
			for (const auto status:
			     {core::CompatibilityStatus::Unknown, core::CompatibilityStatus::MainMenu,
			      core::CompatibilityStatus::InGame, core::CompatibilityStatus::Logo,
			      core::CompatibilityStatus::DoesntBoot}) {
				const char* texts[] = {kyty::gtk::StatusText(status), nullptr};
				GtkWidget*  holder  = gtk_window_new();
				GtkWidget*  probe   = gtk_drop_down_new_from_strings(texts);
				gtk_window_set_child(GTK_WINDOW(holder), probe);
				int natural = 0;
				gtk_widget_measure(probe, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural,
				                   nullptr, nullptr);
				gtk_window_destroy(GTK_WINDOW(holder));
				widest = std::max(widest, natural);
			}
			const auto dropdowns = KytyUi::FindAll(GTK_WIDGET(view), [](GtkWidget* widget) {
				return GTK_IS_DROP_DOWN(widget) && gtk_widget_get_mapped(widget) != FALSE;
			});
			Check(dropdowns.size() == 5, "every game has a status drop-down");
			// Alpha Game's drop-down shows "Doesn't boot" (the local file): every other one is as
			// wide as it.
			const auto alpha_status = KytyUi::RowCells(view, kAlphaTitle);
			GtkWidget* alpha_dropdown =
			    alpha_status.size() == 8
			        ? KytyUi::FindFirst(alpha_status[6],
			                            [](GtkWidget* widget) { return GTK_IS_DROP_DOWN(widget); })
			        : nullptr;
			if (Check(alpha_dropdown != nullptr &&
			              gtk_drop_down_get_selected(GTK_DROP_DOWN(alpha_dropdown)) == 4,
			          "Alpha Game's status drop-down shows \"Doesn't boot\"")) {
				int natural = 0;
				gtk_widget_measure(alpha_dropdown, GTK_ORIENTATION_HORIZONTAL, -1, nullptr,
				                   &natural, nullptr, nullptr);
				Check(gtk_widget_get_width(alpha_dropdown) >= natural,
				      "Alpha Game's status drop-down has the width it needs");
				for (GtkWidget* dropdown: dropdowns) {
					Check(gtk_widget_get_width(dropdown) == gtk_widget_get_width(alpha_dropdown),
					      "a status drop-down is " +
					          std::to_string(gtk_widget_get_width(dropdown)) +
					          " px wide, Alpha Game's (\"Doesn't boot\") " +
					          std::to_string(gtk_widget_get_width(alpha_dropdown)));
				}
			}
			for (GtkWidget* dropdown: dropdowns) {
				Check(gtk_widget_get_width(dropdown) >= widest,
				      "a status drop-down is " + std::to_string(gtk_widget_get_width(dropdown)) +
				          " px wide, the widest status needs " + std::to_string(widest));
				GtkWidget*            cell = gtk_widget_get_parent(gtk_widget_get_parent(dropdown));
				const graphene_rect_t bounds = BoundsIn(dropdown, cell);
				Check(bounds.origin.x + bounds.size.width <=
				          static_cast<float>(gtk_widget_get_width(cell)) + 0.5F,
				      "the drop-down is not clipped by its column");
			}
			const auto alpha  = KytyUi::RowCells(view, kAlphaTitle);
			const auto broken = KytyUi::RowCells(view, "Broken Metadata");
			if (Check(alpha.size() == 8 && broken.size() == 8 && GTK_IS_ENTRY(alpha[7]) &&
			              GTK_IS_ENTRY(broken[7]),
			          "the Comments cells are entries")) {
				Check(gtk_widget_get_sensitive(alpha[7]) != FALSE &&
				          gtk_editable_get_editable(GTK_EDITABLE(alpha[7])) != FALSE &&
				          std::string(gtk_editable_get_text(GTK_EDITABLE(alpha[7]))) ==
				              "Black screen",
				      "Alpha Game's comment can be edited");
				Check(gtk_widget_get_sensitive(broken[7]) == FALSE &&
				          gtk_editable_get_editable(GTK_EDITABLE(broken[7])) == FALSE,
				      "the comment of the game without a title ID is dimmed and takes no input");
			}
			// The capture shows the Status and Comments columns: the list scrolled to its end.
			if (GtkWidget* scrolled =
			        gtk_widget_get_ancestor(GTK_WIDGET(view), GTK_TYPE_SCROLLED_WINDOW);
			    Check(scrolled != nullptr, "the list is in a scrolled window")) {
				GtkAdjustment* h =
				    gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
				gtk_adjustment_set_value(h, gtk_adjustment_get_upper(h) -
				                                gtk_adjustment_get_page_size(h));
				KytyUi::WaitForPaintedFrame(GTK_WIDGET(window));
			}
			Check(KytyUi::Capture(GTK_WINDOW(window), "local"), "the --local list renders");
		}
	}
	fs::current_path(previous, ec);
}
