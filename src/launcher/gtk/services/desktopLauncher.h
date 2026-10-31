#pragma once

// Handing folders, files, and links to the desktop (task 19.4; plan D25; Requirement 10.10): an
// interface that the windows call through LauncherContext::services, so that the visual harness
// and the UI tests can replace it. The GTK implementation uses GtkFileLauncher and GtkUriLauncher,
// which go through the OpenURI portal when one is running and through the desktop's default
// applications otherwise.
//
// Every call is asynchronous and calls done exactly once from the main loop with "" on success (or
// when the user dismissed a chooser of the desktop) or the error message, which the caller shows,
// unless the cancellable (may be null) is cancelled first or the launcher is destroyed first; then
// done is never called.

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <string>

namespace kyty::gtk {

class DesktopLauncher {
public:
	using DoneFn = std::function<void(const std::string& error)>;

	virtual ~DesktopLauncher() = default;

	// Opens folder in the default file manager ("Open folder" of a directory Game_Entry).
	virtual void OpenFolder(GtkWindow* parent, const std::filesystem::path& folder, DoneFn done,
	                        GCancellable* cancellable) = 0;
	// Opens the folder that contains file with the file selected where the file manager can
	// (gtk_file_launcher_open_containing_folder; "Open folder" of a .zar Game_Entry).
	virtual void RevealFile(GtkWindow* parent, const std::filesystem::path& file, DoneFn done,
	                        GCancellable* cancellable) = 0;
	// Opens uri in the default application for its scheme (the releases page, the cheat
	// repository).
	virtual void OpenUri(GtkWindow* parent, const std::string& uri, DoneFn done,
	                     GCancellable* cancellable) = 0;
};

// The GtkFileLauncher and GtkUriLauncher implementation.
std::unique_ptr<DesktopLauncher> MakeGtkDesktopLauncher();

} // namespace kyty::gtk
