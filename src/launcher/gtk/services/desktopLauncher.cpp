#include "launcher/gtk/services/desktopLauncher.h"

#include "launcher/gtk/gobjectPtr.h"

#include <utility>

namespace kyty::gtk {

namespace {

// The user dismissed a chooser of the desktop, or the caller's cancellable was cancelled.
bool IsDismissed(const GErrorPtr& error) {
	return g_error_matches(error.get(), GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
	       g_error_matches(error.get(), GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
	       g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

// One launch: the callback, and what decides whether it may still run.
struct Pending {
	std::shared_ptr<bool>    alive;
	DesktopLauncher::DoneFn  done;
	GObjectPtr<GCancellable> cancellable;

	void Finish(bool ok, const GErrorPtr& error) {
		if (!*alive || (cancellable && g_cancellable_is_cancelled(cancellable.get()) != FALSE)) {
			return;
		}
		done(ok || IsDismissed(error) ? std::string() : std::string(error.Message()));
	}
};

class GtkDesktopLauncher final: public DesktopLauncher {
public:
	~GtkDesktopLauncher() override { *m_alive = false; }

	void OpenFolder(GtkWindow* parent, const std::filesystem::path& folder, DoneFn done,
	                GCancellable* cancellable) override {
		auto launcher = NewFileLauncher(folder);
		gtk_file_launcher_launch(launcher.get(), parent, cancellable, &OnLaunched,
		                         NewPending(std::move(done), cancellable));
	}

	void RevealFile(GtkWindow* parent, const std::filesystem::path& file, DoneFn done,
	                GCancellable* cancellable) override {
		auto launcher = NewFileLauncher(file);
		gtk_file_launcher_open_containing_folder(launcher.get(), parent, cancellable, &OnRevealed,
		                                         NewPending(std::move(done), cancellable));
	}

	void OpenUri(GtkWindow* parent, const std::string& uri, DoneFn done,
	             GCancellable* cancellable) override {
		auto launcher = GObjectPtr<GtkUriLauncher>::Adopt(gtk_uri_launcher_new(uri.c_str()));
		gtk_uri_launcher_launch(launcher.get(), parent, cancellable, &OnUriLaunched,
		                        NewPending(std::move(done), cancellable));
	}

private:
	static GObjectPtr<GtkFileLauncher> NewFileLauncher(const std::filesystem::path& path) {
		auto file = GObjectPtr<GFile>::Adopt(g_file_new_for_path(path.c_str()));
		return GObjectPtr<GtkFileLauncher>::Adopt(gtk_file_launcher_new(file.get()));
	}

	Pending* NewPending(DoneFn done, GCancellable* cancellable) const {
		return new Pending {m_alive, std::move(done), GObjectPtr<GCancellable>::Ref(cancellable)};
	}

	static void OnLaunched(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
		GErrorPtr                error;
		const bool ok = gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result,
		                                                error.Out()) != FALSE;
		pending->Finish(ok, error);
	}

	static void OnRevealed(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
		GErrorPtr                error;
		const bool               ok = gtk_file_launcher_open_containing_folder_finish(
		                                  GTK_FILE_LAUNCHER(source), result, error.Out()) != FALSE;
		pending->Finish(ok, error);
	}

	static void OnUriLaunched(GObject* source, GAsyncResult* result, gpointer data) {
		std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
		GErrorPtr                error;
		const bool               ok =
		    gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(source), result, error.Out()) != FALSE;
		pending->Finish(ok, error);
	}

	std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace

std::unique_ptr<DesktopLauncher> MakeGtkDesktopLauncher() {
	return std::make_unique<GtkDesktopLauncher>();
}

} // namespace kyty::gtk
