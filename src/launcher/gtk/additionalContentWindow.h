#pragma once

// The additional content window of a game (task 21.5; HC-30; Requirement 11.3; LFI-28), modal for
// its parent, from resources/ui/additional-content-window.xml, ported from the Qt launcher's
// AdditionalContentDialog (src/launcher/src/additionalContentDialog.cpp). It lists, in a GTask
// thread, the add-ons of the game's title ID in the global add-ons folder
// (core::LoadAdditionalContent(settings.addons_dir, entry.title_id)): each with its icon (the
// add-on's sce_sys/icon0.png, else the game's icon), its title (or its entitlement label), and its
// content ID as the tooltip. The status line gives the number of add-ons ("1 installed add-on",
// "2 installed add-ons") or says that the add-ons folder is not set, does not exist, or has no
// add-on for the game (the launcher's translated texts, D46). Refresh lists again; Open folder
// (enabled while the folder exists) opens the add-ons folder through DesktopLauncher::OpenFolder
// and shows an error when that fails. Escape and Close close the window.

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define KYTY_TYPE_ADDITIONAL_CONTENT_WINDOW (kyty_additional_content_window_get_type())
G_DECLARE_FINAL_TYPE(KytyAdditionalContentWindow, kyty_additional_content_window, KYTY,
                     ADDITIONAL_CONTENT_WINDOW, AdwWindow)

G_END_DECLS

// The add-ons of entry (copied), transient for and modal over parent (may be nullptr). It starts
// listing at once. Not presented yet; GTK owns the window. context must outlive it.
KytyAdditionalContentWindow* kyty_additional_content_window_new(kyty::gtk::LauncherContext& context,
                                                                GtkWindow*                  parent,
                                                                const kyty::core::GameEntry& entry);

// True while no listing is pending (for the harness and the UI tests).
bool kyty_additional_content_window_is_loaded(KytyAdditionalContentWindow* window);
