#pragma once

// The model of the log view (task 20.5; plan D35, HC-26; Requirements 10.1, 10.5, 10.7, 10.8):
// KytyLogModel, a GListModel of GtkStringObject over the lines of the core::LogCollector it owns.
// - The model shows the lines that the collector held at the last sync; lines that arrive later
//   wait for the next one. kyty_log_model_schedule_sync starts a 100 ms timer (at most one), which
//   syncs once: the run controller calls it whenever the Emulator wrote bytes, so the timer runs
//   only while lines are pending and a line is shown at most about 100 ms after it reached the
//   collector (Requirement 10.5 asks for 1 s).
// - A sync takes the new lines (core::LogCollector::TakeNewLines) and emits at most two
//   items-changed signals: one that removes max(0, size + new - capacity) rows from the head (the
//   lines the collector dropped, phase 5 D16) and one that appends the new rows.
// - Items are created on demand (a GtkStringObject per get_item); the text is the collector's
//   line, where every ill-formed UTF-8 sequence is already U+FFFD. A row whose line the collector
//   dropped after the last sync (only until the next one) reads as "".
// - kyty_log_model_clear empties the collector and the model (a new run, Requirement 10.1).
// - kyty_log_model_append_line adds a line of the launcher itself (an abnormal exit, a warning
//   before a run) and syncs at once. Call it only while no Emulator output is pending in the
//   collector (before the spawn, after the exit): it goes through the standard output accumulator.
// - kyty_log_model_save writes core::LogCollector::ToText() after a sync, so the file holds exactly
//   the rows of the view, one line each (Requirement 10.8), through core::WriteFileAtomically
//   (Requirement 10.13: a failure leaves the file and the log as they were).
// Main thread only.

#include "launcher/core/atomicFile.h"
#include "launcher/core/expected.h"
#include "launcher/core/logBuffer.h"

#include <cstddef>
#include <filesystem>
#include <glib-object.h>
#include <string_view>

G_BEGIN_DECLS

#define KYTY_TYPE_LOG_MODEL (kyty_log_model_get_type())
G_DECLARE_FINAL_TYPE(KytyLogModel, kyty_log_model, KYTY, LOG_MODEL, GObject)

G_END_DECLS

// A new, empty model (transfer full) whose collector keeps retained_lines lines (0 counts as 1).
KytyLogModel*
kyty_log_model_new(std::size_t retained_lines = kyty::core::LogCollector::kDefaultRetainedLines);

// The collector the Emulator's output goes to (the ProcessWatcher's), owned by the model.
kyty::core::LogCollector& kyty_log_model_get_collector(KytyLogModel* model);

// A sync 100 ms from now, unless one is already scheduled.
void kyty_log_model_schedule_sync(KytyLogModel* model);
// Shows the collector's new lines now (and cancels a scheduled sync).
void kyty_log_model_sync(KytyLogModel* model);
// Empties the collector and the model.
void kyty_log_model_clear(KytyLogModel* model);
// Adds line (without '\n') as a line of the log and syncs.
void kyty_log_model_append_line(KytyLogModel* model, std::string_view line);
// Syncs, then writes every row followed by '\n' to path.
kyty::core::Expected<void, kyty::core::IoError>
kyty_log_model_save(KytyLogModel* model, const std::filesystem::path& path);
