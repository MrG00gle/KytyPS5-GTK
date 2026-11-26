#pragma once

// The additional content window of a game (task 21.5; HC-30; Requirement 11.3; LFI-28), modal for
// its parent, from resources/ui/additional-content-window.xml, ported from the Qt launcher's
// AdditionalContentDialog (src/launcher/src/additionalContentDialog.cpp). It lists, in a GTask
// thread, the add-ons of the game's title ID in the global add-ons folder
// (core::LoadAdditionalContent(settings.addons_dir, entry.title_id)): each with its icon (the
// add-on's sce_sys/icon0.png, else the game's icon), its title (or its entitlement label), and its
// content ID as the tooltip. After them come the game's user add-ons (upstream KytyPS5's launcher,
// commit 66e21a62; settings.user_addons of the normalized title ID): each with the game's icon,
// its entitlement label, "License only", "Data: <folder>", or "Data folder not found: <folder>",
// and a Remove button. The status line gives the number of add-ons ("1 add-on", "2 add-ons") or
// says that the add-ons folder is not set, does not exist, or has no add-on for the game (the
// launcher's translated texts, D46). Below the list, "Add an add-on" takes a label or content ID
// and a data folder (typed, or chosen through FileChooser::SelectFolders; empty for a license
// only), checks them with core::MakeUserAddOn (an error goes to the status line and marks the
// row), and saves the game's list through SaveSettingsInteractively; Remove saves the list
// without the entry. A game without a title ID cannot have user add-ons (the group is off).
// Refresh lists again (a focused row gives the focus to Refresh before the rows go,
// focusSupport.h; phase 7 D29); Open folder (enabled while the folder exists) opens the add-ons
// folder through DesktopLauncher::OpenFolder and shows an error when that fails. Escape and Close
// close the window.

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
