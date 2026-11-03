#pragma once

// The registry of every window and dialog type of the GTK launcher (task 19.6, Requirement 17.10;
// plan D17), with an open and a close function each. The UI tests and the visual harness open
// every type through it; each unit registers the types it adds. Every registered window is a
// GtkWindow (AdwWindow, transient for the parent); the alert types are GtkAlertDialogs, whose
// handle carries the GCancellable that their close function cancels, and the save data failure
// list (an AdwWindow of alerts.h ShowErrorList, closed with its Close button).

#include "launcher/core/gameMetadata.h"
#include "launcher/gtk/launcherContext.h"

#include <gtk/gtk.h>
#include <span>
#include <string_view>

namespace kyty::gtk {

struct DialogArgs {
	// The game a per-game window opens for; nullptr for the others.
	const core::GameEntry* entry = nullptr;
};

struct DialogHandle {
	GtkWindow*    window      = nullptr; // the toplevel the open function created (not owned)
	GCancellable* cancellable = nullptr; // owned by the handle until close; alerts only
};

struct DialogType {
	std::string_view id;
	DialogHandle (*open)(LauncherContext& context, GtkWindow* parent, const DialogArgs& args);
	// Closes what open created and empties the handle.
	void (*close)(DialogHandle& handle);
};

// Every registered type, in registration order; ids are unique.
std::span<const DialogType> DialogTypes();

// The type with this id, or nullptr.
const DialogType* FindDialogType(std::string_view id);

} // namespace kyty::gtk
