#pragma once

// The compression dialog of a directory Game_Entry (task 21.5; plan D44; HC-31 dialog part;
// Requirements 11.8-11.11, 11.16; LFI-29), modal for its parent, from
// resources/ui/compression-dialog.xml, ported from the Qt launcher's compress_game
// (src/launcher/src/configurationListWidget.cpp). It packs the game directory into
// "<directory>.zar" (core::ArchiveJob):
// - core::ArchiveJob::Prepare first: when anything exists at the archive path, the dialog shows
//   "<archive> already exists." and starts nothing (11.9);
// - then ArchiveJob::Run in a GTask thread; the percentages it reports (never decreasing, 11.8)
//   reach the progress bar through idle sources of the main context. Cancel, Escape, and closing
//   the dialog set the job's core::CancelToken; the dialog closes once the job has stopped and
//   deleted the partial archive, and shows nothing (11.10);
// - Failed and VerifyFailed show "Could not compress <directory>." (Failed adds the job's error
//   when it says more, 11.16);
// - Completed asks whether to delete the original directory, keeping it being the default (an
//   alert whose default and Escape button is "Keep folder", 11.11); "Delete folder" removes it.
//   Then the dialog emits "archive-created" (the caller rescans the Game_Folders so the archive
//   is listed) and closes, or shows the error when the directory could not be removed.
//
// The job is registered in LauncherContext::jobs while its thread runs, also after the dialog went
// away (destroyed with its parent), so that app.quit and the application's shutdown cancel it and
// wait until it has stopped and deleted its partial archive (phase 7 plan D29, p6-u9-2).
//
// Signal "archive-created" (no arguments): after a Completed job, once the user answered the
// delete question.

#include "launcher/core/archiveJob.h"
#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_COMPRESSION_DIALOG (kyty_compression_dialog_get_type())
G_DECLARE_FINAL_TYPE(KytyCompressionDialog, kyty_compression_dialog, KYTY, COMPRESSION_DIALOG,
                     AdwWindow)

G_END_DECLS

// The compression of entry.path (entry is copied; a directory entry), transient for and modal over
// parent (may be nullptr). It prepares the job and starts packing at once. packer replaces
// Common::PackArchive (for the harness and the UI tests; empty in the launcher). Not presented
// yet; GTK owns the window. context must outlive it.
KytyCompressionDialog* kyty_compression_dialog_new(kyty::gtk::LauncherContext&  context,
                                                   GtkWindow*                   parent,
                                                   const kyty::core::GameEntry& entry,
                                                   kyty::core::ArchivePacker    packer = {});

// The job's outcome so far (Pending while it runs or when Prepare refused).
kyty::core::ArchiveOutcome kyty_compression_dialog_get_outcome(KytyCompressionDialog* dialog);
