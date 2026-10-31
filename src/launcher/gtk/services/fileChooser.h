#pragma once

// File and folder selection (task 19.4; plan D25): an interface that the windows call through
// LauncherContext::services, so that the visual harness and the UI tests can replace it. The GTK
// implementation uses GtkFileDialog, which goes through the FileChooser portal when one is running
// and shows GTK's own dialog otherwise.
//
// Every call is asynchronous and calls done exactly once from the main loop, unless the cancellable
// (may be null) is cancelled first, which also closes the dialog, or the chooser is destroyed
// first; then done is never called. The user closing the dialog without a choice is an empty result
// with an empty error. A failure of the dialog itself (for example a portal error) or a choice that
// is not a local file is an empty result with an error message, which the caller shows.

#include <filesystem>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kyty::gtk {

// One entry of the file type list of OpenFile: a name and glob patterns ("*.json").
struct FileFilter {
	std::string              name;
	std::vector<std::string> patterns;
};

class FileChooser {
public:
	// The chosen folders (empty when none), and an error message ("" unless the dialog failed).
	using FoldersFn =
	    std::function<void(std::vector<std::filesystem::path> folders, const std::string& error)>;
	// The chosen file (nullopt when none), and an error message ("" unless the dialog failed).
	using FileFn =
	    std::function<void(std::optional<std::filesystem::path> file, const std::string& error)>;

	virtual ~FileChooser() = default;

	// One or more folders (Game_Folders, the add-ons directory).
	virtual void SelectFolders(GtkWindow* parent, const std::string& title, FoldersFn done,
	                           GCancellable* cancellable) = 0;
	// An existing file, offered with filters (in order; none shows every file).
	virtual void OpenFile(GtkWindow* parent, const std::string& title,
	                      const std::vector<FileFilter>& filters, FileFn done,
	                      GCancellable* cancellable) = 0;
	// A file to write, with suggested_name as the initial name.
	virtual void SaveFile(GtkWindow* parent, const std::string& title,
	                      const std::string& suggested_name, FileFn done,
	                      GCancellable* cancellable) = 0;
};

// The GtkFileDialog implementation (modal for parent).
std::unique_ptr<FileChooser> MakeGtkFileChooser();

} // namespace kyty::gtk
