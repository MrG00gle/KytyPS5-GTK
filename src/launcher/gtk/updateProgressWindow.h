#pragma once

// The progress window of an update installation (task 21.6; plan D45, G2; HC-34; Requirements
// 13.5, 13.8; LFI-35), modal for its parent, from resources/ui/update-progress-window.xml, ported
// from the QProgressDialog of the Qt launcher's UpdateChecker::Download
// (src/launcher/src/updateChecker.cpp):
// - "Downloading <tag>…" with a progress bar of the received bytes over the package size;
// - "Preparing the update…" with an indeterminate spinner while tar extracts the package and the
//   installed kyty_updater validates it (the update session runs both processes synchronously, so
//   the main loop may pause meanwhile, G2);
// - "Restarting to install the update…" without Cancel once the helper took over (the launcher
//   quits at once).
// Cancel, Escape, and closing the window emit "cancel-requested" while Cancel is offered. The
// window never closes by itself: its owner (updateUi.h) destroys it when the session ends.
//
// Signal "cancel-requested" (no arguments).

#include <adwaita.h>
#include <cstdint>
#include <string_view>

G_BEGIN_DECLS

#define KYTY_TYPE_UPDATE_PROGRESS_WINDOW (kyty_update_progress_window_get_type())
G_DECLARE_FINAL_TYPE(KytyUpdateProgressWindow, kyty_update_progress_window, KYTY,
                     UPDATE_PROGRESS_WINDOW, AdwWindow)

G_END_DECLS

// The download of the release tag at 0 bytes, transient for and modal over parent (may be
// nullptr). Not presented yet; GTK owns the window.
KytyUpdateProgressWindow* kyty_update_progress_window_new(GtkWindow* parent, std::string_view tag);

// received of total bytes are downloaded (total 0 leaves the bar empty).
void kyty_update_progress_window_set_downloaded(KytyUpdateProgressWindow* window, uint64_t received,
                                                uint64_t total);

// The package is extracted and validated: the spinner instead of the bar, Cancel still offered.
void kyty_update_progress_window_set_preparing(KytyUpdateProgressWindow* window);

// The helper took over: no Cancel any more.
void kyty_update_progress_window_set_restarting(KytyUpdateProgressWindow* window);
