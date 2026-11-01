#pragma once

// The cheats window of a game (task 21.4; HC-33; Requirements 11.4-11.6, 11.17; plan D43; LFI-27),
// modal for its parent, from resources/ui/cheats-window.xml, ported from the Qt launcher's
// PatchesDialog (src/launcher/src/patchesDialog.cpp). The main window offers it only for title IDs
// that core::IsCheatTitleId accepts.
//
// Remote view: the credit link (core::kHenCheatsRepository through DesktopLauncher::OpenUri), the
// HEN Cheats Collection files of the game's title ID (core::CheatCatalog, the installed version's
// files first through core::PutInstalledVersionFirst) with version, format, and file name, MC4 and
// SHN files disabled with "<title> — <FORMAT> is not supported." as the tooltip, and the status
// line (the number of files, or none, and the indexes that could not be loaded with "Use Refresh
// to retry."). Selecting a JSON file previews it (core::RemoteCheats::Fetch with the game's title
// ID, version, and executable): its cheats, name, credits, and version, and why it cannot be
// imported when it does not download, parse, or match the game; Import to Local is enabled only
// for an importable file and again disabled after a failure. Refresh cancels a pending preview and
// loads the list again. Import to Local (core::ImportRemoteCheats: _Patches/<TITLE_ID>.json with
// every cheat off) is refused while the local file cannot be read, asks "Replace local cheats?"
// when a local file exists, and, when the local file appeared or changed meanwhile, reads it again
// and asks again; then it shows the Local view.
//
// Local view: the cheats of the local cheat file (core::LoadLocalCheats) with check boxes, the
// status line (no file, cannot be read, not valid with the parse error, or the number of cheats),
// Reload, Apply selection (core::SaveIfUnchanged of the checked states against the bytes that were
// read; "Cheat file changed; reload the dialog." when the file changed on disk; then it reads the
// file again), and Enable all and Disable all, which change only the check boxes. The local file is
// read and written on the main thread (at most core::kMaxCheatFileBytes), as in Qt.
//
// Escape and Close close the window.

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_CHEATS_WINDOW (kyty_cheats_window_get_type())
G_DECLARE_FINAL_TYPE(KytyCheatsWindow, kyty_cheats_window, KYTY, CHEATS_WINDOW, AdwWindow)

G_END_DECLS

// The cheats of entry (copied), transient for and modal over parent (may be nullptr). It reads the
// local cheat file and starts loading the remote list at once through context.services.http. Not
// presented yet; GTK owns the window. context must outlive it.
KytyCheatsWindow* kyty_cheats_window_new(kyty::gtk::LauncherContext& context, GtkWindow* parent,
                                         const kyty::core::GameEntry& entry);
